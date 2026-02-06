// OS Assignment - dice game - server.c (FIXED FOR 3-5 PLAYERS)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

// main setting 
#define MIN_P 3
#define MAX_P 5  // Support 3-5 players
#define WC 20
#define FIFO_PREFIX "/tmp/player_"
#define LOG_FIFO "/tmp/dice_game_log_fifo"
#define gamel "game.log"
#define scoresf "scores.txt"

typedef struct LogNode {
    char message[256];
    struct LogNode *next;
} LogNode;

typedef struct {
    LogNode *head;
    LogNode *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} LogQueue;

LogQueue log_queue;

typedef struct 
{
    int PP[MAX_P];
    int CT;
    int game_active;
    int CP;
    int player_active[MAX_P];
    int FW;
    int round;
    char PN[MAX_P][50];
    int winner[MAX_P];
    pthread_mutex_t shm_lock; 
    pthread_mutex_t table_sync; 
} GameInfo;

GameInfo *gptr = NULL;
int shm_fd;
pthread_t log_t, sch_t; // log_t = logger thread, sch_t = scheduler thread
volatile sig_atomic_t sr = 1; // sr = server running
int log_fifo_fd = -1; // FIFO for cross-process logging

// pre-declaration of functions
void ssm();
void csm(); // csm = cleanup shared memory
void *ltf(void *arg); // ltf = logger thread function
void *stf(void *arg); // stf = scheduler thread function    
void hp(int player_id);    // hp = handle client/player
void sigchld_handler(int sig);
void sigint_handler(int sig);
void log_message(const char *message);
void init();
void reset();
void ls(); // ls = load scores
void ss(); // ss = save scores

void ls() {
    FILE *f_ptr = fopen(scoresf, "r");
    
    if (f_ptr == NULL) {
        printf("[SERVER] No scores.txt found, players start with initial of 0\n");
        for (int i = 0; i < MAX_P; i++) {
            gptr->winner[i] = 0;
        }
        return; 
    }

    printf("[SERVER] History file found, loading data...\n");

    char text_line[256];
    int current_index = 0;

    while (fgets(text_line, sizeof(text_line), f_ptr) != NULL) {
        if (text_line[0] != '|') {
            continue; 
        }

        if (strstr(text_line, "Player Name") != NULL) {
            continue;
        }

        char *score_part = strrchr(text_line, ' '); 
        if (score_part != NULL && current_index < MAX_P) {
            int value = atoi(score_part);
            gptr->winner[current_index] = value;
            
            printf("   - Loaded Slot %d: %d wins\n", current_index + 1, value);
            current_index++;
        }
    }

    fclose(f_ptr);
    printf("[SERVER] Load complete. %d slots updated.\n", current_index);
}

void ss() 
{
    FILE *f = fopen(scoresf, "w");
    if (f) {
        fprintf(f, "======================================\n");
        fprintf(f, "|           Dice Game Results        |\n");
        fprintf(f, "======================================\n");
        fprintf(f, "| %-20s | %-10s |\n", "Player Name", "Score");
        fprintf(f, "======================================\n");

        for (int i = 0; i < MAX_P; i++) 
        {
            char display_name[50];
                        
            if (strlen(gptr->PN[i]) > 0) 
            {
                strncpy(display_name, gptr->PN[i], 49);
                display_name[49] = '\0';
            } else 
            {
                snprintf(display_name, sizeof(display_name), "Slot %d (Empty)", i + 1);
            }

            fprintf(f, "| %-20s | %-10d |\n", display_name, gptr->winner[i]);
        }
        
        fprintf(f, "======================================\n");
        fclose(f);
        printf("[SERVER] Scores saved to %s (Table Format)\n", scoresf);
    } else 
    {
        perror("Failed to save scores");
    }
}

int main() 
{
    printf("=== Dice Race Game Server - Started ===\n");
    printf("=== Server PID: %-24d ===\n", getpid());
    printf("Waiting for %d-%d players to connect...\n\n", MIN_P, MAX_P);
    
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    
    unlink(LOG_FIFO); 
    if (mkfifo(LOG_FIFO, 0666) == -1 && errno != EEXIST) {
        perror("Failed to create log FIFO");
        exit(EXIT_FAILURE);
    }
    
    ssm();
    init();
    
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gptr->shm_lock, &attr);
    pthread_mutex_init(&gptr->table_sync, &attr);
    pthread_mutexattr_destroy(&attr);

    log_queue.head = NULL;
    log_queue.tail = NULL;
    pthread_mutex_init(&log_queue.mutex, NULL);
    pthread_cond_init(&log_queue.cond, NULL);

    ls();
    
    printf("[Main] Creating logger thread...\n");
    int ret = pthread_create(&log_t, NULL, ltf, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to create logger thread: %s\n", strerror(ret));
        csm();
        exit(EXIT_FAILURE);
    }
    
    printf("[Main] Creating scheduler thread...\n");
    ret = pthread_create(&sch_t, NULL, stf, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to create scheduler thread: %s\n", strerror(ret));
        csm();
        exit(EXIT_FAILURE);
    }
    
    printf("\n[Main] Both threads created successfully!\n");
    printf("[Main] Thread count: 2 (logger + scheduler)\n\n");
    
    log_message("Server started - waiting for players");
    
    // Wait for minimum players, then start game when ready
    while (sr && gptr->CP < MAX_P) 
    {
        for (int i = 0; i < MAX_P; i++) 
        {
            if (!gptr->player_active[i]) 
            {
                char fifo_name[256];
                snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, i);
                
                if (access(fifo_name, F_OK) == 0) 
                {
                    pthread_mutex_lock(&gptr->shm_lock);
                    gptr->player_active[i] = 1;
                    gptr->CP++;
                    pthread_mutex_unlock(&gptr->shm_lock);
                    
                    printf("Player %d connected (%d/%d minimum, max %d)\n", 
                           i + 1, gptr->CP, MIN_P, MAX_P);
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Player %d connected", i + 1);
                    log_message(log_msg);
                    
                    pid_t pid = fork();
                    if (pid == 0) 
                    {
                        hp(i);
                        exit(0);
                    } else if (pid < 0) 
                    {
                        fprintf(stderr, "Fork failed for player %d: %s\n", i, strerror(errno));
                        pthread_mutex_lock(&gptr->shm_lock);
                        gptr->player_active[i] = 0;
                        gptr->CP--;
                        pthread_mutex_unlock(&gptr->shm_lock);
                    }
                }
            }
        }
        
        // Start game if minimum players reached and no new connections for a while
        if (gptr->CP >= MIN_P && !gptr->game_active) 
        {
            static int wait_counter = 0;
            wait_counter++;
            
            // Wait 5 seconds for more players, then start
            if (wait_counter >= 5) 
            {
                printf("=== Minimum players reached! Starting game with %d players ===\n", gptr->CP);
                
                pthread_mutex_lock(&gptr->shm_lock);
                gptr->game_active = 1;
                pthread_mutex_unlock(&gptr->shm_lock);
                
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), "Game started with %d players", gptr->CP);
                log_message(log_msg);
                
                printf("[Main] Active processes:\n");
                printf("  - Main server process (PID: %d)\n", getpid());
                printf("  - Logger thread (TID: %lu)\n", (unsigned long)log_t);
                printf("  - Scheduler thread (TID: %lu)\n", (unsigned long)sch_t);
                printf("  - %d child processes (forked for each player)\n", gptr->CP);
                printf("[Main] Total: 1 process + 2 threads + %d child processes\n\n", gptr->CP);
                
                break; // Exit connection loop
            }
        }
        
        sleep(1); 
    }
    
    // If max players reached before timeout
    if (gptr->CP == MAX_P && !gptr->game_active) 
    {
        printf("=== All players connected! Starting game ===\n");
        
        pthread_mutex_lock(&gptr->shm_lock);
        gptr->game_active = 1;
        pthread_mutex_unlock(&gptr->shm_lock);
        
        log_message("Game started - all players connected");
        
        printf("[Main] Active processes:\n");
        printf("  - Main server process (PID: %d)\n", getpid());
        printf("  - Logger thread (TID: %lu)\n", (unsigned long)log_t);
        printf("  - Scheduler thread (TID: %lu)\n", (unsigned long)sch_t);
        printf("  - %d child processes (forked for each player)\n", gptr->CP);
        printf("[Main] Total: 1 process + 2 threads + %d child processes\n\n", gptr->CP);
    }
    
    printf("[Main] Waiting for game to complete...\n");
    while (sr && gptr->game_active) 
    {
        sleep(1);
    }
    
    printf("\n[Main] Game ended. Joining threads...\n");  
    pthread_mutex_lock(&log_queue.mutex);
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);

    pthread_join(log_t, NULL);
    printf("[Main] Logger thread joined\n");
    pthread_join(sch_t, NULL);
    printf("[Main] Scheduler thread joined\n");
    
    printf("[Main] Cleaning up...\n");
    csm();

    printf("=== Server shutdown complete ===\n");
    return 0;
}

void ssm() 
{
    shm_fd = shm_open("/dice_game_shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) 
    {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    
    if (ftruncate(shm_fd, sizeof(GameInfo)) == -1) 
    {
        perror("ftruncate failed");
        close(shm_fd);
        shm_unlink("/dice_game_shm");
        exit(EXIT_FAILURE);
    }
    
    gptr = mmap(NULL, sizeof(GameInfo), PROT_READ | PROT_WRITE, 
                MAP_SHARED, shm_fd, 0);
    if (gptr == MAP_FAILED) 
    {
        perror("mmap failed");
        close(shm_fd);
        shm_unlink("/dice_game_shm");
        exit(EXIT_FAILURE);
    }
}

void csm() 
{
    if (gptr != NULL) 
    {
        pthread_mutex_destroy(&gptr->shm_lock);
        pthread_mutex_destroy(&gptr->table_sync);
        munmap(gptr, sizeof(GameInfo));
    }
    close(shm_fd);
    shm_unlink("/dice_game_shm");
    
    for (int i = 0; i < MAX_P; i++) 
    {
        char fifo_name[256];
        snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, i);
        unlink(fifo_name);
        snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, i);
        unlink(fifo_name);
    }
    
    unlink(LOG_FIFO);
    
    pthread_mutex_destroy(&log_queue.mutex);
    pthread_cond_destroy(&log_queue.cond);
}

void init() 
{
    memset(gptr, 0, sizeof(GameInfo));
    gptr->CT = 0;
    gptr->game_active = 0;
    gptr->CP = 0;
    gptr->FW = -1;
    gptr->round = 1;
    
    for (int i = 0; i < MAX_P; i++) 
    {
        gptr->PP[i] = 0;
        gptr->player_active[i] = 0;
        memset(gptr->PN[i], 0, 50);
    }
}

void reset() 
{
    pthread_mutex_lock(&gptr->shm_lock);
    gptr->CT = 0;
    gptr->FW = -1;
    gptr->round = 1;
    for (int i = 0; i < MAX_P; i++) 
    {
        gptr->PP[i] = 0;
    }
    pthread_mutex_unlock(&gptr->shm_lock);
}

void log_message(const char *message) 
{
    time_t now = time(NULL);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';
    
    char formatted_msg[512];
    snprintf(formatted_msg, sizeof(formatted_msg), "[%s] %s\n", time_str, message);
    
    pthread_mutex_lock(&gptr->shm_lock);
    
    FILE *log_file = fopen(gamel, "a");
    if (log_file) {
        fprintf(log_file, "%s", formatted_msg);
        fflush(log_file);
        fclose(log_file);
    }
    
    pthread_mutex_unlock(&gptr->shm_lock);
}

void *ltf(void *arg) 
{
    printf("[Logger Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    
    int fifo_fd = open(LOG_FIFO, O_RDONLY);
    if (fifo_fd == -1) {
        perror("Logger thread: Failed to open log FIFO");
        return NULL;
    }
    
    FILE *log_file = fopen(gamel, "a");
    if (!log_file) {
        perror("Logger thread: Failed to open log file");
        close(fifo_fd);
        return NULL;
    }
    
    char buffer[512];
    ssize_t bytes_read;
    
    printf("[Logger Thread] Listening for log messages...\n");
    
    while (sr) {
        bytes_read = read(fifo_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            fprintf(log_file, "%s", buffer);
            fflush(log_file); 
        } else if (bytes_read == 0) {
            usleep(10000); // 10ms
        } else {
            if (errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
    }
    
    fclose(log_file);
    close(fifo_fd);
    printf("[Logger Thread] Shutting down\n");
    return NULL;
}

void *stf(void *arg) 
{
    printf("[Scheduler Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    
    while (sr) 
    {
        if (gptr->game_active) 
        {
            pthread_mutex_lock(&gptr->shm_lock);
            
            // Check for winner
            for (int i = 0; i < MAX_P; i++) 
            {
                if (gptr->player_active[i] && gptr->PP[i] >= WC) 
                {
                    gptr->FW = i;
                    gptr->game_active = 0;
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Game Over! %s wins!", 
                             gptr->PN[i]);
                    pthread_mutex_unlock(&gptr->shm_lock);
                    log_message(log_msg);
                    printf("%s\n", log_msg);
                    
                    printf("[Scheduler Thread] Game ended, shutting down\n");
                    return NULL;
                }
            }
            
            // Advance turn to next active player (Round Robin with skip)
            int starting_turn = gptr->CT;
            int attempts = 0;
            
            while (attempts < MAX_P) {
                gptr->CT = (gptr->CT + 1) % MAX_P;
                
                // If we found an active player, break
                if (gptr->player_active[gptr->CT]) {
                    break;
                }
                
                attempts++;
            }
            
            // Update round counter when we cycle back to player 0
            if (gptr->CT == 0 && starting_turn != 0) {
                gptr->round++;
            }
            
            pthread_mutex_unlock(&gptr->shm_lock);
        }
        usleep(50000); // 50ms
    }
    
    printf("[Scheduler Thread] Shutting down\n");
    return NULL;
}

void hp(int player_id) 
{
    char fifo_read[256], fifo_write[256];
    snprintf(fifo_read, sizeof(fifo_read), "%s%d_to_server", FIFO_PREFIX, player_id);
    snprintf(fifo_write, sizeof(fifo_write), "%s%d_from_server", FIFO_PREFIX, player_id);
    
    usleep(100000); // Wait 100ms
    
    printf("[Player-Handler] Handling Player: %s | Slot: %d | My PID: %d | Parent: %d\n", 
           gptr->PN[player_id], player_id + 1, getpid(), getppid());    
    srand(time(NULL) ^ getpid());
    
    int fd_read = open(fifo_read, O_RDONLY); 
    int fd_write = open(fifo_write, O_WRONLY);

    char buffer[256];

    while (gptr->game_active) {
        if (gptr->CT != player_id) {
            usleep(50000); 
            continue;
        }

        memset(buffer, 0, sizeof(buffer));
        if (read(fd_read, buffer, sizeof(buffer)) > 0) {
            if (strncmp(buffer, "ROLL", 4) == 0) {
                int dice_roll = (rand() % 6) + 1;
                
                pthread_mutex_lock(&gptr->shm_lock);
                
                int old_pos = gptr->PP[player_id];
                gptr->PP[player_id] += dice_roll;

                if (gptr->PP[player_id] >= WC) {
                    gptr->PP[player_id] = WC;
                    gptr->FW = player_id;
                    gptr->game_active = 0;
                    
                    gptr->winner[player_id]++;
                    pthread_mutex_unlock(&gptr->shm_lock);
                    
                    char win_msg[256];
                    snprintf(win_msg, sizeof(win_msg), 
                             "%s rolled %d (R%d -> R%d) and WON! Total wins: %d", 
                             gptr->PN[player_id], dice_roll, old_pos, 
                             gptr->PP[player_id], gptr->winner[player_id]);
                    log_message(win_msg);
                    printf("[Player-Handler] %s\n", win_msg);
                    
                    ss(); 
                } else {
                    int new_pos = gptr->PP[player_id];
                    
                    // Find next active player
                    int next_turn = (gptr->CT + 1) % MAX_P;
                    int attempts = 0;
                    while (attempts < MAX_P && !gptr->player_active[next_turn]) {
                        next_turn = (next_turn + 1) % MAX_P;
                        attempts++;
                    }
                    
                    gptr->CT = next_turn;
                    
                    if (next_turn == 0) {
                        gptr->round++;
                    }
                    pthread_mutex_unlock(&gptr->shm_lock);
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), 
                             "Player %s rolled a %d! (R%d -> R%d)", 
                             gptr->PN[player_id], dice_roll, old_pos, new_pos);
                    log_message(log_msg);
                    printf("[Player-Handler] %s\n", log_msg);
                }

                sprintf(buffer, "ROLLED %d", dice_roll);
                write(fd_write, buffer, strlen(buffer) + 1);
            }
        }
    }
    
    close(fd_read);
    close(fd_write);
}

void sigchld_handler(int sig) {
    int saved_errno = errno;
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[SYSTEM] Player Process %d has exited, cleaning up the grid", pid);
        log_message(buf);
        printf("%s\n", buf);
    }
    errno = saved_errno;
}

void sigint_handler(int sig) 
{
    printf("\n\n[SYSTEM] Player entered Ctrl + C\n");
    printf("[SYSTEM] Finalizing scores and cleaning up the grid...\n");
    sr = 0;

    if (gptr) {
        pthread_mutex_lock(&gptr->shm_lock);
        gptr->game_active = 0;
        pthread_mutex_unlock(&gptr->shm_lock);
    }
}
// OS Assignment - dice game - server.c

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

// setting: min 3 players, and max 5 players, the first player race to R20 will be the winner, each player uses unique FIFO path
#define MXP 5 // MXP = maximum 5 player
#define MNP 3 // MNP = minimum 3 player 
#define wc 20 // win condition when players reaches R20 first 
#define fifo_p "/tmp/player_"
#define log "game.log"
#define srocesf "scores.txt"

// Structure for log messages queue
struct LogNode 
{
    char message[256];
    struct LogNode *next;
};

// Queue for logger thread
struct LogQueue 
{
    struct LogNode *head;
    struct LogNode *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct LogQueue log_queue;

// Main game state structure
struct GameInfo 
{
    int PP[MXP]; //PP = player position 
    int CT; // CT = current turn
    int game_active;
    int CP; // CP = connected player 
    int player_active[MXP];
    int FW; // FW = final winner 
    int round; 
    char PN[MXP][50]; //PN = player name
    int TWN[MXP]; //TWN = total winning
    pthread_mutex_t shm_lock;
    pthread_mutex_t table_sync;
    int mnpr; // mnpr = min player require
};

struct GameInfo *gptr = NULL; // gptr = game pointer
int shared_mem_fd;
pthread_t logger_thread, scheduler_thread;
volatile sig_atomic_t server_running = 1;
pid_t child_pids[MXP];
int child_count_total = 0;

// Function declarations
void ssm(); // ssm = setting share memeory 
void csm(); // csm = cleanning share memory 
void *ltf(void *arg); // ltf = logger thread memor y
void *stf(void *arg); // stf = schedular thread function 
void hd(int player_id); // hd = handler player 
void sigchld_handler(int sig);
void sigint_handler(int sig);
void log_message(const char *message);
void intg(); // intg = intialising game 
void rg(); //rg = reset game 
void ls(); // ls = loading sccros 
void ss(); //ss = saves scors

// Load previous scores from file
void ls() 
{
    FILE *fptr; // fptr = file pointer 
    fptr = fopen(srocesf, "r");
    
    if (fptr == NULL) 
    {
        printf("[SERVER] No previous scores file found\n");
        printf("[SERVER] Starting with fresh scores\n");
        int i;
        for (i = 0; i < MXP; i = i + 1) 
        {
            gptr->TWN[i] = 0;
        }
        return;
    }

    printf("[SERVER] Loading scores from file...\n");
    char line_buffer[256];
    
    while (fgets(line_buffer, sizeof(line_buffer), fptr) != NULL) 
    {
        if (strstr(line_buffer, "Slot") != NULL) 
        {
            if (strstr(line_buffer, "|") != NULL) 
            {
                int slot_n; // slot_n = slot number
                int wins;
                int result;
                result = sscanf(line_buffer, "| Slot %d | %d", &slot_n, &wins);
                
                if (result == 2) 
                {
                    if (slot_n >= 1 && slot_n <= MXP) 
                    {
                        gptr->TWN[slot_n - 1] = wins;
                        printf("   Loaded Slot %d: %d wins\n", slot_n, wins);
                    }
                }
            }
        }
    }

    fclose(fptr);
    printf("[SERVER] Score loading complete\n");
}

// Save current scores to file
void ss() 
{
    pthread_mutex_lock(&gptr->shm_lock);
    
    FILE *fptr;
    fptr = fopen(srocesf, "w");
    
    if (fptr == NULL) 
    {
        perror("Error saving scores");
        pthread_mutex_unlock(&gptr->shm_lock);
        return;
    }
    
    fprintf(fptr, "======================================\n");
    fprintf(fptr, "|   Dice Game Score Statistics       |\n");
    fprintf(fptr, "======================================\n");
    fprintf(fptr, "| %-20s | %-10s |\n", "Slot", "Total Wins");
    fprintf(fptr, "======================================\n");

    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        fprintf(fptr, "| Slot %-16d | %-10d |\n", i + 1, gptr->TWN[i]);
    }
    
    fprintf(fptr, "======================================\n");
    fprintf(fptr, "\nCurrent Game Session:\n");
    
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (strlen(gptr->PN[i]) > 0) 
        {
            fprintf(fptr, "  Slot %d: %s (%d wins)\n", 
                    i + 1, gptr->PN[i], gptr->TWN[i]);
        }
    }
    
    fclose(fptr);
    printf("[SERVER] Scores saved to %s\n", srocesf);
    
    pthread_mutex_unlock(&gptr->shm_lock);
}

int main() 
{
    printf("\n");
    printf(" ============================================================\n");
    printf(" |               Welcome to DICE RACE GAME!                 |\n");
    printf(" ============================================================\n");
    printf(" |                                                          |\n");
    printf(" | HOW TO PLAY (Instruction):                               |\n");
    printf(" | 1. Wait for minimum %d players to join                    |\n", MNP);
    printf(" |  (Maximum of  %d players are allowed)                     |\n", MXP);
    printf(" | 2. Type: ./client YourName (in your own terminal)        |\n");
    printf(" | 3. HIT ENTER when it's your turn, else WAIT              |\n");
    printf(" | 4. First player that reach the Row %d WINS!              |\n", wc);
    printf(" |                                                          |\n");
    printf(" | Will the winner be you? Or them? Let's find out!         |\n");
    printf(" ============================================================\n");
    printf("\n");

    printf("Server Process ID: %d\n", getpid());
    printf("Waiting for %d to %d players...\n\n", MNP, MXP);
    
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        child_pids[i] = -1;
    }
    
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    
    ssm();
    intg();
    
    // Set minimum players based on configuration
    gptr->mnpr = MNP;
    
    // Setup process-shared mutexes
    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gptr->shm_lock, &mutex_attr);
    pthread_mutex_init(&gptr->table_sync, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    // Initialize log queue
    log_queue.head = NULL;
    log_queue.tail = NULL;
    pthread_mutex_init(&log_queue.mutex, NULL);
    pthread_cond_init(&log_queue.cond, NULL);

    ls();
    
    printf("[Main] Creating logger thread...\n");
    int create_result;
    create_result = pthread_create(&logger_thread, NULL, ltf, NULL);
    if (create_result != 0) 
    {
        fprintf(stderr, "Logger thread creation failed: %s\n", strerror(create_result));
        csm();
        exit(EXIT_FAILURE);
    }
    
    printf("[Main] Creating scheduler thread...\n");
    create_result = pthread_create(&scheduler_thread, NULL, stf, NULL);
    if (create_result != 0) 
    {
        fprintf(stderr, "Scheduler thread creation failed: %s\n", strerror(create_result));
        csm();
        exit(EXIT_FAILURE);
    }
    
    printf("\n[Main] Threads created successfully\n");
    printf("[Main] Total threads running: 2\n");
    printf("  - Logger thread\n");
    printf("  - Scheduler thread\n\n");
    
    log_message("Server started - waiting for players to join...");
    
    while (server_running == 1 && gptr->CP < gptr->mnpr) 
    {
        for (i = 0; i < MXP; i = i + 1) 
        {
            if (gptr->player_active[i] == 0) 
            {
                char fifo_path[256];
                snprintf(fifo_path, sizeof(fifo_path), "%s%d_to_server", fifo_p, i);
                
                int file_exists;
                file_exists = access(fifo_path, F_OK);
                
                if (file_exists == 0) 
                {
                    pthread_mutex_lock(&gptr->shm_lock);
                    gptr->player_active[i] = 1;
                    gptr->CP = gptr->CP + 1;
                    pthread_mutex_unlock(&gptr->shm_lock);
                    
                    printf("Player %d connected (%d/%d minimum)\n", 
                           i + 1, gptr->CP, gptr->mnpr);
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Player %d connected", i + 1);
                    log_message(log_msg);
                    
                    pid_t child_pid;
                    child_pid = fork();
                    
                    if (child_pid == 0) 
                    {
                        hd(i);
                        exit(0);
                    } 
                    else if (child_pid < 0) 
                    {
                        fprintf(stderr, "Fork failed for player %d: %s\n", i, strerror(errno));
                        pthread_mutex_lock(&gptr->shm_lock);
                        gptr->player_active[i] = 0;
                        gptr->CP = gptr->CP - 1;
                        pthread_mutex_unlock(&gptr->shm_lock);
                    }
                    else
                    {
                        child_pids[i] = child_pid;
                        child_count_total = child_count_total + 1;
                    }
                }
            }
        }
        sleep(1);
    }
    
    // Check if we have enough players
    if (server_running ==0 || gptr->CP < gptr->mnpr) 
    {
        printf("\n[Main] Server shutting down before game start\n");
        csm();
        exit(EXIT_SUCCESS);
    }
    
    printf("\n===========================================\n");
    printf("  Minimum players connected!\n");
    printf("  Starting game...\n");
    printf("===========================================\n");
    
    pthread_mutex_lock(&gptr->shm_lock);
    gptr->game_active = 1;
    gptr->CT = 0;
    
    int first_active;
    first_active = -1;
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (gptr->player_active[i] ==1) 
        {
            first_active = i;
            break;
        }
    }
    gptr->CT = first_active;
    gptr->round = 1;
    pthread_mutex_unlock(&gptr->shm_lock);
    
    char game_start_log[256];
    snprintf(game_start_log, sizeof(game_start_log), "Game started - all players connected");
    log_message(game_start_log);
    
    printf("\n[Main] System status:\n");
    printf("   Main process PID: %d\n", getpid());
    printf("   Logger thread ID: %lu\n", (unsigned long)logger_thread);
    printf("   Scheduler thread ID: %lu\n", (unsigned long)scheduler_thread);
    
    int child_count;
    child_count = 0;
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (gptr->player_active[i] == 1) 
        {
            child_count = child_count + 1;
        }
    }
    
    printf("   Child processes: %d\n", child_count);
    printf("   Total: 1 parent + 2 threads + %d children\n", child_count);
    
    printf("\n==========\n");
    printf("[Main] Game in progress...\n");
    
    while (server_running == 1 && gptr->game_active ==1) 
    {
        int j;
        for (j =0; j < MXP; j = j + 1) 
        {
            if (gptr->player_active[j] ==0) 
            {
                char fifo_path[256];
                snprintf(fifo_path, sizeof(fifo_path), "%s%d_to_server", fifo_p, j);
                
                int file_exists;
                file_exists = access(fifo_path, F_OK);
                
                if (file_exists == 0) 
                {
                    pthread_mutex_lock(&gptr->shm_lock);
                    gptr->player_active[j] = 1;
                    gptr->CP = gptr->CP + 1;
                    pthread_mutex_unlock(&gptr->shm_lock);
                    
                    printf("Player %d connected (%d/%d maximum)\n", 
                           j + 1, gptr->CP, MXP);
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Player %d connected", j + 1);
                    log_message(log_msg);
                    
                    pid_t child_pid;
                    child_pid = fork();
                    
                    if (child_pid ==0) 
                    {
                        hd(j);
                        exit(0);
                    } 
                    else if (child_pid<0) 
                    {
                        fprintf(stderr, "Fork failed for player %d: %s\n", j, strerror(errno));
                        pthread_mutex_lock(&gptr->shm_lock);
                        gptr->player_active[j] = 0;
                        gptr->CP = gptr->CP - 1;
                        pthread_mutex_unlock(&gptr->shm_lock);
                    }
                    else
                    {
                        child_pids[j] = child_pid;
                        child_count_total = child_count_total + 1;
                    }
                }
            }
        }
        sleep(1);
    }
    
    printf("\n=========================================\n");
    printf("                 Game Ended!               \n");
    printf("============================================\n");
    
    if (gptr->FW >= 0 && gptr->FW<MXP) 
    {
        printf("Winner: %s (Slot %d)\n", gptr->PN[gptr->FW], gptr->FW + 1);
        printf("Total wins for %s: %d\n", gptr->PN[gptr->FW], gptr->TWN[gptr->FW]);
    }
    
    printf("\nFinal Standings:\n");
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (gptr->player_active[i] ==1) 
        {
            printf("  %s: R%d (Total wins: %d)\n", 
                   gptr->PN[i], 
                   gptr->PP[i], 
                   gptr->TWN[i]);
        }
    }
    
    ss();
    
    printf("\nWaiting for all child processes to finish...\n");
    
    int wait_status;
    int children_left;
    children_left = child_count_total;
    
    while (children_left > 0)
    {
        pid_t finished_pid;
        finished_pid = waitpid(-1, &wait_status, 0);
        
        if (finished_pid > 0)
        {
            children_left = children_left - 1;
            printf("[Main] Child process %d finished (%d remaining)\n", finished_pid, children_left);
        }
        else if (finished_pid == -1)
        {
            if (errno == ECHILD)
            {
                break;
            }
        }
    }
    
    printf("[Main] All child processes have finished\n");
    
    printf("\n[Main] Cleaning up and shutting down...\n");
    
    server_running = 0;
    pthread_mutex_lock(&log_queue.mutex);
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);
    
    pthread_join(logger_thread, NULL);
    pthread_join(scheduler_thread, NULL);
    
    printf("[Main] Threads joined\n");
    
    csm();
    
    printf("\n===========================================\n");
    printf("         Server Shutdown Complete           \n");
    printf("==============================================\n");
    
    return 0;
}

void ssm() 
{
    shared_mem_fd = shm_open("/dice_game_shm", O_CREAT | O_RDWR, 0666);
    
    if (shared_mem_fd == -1) 
    {
        perror("Shared memory creation failed");
        exit(EXIT_FAILURE);
    }
    
    int truncate_result;
    truncate_result = ftruncate(shared_mem_fd, sizeof(struct GameInfo));
    
    if (truncate_result == -1) 
    {
        perror("Shared memory truncation failed");
        exit(EXIT_FAILURE);
    }
    
    gptr = mmap(NULL, sizeof(struct GameInfo), 
                    PROT_READ | PROT_WRITE, MAP_SHARED, shared_mem_fd,0);
    
    if (gptr == MAP_FAILED) 
    {
        perror("Memory mapping failed");
        exit(EXIT_FAILURE);
    }
}

void csm() 
{
    if (gptr != NULL) 
    {
        pthread_mutex_destroy(&gptr->shm_lock);
        pthread_mutex_destroy(&gptr->table_sync);
        munmap(gptr, sizeof(struct GameInfo));
    }
    
    shm_unlink("/dice_game_shm");
    
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        char fifo_path[256];
        
        snprintf(fifo_path, sizeof(fifo_path), "%s%d_to_server", fifo_p, i);
        unlink(fifo_path);
        
        snprintf(fifo_path, sizeof(fifo_path), "%s%d_from_server", fifo_p, i);
        unlink(fifo_path);
    }
    
    pthread_mutex_destroy(&log_queue.mutex);
    pthread_cond_destroy(&log_queue.cond);
    
    struct LogNode *current_node;
    current_node = log_queue.head;
    
    while (current_node != NULL) 
    {
        struct LogNode *next_node;
        next_node = current_node->next;
        free(current_node);
        current_node = next_node;
    }
}

void intg() 
{
    memset(gptr, 0, sizeof(struct GameInfo));
    
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        gptr->PP[i] = 0;
        gptr->player_active[i] =0;
    }
    
    gptr->CT = 0;
    gptr->game_active =0;
    gptr->CP =0;
    gptr->FW = -1;
    gptr->round =0;
}

void rg() 
{
    pthread_mutex_lock(&gptr->shm_lock);
    
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        gptr->PP[i] = 0;
        gptr->player_active[i] =0;
    }
    
    gptr->CT =0;
    gptr->game_active = 0;
    gptr->CP = 0;
    gptr->FW = -1;
    gptr->round =0;
    
    pthread_mutex_unlock(&gptr->shm_lock);
}

void log_message(const char *message) 
{
    time_t current_time;
    current_time = time(NULL);
    
    char *time_string;
    time_string = ctime(&current_time);
    time_string[strlen(time_string) -1] = '\0';
    
    char formatted_message[512];
    snprintf(formatted_message, sizeof(formatted_message), "[%s] %s\n", time_string, message);
    
    pthread_mutex_lock(&gptr->shm_lock);
    
    FILE *log_file;
    log_file = fopen(log, "a");
    
    if (log_file != NULL) 
    {
        fprintf(log_file, "%s", formatted_message);
        fflush(log_file);
        fclose(log_file);
    }
    
    pthread_mutex_unlock(&gptr->shm_lock);
}

void *ltf(void *arg) {
    printf("[Logger Thread] Started with TID: %lu\n", (unsigned long)pthread_self());
    FILE *log_file;

    while (server_running == 1 || log_queue.head != NULL) 
    {
        pthread_mutex_lock(&log_queue.mutex);
        
        while (log_queue.head == NULL && server_running == 1) 
        {
            pthread_cond_wait(&log_queue.cond, &log_queue.mutex);
        }
        
        if (log_queue.head == NULL && server_running == 0) 
        {
            pthread_mutex_unlock(&log_queue.mutex);
            break;
        }

        struct LogNode *current_node;
        current_node = log_queue.head;
        log_queue.head = current_node->next;
        
        if (log_queue.head == NULL) 
        {
            log_queue.tail = NULL;
        }
        
        pthread_mutex_unlock(&log_queue.mutex);

        log_file = fopen(log, "a");
        if (log_file != NULL) 
        {
            fprintf(log_file, "%s\n", current_node->message);
            fclose(log_file);
        }
        free(current_node);
    }
    
    printf("[Logger Thread] Shutting down\n");
    return NULL;
}

void *stf(void *arg) 
{
    printf("[Scheduler Thread] Started with TID: %lu\n", (unsigned long)pthread_self());
    
    while (server_running == 1 && gptr->game_active ==1) 
    {
        pthread_mutex_lock(&gptr->shm_lock);
        
        int game_should_end;
        game_should_end = 0;
        
        int i;
        for (i = 0; i < MXP; i = i + 1 ) 
        {
            if (gptr->PP[i] >= wc) 
            {
                game_should_end =1;
                break;
            }
        }
        
        pthread_mutex_unlock(&gptr->shm_lock);
        
        if (game_should_end == 1) 
        {
            break;
        }
        
        usleep(100000);
    }
    
    printf("[Scheduler Thread] Shutting down\n");
    return NULL;
}

void hd(int player_id) 
{
    char fifo_read_path[256];
    char fifo_write_path[256];
    snprintf(fifo_read_path, sizeof(fifo_read_path), "%s%d_to_server", fifo_p, player_id);
    snprintf(fifo_write_path, sizeof(fifo_write_path), "%s%d_from_server", fifo_p, player_id);
    
    usleep(2000);
    
    printf("[Player-Handler] Started for player: %s\n", gptr->PN[player_id]);
    printf("[Player-Handler] Slot: %d | PID: %d | Parent PID: %d\n", 
           player_id + 1, getpid(), getppid());
    
    char process_log[256];
    snprintf(process_log, sizeof(process_log), 
             "[Player-Handler] Process %d handling %s (Slot %d)", 
             getpid(), gptr->PN[player_id], player_id + 1);
    log_message(process_log);
    
    srand(time(NULL) ^ getpid());
    
    int fd_read;
    int fd_write;
    fd_read = open(fifo_read_path, O_RDONLY | O_NONBLOCK);
    fd_write = open(fifo_write_path, O_WRONLY);

    char buffer[256];

    while (gptr->game_active == 1) 
    {
        if (gptr->CT != player_id) 
        {
            usleep(100000);
            continue;
        }

        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read;
        bytes_read = read(fd_read, buffer, sizeof(buffer));
        
        if (bytes_read > 0) 
        {
            int compare_result;
            compare_result = strncmp(buffer, "ROLL", 4);
            
            if (compare_result == 0) 
            {
                int dice_value;
                dice_value = (rand() % 6) + 1;
                
                pthread_mutex_lock(&gptr->shm_lock);
                
                gptr->PP[player_id] = gptr->PP[player_id] + dice_value;

                if (gptr->PP[player_id] >= wc) 
                {
                    gptr->PP[player_id] = wc;
                    gptr->FW = player_id;
                    gptr->game_active =0;
                    
                    gptr->TWN[player_id] = gptr->TWN[player_id] + 1;
                    
                    printf("\n[Player-Handler] %s reached the goal!\n", gptr->PN[player_id]);
                    printf("[Player-Handler] Player %d wins! Total wins: %d\n", 
                           player_id + 1, gptr->TWN[player_id]);
                } 
                else 
                {
                    int next_player;
                    next_player = (gptr->CT +1) % MXP;
                    
                    int attempts;
                    attempts = 0;
                    while (gptr->player_active[next_player] ==0 && attempts < MXP) 
                    {
                        next_player = (next_player +1) % MXP;
                        attempts = attempts +1;
                    }
                    
                    gptr->CT = next_player;
                    
                    if (next_player ==0) 
                    {
                        gptr->round = gptr->round +1;
                    }
                }

                pthread_mutex_unlock(&gptr->shm_lock);

                sprintf(buffer, "ROLLED %d", dice_value);
                write(fd_write, buffer, strlen(buffer) + 1);
                
                char roll_log[256];
                snprintf(roll_log, sizeof(roll_log), 
                         "Player %s rolled %d! Position: R%d", 
                         gptr->PN[player_id], 
                         dice_value, 
                         gptr->PP[player_id]);
                log_message(roll_log);
                printf("[Player-Handler] %s\n", roll_log);
                
                if (gptr->game_active == 0) 
                {
                    break;
                }
            }
        } 
        else 
        {
            if (gptr->game_active == 0)
            {
                break;
            }
            usleep(50000);
        }
    }
    
    close(fd_read);
    close(fd_write);
    
    char exit_log[256];
    snprintf(exit_log, sizeof(exit_log), 
             "[Player-Handler] Process %d for %s disconnecting", 
             getpid(), gptr->PN[player_id]);
    log_message(exit_log);
    
    printf("[Player-Handler] Handler for %s exiting\n", gptr->PN[player_id]);
}

void sigchld_handler(int sig)
{
    int saved_error;
    saved_error = errno;
    
    pid_t process_id;
    int wait_status;

    while (1) 
    {
        process_id = waitpid(-1, &wait_status, WNOHANG);
        
        if (process_id >0) 
        {
            char exit_message[128];
            snprintf(exit_message, sizeof(exit_message), 
                     "[SYSTEM] Player Process %d has exited", process_id);
            log_message(exit_message);
        } 
        else 
        {
            break;
        }
    }
    
    errno = saved_error;
}

void sigint_handler(int sig) 
{
    printf("\n\n[SYSTEM] Player hit Ctrl + C\n");
    printf("[SYSTEM] Saving scores and shutting down...\n");
    server_running = 0;

    pthread_mutex_lock(&log_queue.mutex);
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);
    
    if (gptr != NULL) 
    {
        pthread_mutex_lock(&gptr->shm_lock);
        gptr->game_active = 0;
        pthread_mutex_unlock(&gptr->shm_lock);
    }
}
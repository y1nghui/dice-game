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

#define MAX_PLAYERS 3
#define WINNING_SCORE 20
#define FIFO_PREFIX "/tmp/player_"
#define LOG_FILE "game.log"
#define SCORES_FILE "scores.txt"

// --- LOGGER QUEUE DEFINITIONS (TASK 3) ---
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
// -----------------------------------------

// Game state stored in shared memory
typedef struct {
    int player_positions[MAX_PLAYERS];
    int current_turn;
    int game_active;
    int players_connected;
    int player_active[MAX_PLAYERS];
    int game_winner;
    int round_num;
    char player_names[MAX_PLAYERS][50];
    int player_wins[MAX_PLAYERS]; // Added for scoring (Task 2)
    pthread_mutex_t game_mutex;
    pthread_mutex_t log_mutex; // Kept for structure compatibility, but queue uses its own
} GameState;

GameState *shared_state = NULL;
int shm_fd;
pthread_t logger_thread, scheduler_thread;
volatile sig_atomic_t server_running = 1;

// Function Prototypes
void setup_shared_memory();
void cleanup_shared_memory();
void *logger_thread_func(void *arg);
void *scheduler_thread_func(void *arg);
void handle_client(int player_id);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void log_message(const char *message);
void init_game();
void reset_game();
void load_scores();
void save_scores();

void load_scores() {
    FILE *f = fopen(SCORES_FILE, "r");
    if (f) {
        printf("[Server] Loading scores from %s...\n", SCORES_FILE);
        
        char line[256];
        int slot = 0;

        // Read the file line by line
        while (fgets(line, sizeof(line), f)) {
            // We look for lines that start with '|' but NOT header lines
            // A data line looks like: "| Alice                | 5          |"
            
            // Skip separator lines (====) or the Header Title
            if (line[0] != '|') continue;
            if (strstr(line, "Dice Game")) continue;
            if (strstr(line, "Client Name")) continue;

            // If we are here, it's a data line. Parse it.
            // Format: "| Name | Score |"
            int score = 0;
            
            // Using logic: Find the last '|', the number is just before it.
            // Simplified parsing:
            char *last_pipe = strrchr(line, '|'); // Find end pipe
            if (last_pipe) {
                *last_pipe = '\0'; // Cut off the end pipe
                
                // Find the second-to-last pipe (separator between name and score)
                char *middle_pipe = strrchr(line, '|');
                if (middle_pipe) {
                    // The string after middle_pipe is " 5 "
                    score = atoi(middle_pipe + 1); // Convert to int
                    
                    if (slot < MAX_PLAYERS) {
                        shared_state->player_wins[slot] = score;
                        printf("  - Slot %d Wins: %d (Loaded)\n", slot + 1, score);
                        slot++;
                    }
                }
            }
        }
        fclose(f);
    } else {
        printf("[Server] No previous scores found. Starting fresh.\n");
        for (int i = 0; i < MAX_PLAYERS; i++) {
            shared_state->player_wins[i] = 0;
        }
    }
}

void save_scores() {
    FILE *f = fopen(SCORES_FILE, "w");
    if (f) {
        // 1. Write the Header
        fprintf(f, "======================================\n");
        fprintf(f, "|           Dice Game Results        |\n");
        fprintf(f, "======================================\n");
        fprintf(f, "| %-20s | %-10s |\n", "Client Name", "Score");
        fprintf(f, "======================================\n");

        // 2. Write each player's data
        for (int i = 0; i < MAX_PLAYERS; i++) {
            char display_name[50];
            
            // --- FIX: REMOVED MUTEX LOCKS HERE (Already locked by caller) ---
            
            if (strlen(shared_state->player_names[i]) > 0) {
                strncpy(display_name, shared_state->player_names[i], 49);
                display_name[49] = '\0';
            } else {
                snprintf(display_name, sizeof(display_name), "Slot %d (Empty)", i + 1);
            }
            // ----------------------------------------------------------------

            fprintf(f, "| %-20s | %-10d |\n", display_name, shared_state->player_wins[i]);
        }
        
        // 3. Write Footer
        fprintf(f, "======================================\n");
        fclose(f);
        printf("[Server] Scores saved to %s (Table Format)\n", SCORES_FILE);
    } else {
        perror("Failed to save scores");
    }
}

int main() {
    printf("=== Dice Race Game Server - Started ===\n");
    printf("=== Server PID: %-24d ===\n", getpid());
    printf("Waiting for %d players to connect...\n\n", MAX_PLAYERS);
    
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    
    // 1. Create Memory
    setup_shared_memory();
    
    // 2. Clear Memory (CRITICAL: Must be before mutex init)
    init_game();
    
    // 3. Initialize Shared Mutexes
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared_state->game_mutex, &attr);
    pthread_mutex_init(&shared_state->log_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // 4. Initialize Logger Queue Mutexes (Local to process)
    log_queue.head = NULL;
    log_queue.tail = NULL;
    pthread_mutex_init(&log_queue.mutex, NULL);
    pthread_cond_init(&log_queue.cond, NULL);

    // 5. Load scores
    load_scores();
    
    // Create logger thread
    printf("[Main] Creating logger thread...\n");
    int ret = pthread_create(&logger_thread, NULL, logger_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to create logger thread: %s\n", strerror(ret));
        cleanup_shared_memory();
        exit(EXIT_FAILURE);
    }
    
    // Create scheduler thread
    printf("[Main] Creating scheduler thread...\n");
    ret = pthread_create(&scheduler_thread, NULL, scheduler_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to create scheduler thread: %s\n", strerror(ret));
        cleanup_shared_memory();
        exit(EXIT_FAILURE);
    }
    
    printf("\n[Main] Both threads created successfully!\n");
    printf("[Main] Thread count: 2 (logger + scheduler)\n\n");
    
    log_message("Server started - waiting for players");
    
    // Main accept loop
    while (server_running && shared_state->players_connected < MAX_PLAYERS) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!shared_state->player_active[i]) {
                char fifo_name[256];
                snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, i);
                
                if (access(fifo_name, F_OK) == 0) {
                    pthread_mutex_lock(&shared_state->game_mutex);
                    shared_state->player_active[i] = 1;
                    shared_state->players_connected++;
                    pthread_mutex_unlock(&shared_state->game_mutex);
                    
                    printf("Player %d connected (%d/%d)\n", i + 1, 
                           shared_state->players_connected, MAX_PLAYERS);
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Player %d connected", i + 1);
                    log_message(log_msg);
                    
                    pid_t pid = fork();
                    if (pid == 0) {
                        handle_client(i);
                        exit(0);
                    } else if (pid < 0) {
                        fprintf(stderr, "Fork failed for player %d: %s\n", i, strerror(errno));
                        pthread_mutex_lock(&shared_state->game_mutex);
                        shared_state->player_active[i] = 0;
                        shared_state->players_connected--;
                        pthread_mutex_unlock(&shared_state->game_mutex);
                    }
                }
            }
        }
        sleep(1); 
    }
    
    if (shared_state->players_connected == MAX_PLAYERS) {
        printf("=== All players connected! Starting game ===\n");
        
        pthread_mutex_lock(&shared_state->game_mutex);
        shared_state->game_active = 1;
        pthread_mutex_unlock(&shared_state->game_mutex);
        
        log_message("Game started - all players connected");
        
        printf("[Main] Active processes:\n");
        printf("  - Main server process (PID: %d)\n", getpid());
        printf("  - Logger thread (TID: %lu)\n", (unsigned long)logger_thread);
        printf("  - Scheduler thread (TID: %lu)\n", (unsigned long)scheduler_thread);
        printf("  - 3 child processes (forked for each player)\n");
        printf("[Main] Total: 1 process + 2 threads + 3 child processes\n\n");
    }
    
    // Wait for game to finish
    printf("[Main] Waiting for game to complete...\n");
    while (server_running && shared_state->game_active) {
        sleep(1);
    }
    
    printf("\n[Main] Game ended. Joining threads...\n");
    
    // Wake up logger so it can exit
    pthread_mutex_lock(&log_queue.mutex);
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);

    pthread_join(logger_thread, NULL);
    printf("[Main] Logger thread joined\n");
    pthread_join(scheduler_thread, NULL);
    printf("[Main] Scheduler thread joined\n");
    
    printf("[Main] Cleaning up...\n");
    cleanup_shared_memory();

    printf("=== Server shutdown complete ===\n");
    return 0;
}

void setup_shared_memory() {
    shm_fd = shm_open("/dice_game_shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    
    if (ftruncate(shm_fd, sizeof(GameState)) == -1) {
        perror("ftruncate failed");
        close(shm_fd);
        shm_unlink("/dice_game_shm");
        exit(EXIT_FAILURE);
    }
    
    shared_state = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, 
                        MAP_SHARED, shm_fd, 0);
    if (shared_state == MAP_FAILED) {
        perror("mmap failed");
        close(shm_fd);
        shm_unlink("/dice_game_shm");
        exit(EXIT_FAILURE);
    }
    // Note: Mutex init moved to main() to avoid memset wiping it
}

void cleanup_shared_memory() {
    if (shared_state != NULL) {
        pthread_mutex_destroy(&shared_state->game_mutex);
        pthread_mutex_destroy(&shared_state->log_mutex);
        munmap(shared_state, sizeof(GameState));
    }
    close(shm_fd);
    shm_unlink("/dice_game_shm");
    
    // Clean up FIFOs
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char fifo_name[256];
        snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, i);
        unlink(fifo_name);
        snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, i);
        unlink(fifo_name);
    }
    
    // Clean up Log Queue
    pthread_mutex_destroy(&log_queue.mutex);
    pthread_cond_destroy(&log_queue.cond);
}

void init_game() {
    memset(shared_state, 0, sizeof(GameState));
    shared_state->current_turn = 0;
    shared_state->game_active = 0;
    shared_state->players_connected = 0;
    shared_state->game_winner = -1;
    shared_state->round_num = 1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        shared_state->player_positions[i] = 0;
        shared_state->player_active[i] = 0;
        memset(shared_state->player_names[i], 0, 50);
    }
}

void reset_game() {
    pthread_mutex_lock(&shared_state->game_mutex);
    shared_state->current_turn = 0;
    shared_state->game_winner = -1;
    shared_state->round_num = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        shared_state->player_positions[i] = 0;
    }
    pthread_mutex_unlock(&shared_state->game_mutex);
}

// --- CONCURRENT LOGGER PRODUCER (TASK 3) ---
void log_message(const char *message) {
    LogNode *new_node = (LogNode *)malloc(sizeof(LogNode));
    if (!new_node) {
        perror("Failed to allocate log node");
        return;
    }
    
    time_t now = time(NULL);
    char *time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';
    snprintf(new_node->message, sizeof(new_node->message), "[%s] %s", time_str, message);
    new_node->next = NULL;

    pthread_mutex_lock(&log_queue.mutex);
    
    if (log_queue.tail == NULL) {
        log_queue.head = new_node;
        log_queue.tail = new_node;
    } else {
        log_queue.tail->next = new_node;
        log_queue.tail = new_node;
    }
    
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);
}

// --- CONCURRENT LOGGER CONSUMER (TASK 3) ---
void *logger_thread_func(void *arg) {
    printf("[Logger Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    FILE *log_file;

    while (server_running || log_queue.head != NULL) {
        pthread_mutex_lock(&log_queue.mutex);
        
        while (log_queue.head == NULL && server_running) {
            pthread_cond_wait(&log_queue.cond, &log_queue.mutex);
        }
        
        if (log_queue.head == NULL && !server_running) {
            pthread_mutex_unlock(&log_queue.mutex);
            break;
        }

        LogNode *current = log_queue.head;
        log_queue.head = current->next;
        if (log_queue.head == NULL) {
            log_queue.tail = NULL;
        }
        
        pthread_mutex_unlock(&log_queue.mutex);

        log_file = fopen(LOG_FILE, "a");
        if (log_file) {
            fprintf(log_file, "%s\n", current->message);
            fclose(log_file);
        }
        free(current);
    }
    
    printf("[Logger Thread] Shutting down\n");
    return NULL;
}

void *scheduler_thread_func(void *arg) {
    printf("[Scheduler Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    
    while (server_running) {
        if (shared_state->game_active) {
            pthread_mutex_lock(&shared_state->game_mutex);
            
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (shared_state->player_positions[i] >= WINNING_SCORE) {
                    shared_state->game_winner = i;
                    shared_state->game_active = 0;
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Game Over! %s wins!", 
                             shared_state->player_names[i]);
                    log_message(log_msg);
                    printf("%s\n", log_msg);
                    
                    // Note: Wins are incremented in handle_client, but good practice to double check here 
                    // or handle global win events. We rely on handle_client for score update in this logic.
                    
                    pthread_mutex_unlock(&shared_state->game_mutex);
                    printf("[Scheduler Thread] Game ended, shutting down\n");
                    return NULL;
                }
            }
            pthread_mutex_unlock(&shared_state->game_mutex);
        }
        usleep(100000); 
    }
    
    printf("[Scheduler Thread] Shutting down\n");
    return NULL;
}

void handle_client(int player_id) {
    char fifo_read[256], fifo_write[256];
    snprintf(fifo_read, sizeof(fifo_read), "%s%d_to_server", FIFO_PREFIX, player_id);
    snprintf(fifo_write, sizeof(fifo_write), "%s%d_from_server", FIFO_PREFIX, player_id);
    
    // Wait a moment for client to write their name to shared memory
    usleep(200000); // 200ms
    
    printf("[Child Process] Handling %s (slot %d, PID: %d, PPID: %d)\n", 
           shared_state->player_names[player_id], player_id + 1, getpid(), getppid());
    
    // Seed random number generator unique to this child process
    srand(time(NULL) ^ getpid());
    
    int fd_read = open(fifo_read, O_RDONLY); 
    int fd_write = open(fifo_write, O_WRONLY);

    char buffer[256];

    while (shared_state->game_active) {
        // Check if it is this player's turn
        if (shared_state->current_turn != player_id) {
            usleep(100000); // Sleep 0.1s to avoid high CPU usage
            continue;
        }

        memset(buffer, 0, sizeof(buffer));
        if (read(fd_read, buffer, sizeof(buffer)) > 0) {
            
            if (strncmp(buffer, "ROLL", 4) == 0) {
                // --- CRITICAL: Server generates the number ---
                int dice_roll = (rand() % 6) + 1;
                
                pthread_mutex_lock(&shared_state->game_mutex);
                shared_state->player_positions[player_id] += dice_roll;
                
                // Check Win Condition
                if (shared_state->player_positions[player_id] >= WINNING_SCORE) {
                    shared_state->game_winner = player_id;
                    shared_state->game_active = 0;
                    
                    // --- SCORING LOGIC ---
                    shared_state->player_wins[player_id]++;
                    printf("Player %d won! Total wins: %d\n", 
                           player_id + 1, shared_state->player_wins[player_id]);
                    save_scores(); 
                    // ---------------------
                    
                } else {
                    // --- ROUND & TURN UPDATE LOGIC ---
                    int next_turn = (shared_state->current_turn + 1) % MAX_PLAYERS;
                    shared_state->current_turn = next_turn; // Fixed missing semicolon here
                    
                    // If we wrap back to Player 0 (Slot 1), the round is finished
                    if (next_turn == 0) {
                        shared_state->round_num++;
                    }
                    // --------------------------------
                }
                
                // --- CRITICAL FIX: UNLOCK BEFORE WRITING ---
                pthread_mutex_unlock(&shared_state->game_mutex);

                // Send Result back to Client
                sprintf(buffer, "ROLLED %d", dice_roll);
                write(fd_write, buffer, strlen(buffer) + 1);
                
                // Log the event
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), "Player %d rolled %d", player_id + 1, dice_roll);
                log_message(log_msg);
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
        // printf("[SIGCHLD] Reaped PID: %d\n", pid); // Optional noise
    }
    errno = saved_errno;
}

void sigint_handler(int sig) {
    printf("\n\n[SIGINT Handler] Caught interrupt signal\n");
    printf("[SIGINT Handler] Shutting down server gracefully...\n");
    server_running = 0;
    
    // Wake up logger queue if stuck waiting
    pthread_mutex_lock(&log_queue.mutex);
    pthread_cond_signal(&log_queue.cond);
    pthread_mutex_unlock(&log_queue.mutex);
    
    if (shared_state) {
        shared_state->game_active = 0;
    }
}
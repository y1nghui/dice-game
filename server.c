//OS Assignment - dice game - server.c

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

// Game state stored in shared memory
typedef struct {
    int player_positions[MAX_PLAYERS];
    int current_turn;
    int game_active;
    int players_connected;
    int player_active[MAX_PLAYERS];
    int game_winner;
    char player_names[MAX_PLAYERS][50];  // Store player names
    pthread_mutex_t game_mutex;
    pthread_mutex_t log_mutex;
} GameState;

GameState *shared_state = NULL;
int shm_fd;
pthread_t logger_thread, scheduler_thread;
volatile sig_atomic_t server_running = 1;

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

int main() {
    printf("=== Dice Race Game Server - Started ===\n");
    printf("=== Server PID: %-24d ===\n", getpid());
    printf("Waiting for %d players to connect...\n\n", MAX_PLAYERS);
    
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, sigint_handler);
    
    setup_shared_memory();
    init_game();
    
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
    
    // Main accept loop - wait for players via named pipes
    while (server_running && shared_state->players_connected < MAX_PLAYERS) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!shared_state->player_active[i]) {
                char fifo_name[256];
                snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, i);
                
                // Check if player's FIFO exists
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
                    
                    // Fork child for this player
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
        
        sleep(1); // Check for new connections every second
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
    
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared_state->game_mutex, &attr);
    pthread_mutex_init(&shared_state->log_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
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
}

void init_game() {
    memset(shared_state, 0, sizeof(GameState));
    shared_state->current_turn = 0;
    shared_state->game_active = 0;
    shared_state->players_connected = 0;
    shared_state->game_winner = -1;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        shared_state->player_positions[i] = 0;
        shared_state->player_active[i] = 0;
        memset(shared_state->player_names[i], 0, 50);
    }
}

void reset_game() {
    // TODO: - Implement game reset for successive games
    // This should reset game state but preserve player connections
    pthread_mutex_lock(&shared_state->game_mutex);
    shared_state->current_turn = 0;
    shared_state->game_winner = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        shared_state->player_positions[i] = 0;
    }
    pthread_mutex_unlock(&shared_state->game_mutex);
}

void *logger_thread_func(void *arg) {
    // Logger runs continuously and handles log writes
    printf("[Logger Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    
    // TODO: - Implement message queue for concurrent logging
    // Should use a circular buffer or linked list for queued messages
    // Example structure:
    // - Check queue for pending messages
    // - Write to file atomically
    // - Signal waiting threads that queue has space
    
    while (server_running) {
        // Logger would process queued messages here
        sleep(2); // Simulate periodic logging activity
    }
    
    printf("[Logger Thread] Shutting down\n");
    return NULL;
}

void *scheduler_thread_func(void *arg) {
    printf("[Scheduler Thread] Started (TID: %lu)\n", (unsigned long)pthread_self());
    
    // TODO: - Implement Round Robin turn scheduling
    // Requirements:
    // 1. Advance turn to next active player
    // 2. Skip disconnected players
    // 3. Check win condition after each turn
    // 4. Signal appropriate player processes
    
    while (server_running) {
        if (shared_state->game_active) {
            pthread_mutex_lock(&shared_state->game_mutex);
            
            // Check if any player won
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (shared_state->player_positions[i] >= WINNING_SCORE) {
                    shared_state->game_winner = i;
                    shared_state->game_active = 0;
                    
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Game Over! %s wins!", 
                             shared_state->player_names[i]);
                    log_message(log_msg);
                    printf("%s\n", log_msg);
                    
                    pthread_mutex_unlock(&shared_state->game_mutex);
                    printf("[Scheduler Thread] Game ended, shutting down\n");
                    return NULL;
                }
            }
            
            pthread_mutex_unlock(&shared_state->game_mutex);
        }
        
        usleep(100000); // Check every 100ms for responsiveness
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
    
    // TODO: - Implement client handler
    // Required functionality:
    // 1. Open FIFOs for bidirectional communication
    // 2. Read player commands (ROLL, QUIT, etc.)
    // 3. Validate it's player's turn before processing
    // 4. Generate dice roll SERVER-SIDE (use rand() with proper seed)
    // 5. Update shared memory safely with mutexes
    // 6. Send results back to client
    // 7. Handle disconnections gracefully
    
    // Seed random number generator with process ID + time
    srand(time(NULL) ^ getpid());
    
    int turn_count = 0;
    while (shared_state->game_active) {
        sleep(1);
        turn_count++;
        
        // Simulate some activity every 5 seconds
        if (turn_count % 5 == 0) {
            printf("[Child Process] Player %d still active (turns: %d)\n", 
                   player_id + 1, turn_count);
        }
    }
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "%s (slot %d) disconnected", 
             shared_state->player_names[player_id], player_id + 1);
    log_message(log_msg);
    
    printf("[Child Process] %s handler exiting (PID: %d)\n", 
           shared_state->player_names[player_id], getpid());
}

void log_message(const char *message) {
    pthread_mutex_lock(&shared_state->log_mutex);
    
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';
        
        fprintf(log_file, "[%s] %s\n", time_str, message);
        fclose(log_file);
    } else {
        fprintf(stderr, "Warning: Could not open log file: %s\n", strerror(errno));
    }
    
    pthread_mutex_unlock(&shared_state->log_mutex);
}

void sigchld_handler(int sig) {
    // Reap zombie child processes
    int saved_errno = errno;
    pid_t pid;
    int status;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("[SIGCHLD Handler] Reaped zombie process PID: %d\n", pid);
        if (WIFEXITED(status)) {
            printf("[SIGCHLD Handler] Child exited with status: %d\n", WEXITSTATUS(status));
        }
    }
    
    errno = saved_errno;
}

void sigint_handler(int sig) {
    printf("\n\n[SIGINT Handler] Caught interrupt signal\n");
    printf("[SIGINT Handler] Shutting down server gracefully...\n");
    server_running = 0;
    
    if (shared_state) {
        shared_state->game_active = 0;
    }
}
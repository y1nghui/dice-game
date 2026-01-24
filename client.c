// OS Assignment - dice game - client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define MAX_PLAYERS 3
#define WINNING_SCORE 20
#define FIFO_PREFIX "/tmp/player_"

// Shared memory structure
typedef struct {
    int player_positions[MAX_PLAYERS];
    int current_turn;
    int game_active;
    int players_connected;
    int player_active[MAX_PLAYERS];
    int game_winner;
    int round_num;
    char player_names[MAX_PLAYERS][50];
    int player_wins[MAX_PLAYERS]; 
    pthread_mutex_t game_mutex;
    pthread_mutex_t log_mutex;
} GameState;

GameState *shared_state = NULL;
int player_id = -1;
char player_name[50];

void setup_shared_memory();
void create_fifos();
void display_grid(const char *last_action, const char *current_status); 
void play_game();
void cleanup();
int find_available_slot();

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <username>\n", argv[0]);
        return 1;
    }
    
    strncpy(player_name, argv[1], sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';
    
    printf("=== Dice Race Game - %-20s ===\n", player_name);
    
    setup_shared_memory();
    
    player_id = find_available_slot();
    if (player_id == -1) {
        fprintf(stderr, "ERROR: Game is full (3 players maximum)\n");
        cleanup();
        return 1;
    }
    
    printf("✓ Assigned to Player slot %d\n", player_id + 1);
    
    pthread_mutex_lock(&shared_state->game_mutex);
    strncpy(shared_state->player_names[player_id], player_name, 49);
    shared_state->player_names[player_id][49] = '\0';
    pthread_mutex_unlock(&shared_state->game_mutex);
    
    create_fifos();
    
    printf("✓ Connected to server as %s!\n", player_name);
    printf("Waiting for other players to join...\n\n");
    
    char join_msg[100];
    snprintf(join_msg, sizeof(join_msg), "%s Joined Game.", player_name);
    
    int last_count = -1;
    while (!shared_state->game_active) {
        int current_count = shared_state->players_connected;
        if (current_count != last_count) {
            last_count = current_count;
            printf("Players connected: %d/%d\n", current_count, MAX_PLAYERS);
            display_grid(join_msg, "Waiting for players to join...");
        }
        sleep(1);
    }
    
    printf("\nGame Started! Race to %d!\n", WINNING_SCORE);
    
    play_game();
    
    display_grid("GAME OVER!", "Final Results");
    
    // --- DYNAMIC FILENAME SAVING ---
    char filename[256];
    snprintf(filename, sizeof(filename), "result_%s.txt", player_name);
    
    FILE *f = fopen(filename, "w");
    if (f) {
        fprintf(f, "Game Result for %s\n", player_name);
        fprintf(f, "Winner: %s\n", shared_state->player_names[shared_state->game_winner]);
        fprintf(f, "My Wins: %d\n", shared_state->player_wins[player_id]);
        fprintf(f, "Final Position: R%d\n", shared_state->player_positions[player_id]);
        fclose(f);
        // printf("\n[Client] Summary saved to %s\n", filename); // Optional print
    }
    // -------------------------------

    printf("\n##########################################\n");
    if (shared_state->game_winner == player_id) {
        printf("       CONGRATULATIONS! YOU WON!          \n");
    } else {
        printf("       Game Over! %s won the game!        \n", 
               shared_state->player_names[shared_state->game_winner]);
    }
    printf("##########################################\n");
    
    // --- CHANGED: Use Name instead of Slot Number ---
    printf("   Total Wins for %s: %d\n", player_name, shared_state->player_wins[player_id]);
    printf("------------------------------------------\n");
    
    cleanup();
    return 0;
}

void setup_shared_memory() {
    int shm_fd = shm_open("/dice_game_shm", O_RDWR, 0666);
    if (shm_fd == -1) {
        fprintf(stderr, "ERROR: Could not connect to server: %s\n", strerror(errno));
        exit(1);
    }
    shared_state = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_state == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
    close(shm_fd);
}

int find_available_slot() {
    pthread_mutex_lock(&shared_state->game_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        // --- CRITICAL FIX: BRACES ADDED ---
        if (!shared_state->player_active[i]) {
            slot = i; 
            break;
        }
        // ----------------------------------
    }
    pthread_mutex_unlock(&shared_state->game_mutex);
    return slot;
}

void create_fifos() {
    char fifo_name[256];
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) perror("mkfifo");
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) perror("mkfifo");
}

void display_grid(const char *last_action, const char *current_status) {
    printf("\033[H\033[J"); // Clear Screen

    // 1. Header
    printf("==========================================\n");
    printf("       DICE RACE - ROUND %d\n", shared_state->round_num);
    printf("==========================================\n");

    // 2. The Grid
    for (int row = WINNING_SCORE; row >= 1; row--) {
        printf("|");
        for (int p = 0; p < MAX_PLAYERS; p++) {
            char token = ' ';
            if (shared_state->player_active[p] && 
                shared_state->player_positions[p] == row) {
                token = shared_state->player_names[p][0];
            }
            printf("  %c  |", token);
        }
        printf(" R%-2d\n", row);
        printf("-------------------------------\n");
    }

    // 3. Start Line
    printf("|");
    for (int p = 0; p < MAX_PLAYERS; p++) {
        char token = ' ';
        if (shared_state->player_active[p] && 
            shared_state->player_positions[p] == 0) {
            token = shared_state->player_names[p][0];
        }
        printf("  %c  |", token);
    }
    printf(" Start (R0)\n");
    printf("-------------------------------\n");

    // 4. Player Positions Table (CLEANED UP)
    printf("\n   Current Standing:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shared_state->player_active[i]) {
            // --- REMOVED (YOU) AND (*) ---
            printf("   %-10s | R%-2d\n", 
                   shared_state->player_names[i], 
                   shared_state->player_positions[i]);
            // -----------------------------
        }
    }
    
    // 5. Messages
    printf("\n------------------------------------------\n");
    if (last_action && strlen(last_action) > 0) printf(">> %s\n", last_action);
    if (current_status) printf(">> %s\n", current_status);
}

void play_game() {
    char fifo_write[256], fifo_read[256];
    snprintf(fifo_write, sizeof(fifo_write), "%s%d_to_server", FIFO_PREFIX, player_id);
    snprintf(fifo_read, sizeof(fifo_read), "%s%d_from_server", FIFO_PREFIX, player_id);
    
    int fd_write = open(fifo_write, O_WRONLY);
    int fd_read = open(fifo_read, O_RDONLY);
    
    if (fd_write == -1 || fd_read == -1) return;
    
    srand(time(NULL) ^ getpid());
    
    char my_last_action[256] = ""; 
    char current_status[256];

    while (shared_state->game_active) {
        
        // If not your turn
        if (shared_state->current_turn != player_id) {
            snprintf(current_status, sizeof(current_status), "Waiting for %s...", 
                   shared_state->player_names[shared_state->current_turn]);
            
            display_grid(my_last_action, current_status);
            sleep(1); 
            continue; 
        }

        // If your turn
        display_grid(my_last_action, "YOUR TURN! Press ENTER to roll...");
        getchar();

        char buffer[256] = "ROLL";
        write(fd_write, buffer, strlen(buffer) + 1);

        memset(buffer, 0, sizeof(buffer));
        if (read(fd_read, buffer, sizeof(buffer)) > 0) {
            int dice_roll;
            sscanf(buffer, "ROLLED %d", &dice_roll);

            snprintf(my_last_action, sizeof(my_last_action), 
                "You rolled a %d! Moved to R%d.", 
                dice_roll, shared_state->player_positions[player_id]);
            
            display_grid(my_last_action, "Turn complete.");
            sleep(2); 
        }
    }
    close(fd_write);
    close(fd_read);
}

void cleanup() {
    if (shared_state != NULL) munmap(shared_state, sizeof(GameState));
    char fifo_name[256];
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
}
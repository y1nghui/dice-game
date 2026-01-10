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

// Shared memory structure (must match server)
typedef struct {
    int player_positions[MAX_PLAYERS];
    int current_turn;
    int game_active;
    int players_connected;
    int player_active[MAX_PLAYERS];
    int game_winner;
    char player_names[MAX_PLAYERS][50];
    pthread_mutex_t game_mutex;
    pthread_mutex_t log_mutex;
} GameState;

GameState *shared_state = NULL;
int player_id = -1;
char player_name[50];

void setup_shared_memory();
void create_fifos();
void display_game_state();
void play_game();
void cleanup();
int find_available_slot();

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <username>\n", argv[0]);
        printf("\nExample:\n");
        printf("  Terminal 1: ./client alice\n");
        printf("  Terminal 2: ./client bob\n");
        printf("  Terminal 3: ./client charlie\n");
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
    
    int last_count = -1;
    while (!shared_state->game_active) {
        int current_count = shared_state->players_connected;
        if (current_count != last_count) {
            last_count = current_count;
            printf("Players connected: %d/%d\n", current_count, MAX_PLAYERS);
        }
        sleep(1);
    }
    
    printf("\nGame Started! Race to %d!\n", WINNING_SCORE);
    printf("===============================\n\n");
    
    play_game();
    
    printf("\n-----------------------------\n");
    if (shared_state->game_winner == player_id) {
        printf("CONGRATULATIONS! YOU WON!\n");
    } else {
        printf("Game Over! %s won the game!\n", shared_state->player_names[shared_state->game_winner]);
    }
    printf("\n-----------------------------\n");
    
    cleanup();
    
    return 0;
}

void setup_shared_memory() {
    int shm_fd = shm_open("/dice_game_shm", O_RDWR, 0666);
    if (shm_fd == -1) {
        fprintf(stderr, "ERROR: Could not connect to server: %s\n", strerror(errno));
        fprintf(stderr, "\nIs the server running? Start the server first with: ./server\n");
        exit(1);
    }
    
    shared_state = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (shared_state == MAP_FAILED) {
        fprintf(stderr, "ERROR: Memory mapping failed: %s\n", strerror(errno));
        close(shm_fd);
        exit(1);
    }
    
    close(shm_fd);
}

int find_available_slot() {
    pthread_mutex_lock(&shared_state->game_mutex);
    
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!shared_state->player_active[i]) {
            slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&shared_state->game_mutex);
    return slot;
}

void create_fifos() {
    char fifo_name[256];
    
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) {
        fprintf(stderr, "Warning: Could not create FIFO %s: %s\n", 
                fifo_name, strerror(errno));
    }
    
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) {
        fprintf(stderr, "Warning: Could not create FIFO %s: %s\n", 
                fifo_name, strerror(errno));
    }
}

void display_game_state() {
    printf("\n");
    int first = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (shared_state->player_active[i]) {
            if (!first) printf(", ");
            first = 0;
            
            printf("%s: %d", shared_state->player_names[i], shared_state->player_positions[i]);
            
            if (i == player_id) {
                printf(" (YOU)");
            }
            if (i == shared_state->current_turn) {
                printf(" <- Turn");
            }
        }
    }
    printf("\n");
}

void play_game() {
    char fifo_write[256], fifo_read[256];
    snprintf(fifo_write, sizeof(fifo_write), "%s%d_to_server", FIFO_PREFIX, player_id);
    snprintf(fifo_read, sizeof(fifo_read), "%s%d_from_server", FIFO_PREFIX, player_id);
    
    srand(time(NULL) ^ getpid());
    
    while (shared_state->game_active) {
        if (shared_state->current_turn == player_id) {
            display_game_state();
            
            printf("\nYOUR TURN! Press ENTER to roll the dice...\n");
            fflush(stdout);
            getchar();
            
            int dice_roll = (rand() % 6) + 1;
            
            pthread_mutex_lock(&shared_state->game_mutex);
            shared_state->player_positions[player_id] += dice_roll;
            
            printf("You rolled a %d! ", dice_roll);
            printf("New position: %d/%d\n", 
                   shared_state->player_positions[player_id], WINNING_SCORE);
            
            if (shared_state->player_positions[player_id] >= WINNING_SCORE) {
                shared_state->game_winner = player_id;
                shared_state->game_active = 0;
                pthread_mutex_unlock(&shared_state->game_mutex);
                break;
            }
            
            shared_state->current_turn = (shared_state->current_turn + 1) % MAX_PLAYERS;
            pthread_mutex_unlock(&shared_state->game_mutex);
            
        } else {
            if (shared_state->player_active[shared_state->current_turn]) {
                printf("\rWaiting for %s's turn...     ", 
                       shared_state->player_names[shared_state->current_turn]);
            } else {
                printf("\rWaiting...     ");
            }
            fflush(stdout);
            sleep(1);
        }
    }
}

void cleanup() {
    if (shared_state != NULL) {
        munmap(shared_state, sizeof(GameState));
    }
    
    char fifo_name[256];
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
}
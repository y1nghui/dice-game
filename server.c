// Operating Systems Assignment - Group 8
// server.c
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

// Macro definitions
#define MAX_PLAYERS 3
#define WINNING_SCORE 20
#define FIFO_PREFIX "/tmp/player_"
#define SHM_NAME "/dice_game_shm"

// Global Variables / structs
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
char player_name[50]; // player username

// Function prototypes
void setup_shared_memory();


// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <username>\n", argv[0]);
        return 1;
    }
    
    strncpy(player_name, argv[1], sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';
    
    printf("=== Dice Race Game ===\n");
    printf("Player: %s\n", player_name);
    
    setup_shared_memory();
    
    return 0;
}

// Functions
void setup_shared_memory() {
    // Open existing shared memory
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to connect to server");
        exit(1);
    }

    // Map shared memory into address space
    shared_state = mmap(NULL, sizeof(GameState), 
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (shared_state == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
    
    close(shm_fd);
    
    printf("Connected to shared memory\n");
}

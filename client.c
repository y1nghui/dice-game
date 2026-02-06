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

// Game settings: 3-5 players, race to R20, each player uses unique FIFO path
#define MIN_P 3         // Minimum players to start
#define MAX_P 5         // Maximum players supported
#define WC 20           // WC = Winning Criteria / condition
#define FIFO_PREFIX "/tmp/player_"

// Shared memory structure
typedef struct 
{
    int PP[MAX_P];              // PP = player position
    int CT;                     // CT = current turn
    int game_active; 
    int CP;                     // CP = connected player
    int player_active[MAX_P];
    int FW;                     // FW = final winner
    int round;                  // round = number of current round 
    char PN[MAX_P][50];         // PN = player names/ client name
    int winner[MAX_P]; 
    pthread_mutex_t shm_lock;   // shm_lock = shared memory lock
    pthread_mutex_t table_sync; // table_sync = Syncing the table/grid display 
} GameInfo;                     // GameInfo = Game information / Game State 

GameInfo *g_ptr = NULL;         // g_ptr = game pointer 
int player_id = -1;
char player_name[50];

// pre-declaration of functions
void ssm();                     // ssm = setup shared memory
void c_fifos();                 // c_fifos = create fifos
void d_grid(const char *last_action, const char *current_status); // d_grid = display grid
void play_game();
void cleanup(); 
int fas();                      // fas = find available slot

int main(int argc, char *argv[]) {
    if (argc != 2) 
    { 
        printf("Please enter a valid instructions: %s (YourName)\n", argv[0]);
        return 1;
    }
    
    strncpy(player_name, argv[1], sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';
    
    printf("=== Dice Race Game - %-20s ===\n", player_name);
    
    ssm();
    
    player_id = fas();
    if (player_id == -1) 
    {
        fprintf(stderr, "ERROR: Game is full (%d players maximum)\n", MAX_P);
        cleanup();
        return 1;
    }
    
    // assigning player to the particular slot 
    printf("Assigned to Player slot %d\n", player_id + 1);
    pthread_mutex_lock(&g_ptr->shm_lock);
    strncpy(g_ptr->PN[player_id], player_name, 49);
    g_ptr->PN[player_id][49] = '\0';
    pthread_mutex_unlock(&g_ptr->shm_lock);
    
    c_fifos();
    printf("Connected to server as %s!\n", player_name);
    printf("Waiting for other players to join...\n\n");
    
    char join_msg[100];
    snprintf(join_msg, sizeof(join_msg), "%s Joined Game.", player_name);
    
    int last_count = -1;
    while (!g_ptr->game_active) 
    {
        int current_count = g_ptr->CP;
        if (current_count != last_count) 
        {
            last_count = current_count;
            
            if (current_count < MIN_P) 
            {
                printf("Players connected: %d/%d minimum needed\n", current_count, MIN_P);
                d_grid(join_msg, "Waiting for minimum players...");
            } 
            else 
            {
                printf("Players connected: %d (minimum met, max %d)\n", current_count, MAX_P);
                d_grid(join_msg, "Enough players! Game starting soon...");
            }
        }
        sleep(1);
    }
    
    printf("\nGame Started! Race to %d!\n", WC);
    printf("Playing with %d players\n\n", g_ptr->CP);
    
    play_game();
    
    d_grid("GAME OVER!", "Final Results");
    
    // Save results to file
    char filename[256];
    snprintf(filename, sizeof(filename), "result_%s.txt", player_name);
    
    FILE *f = fopen(filename, "w");
    if (f) 
    {
        fprintf(f, "Game Result for %s\n", player_name);
        fprintf(f, "Total Players: %d\n", g_ptr->CP);
        fprintf(f, "Winner: %s\n", g_ptr->PN[g_ptr->FW]);
        fprintf(f, "My Wins: %d\n", g_ptr->winner[player_id]);
        fprintf(f, "Final Position: R%d\n", g_ptr->PP[player_id]);
        fclose(f);
    }

    printf("\n##########################################\n");
    if (g_ptr->FW == player_id) 
    {
        printf("       CONGRATULATIONS! YOU WON!          \n");
    } 
    else 
    {
        printf("       Game Over! %s won the game!        \n", 
               g_ptr->PN[g_ptr->FW]);
    }
    printf("##########################################\n");
    
    printf("   Total Wins for %s: %d\n", player_name, g_ptr->winner[player_id]);
    printf("------------------------------------------\n");
    
    cleanup();
    return 0;
}

void ssm() 
{
    int shm_fd = shm_open("/dice_game_shm", O_RDWR, 0666);
    if (shm_fd == -1) 
    {
        fprintf(stderr, "ERROR: Could not connect to server: %s\n", strerror(errno));
        exit(1);
    }
    g_ptr = mmap(NULL, sizeof(GameInfo), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_ptr == MAP_FAILED) 
    {
        perror("mmap failed");
        exit(1);
    }
    close(shm_fd);
}

int fas() 
{
    pthread_mutex_lock(&g_ptr->shm_lock);
    int slot = -1; 
    for (int i = 0; i < MAX_P; i++) 
    {
        if (!g_ptr->player_active[i]) 
        {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_ptr->shm_lock);
    return slot;
}

void c_fifos() 
{
    char fifo_name[256];
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) perror("mkfifo");
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    if (mkfifo(fifo_name, 0666) == -1 && errno != EEXIST) perror("mkfifo");
}

void d_grid(const char *last_action, const char *current_status) 
{
    printf("\033[H\033[J"); // Clear Screen
    
    // 1. Header
    printf("==========================================\n");
    printf("       DICE RACE - ROUND %d\n", g_ptr->round);
    printf("       (%d Players Active)\n", g_ptr->CP);
    printf("==========================================\n");
    
    // 2. The Grid - Build columns dynamically based on active players
    for (int row = WC; row >= 1; row--) 
    {
        printf("|");
        for (int p = 0; p < MAX_P; p++) 
        {
            if (g_ptr->player_active[p]) 
            {
                char token = ' ';
                if (g_ptr->PP[p] == row) 
                {
                    token = g_ptr->PN[p][0];
                }
                printf("  %c  |", token);
            }
        }
        printf(" R%-2d\n", row);
        
        // Print separator (adjust width based on active players)
        printf("------");
        for (int p = 0; p < MAX_P; p++) 
        {
            if (g_ptr->player_active[p]) 
            {
                printf("------");
            }
        }
        printf("\n");
    }
    
    // 3. Start Line
    printf("|");
    for (int p = 0; p < MAX_P; p++) 
    {
        if (g_ptr->player_active[p]) 
        {
            char token = ' ';
            if (g_ptr->PP[p] == 0) 
            {
                token = g_ptr->PN[p][0];
            }
            printf("  %c  |", token);
        }
    }
    printf(" Start (R0)\n");
    
    printf("------");
    for (int p = 0; p < MAX_P; p++) 
    {
        if (g_ptr->player_active[p]) 
        {
            printf("------");
        }
    }
    printf("\n");
    
    // 4. Player Positions Table 
    printf("\n   Current Standing:\n");
    for (int i = 0; i < MAX_P; i++) 
    {
        if (g_ptr->player_active[i]) 
        {
            char marker = (i == player_id) ? '*' : ' ';
            printf(" %c %-10s | R%-2d\n", 
                   marker,
                   g_ptr->PN[i], 
                   g_ptr->PP[i]);
        }
    }
    
    // 5. Messages
    printf("\n------------------------------------------\n");
    if (last_action && strlen(last_action) > 0) 
        printf(">> %s\n", last_action);
    if (current_status) 
        printf(">> %s\n", current_status);
}

void play_game() 
{
    char fifo_write[256], fifo_read[256];
    snprintf(fifo_write, sizeof(fifo_write), "%s%d_to_server", FIFO_PREFIX, player_id);
    snprintf(fifo_read, sizeof(fifo_read), "%s%d_from_server", FIFO_PREFIX, player_id);
    
    int fd_write = open(fifo_write, O_WRONLY);
    int fd_read = open(fifo_read, O_RDONLY);
    
    if (fd_write == -1 || fd_read == -1) 
    {
        fprintf(stderr, "Failed to open FIFO pipes\n");
        return;
    }
    
    srand(time(NULL) ^ getpid());
    
    char my_last_action[256] = ""; 
    char current_status[256];
    
    // Track last displayed state to avoid unnecessary redraws
    int last_displayed_turn = -1;

    while (g_ptr->game_active) 
    {
        int current_turn = g_ptr->CT;
        
        // Only redraw if the turn changed
        if (current_turn != last_displayed_turn) 
        {
            if (current_turn != player_id) 
            {
                snprintf(current_status, sizeof(current_status), 
                        "Waiting for %s...", g_ptr->PN[current_turn]);
                d_grid(my_last_action, current_status);
            } 
            else 
            {
                d_grid(my_last_action, "YOUR TURN! Press ENTER to roll...");
            }
            last_displayed_turn = current_turn;
        }
        
        // If not my turn, wait and continue
        if (current_turn != player_id) 
        {
            usleep(100000); // Sleep 0.1 second
            continue; 
        }

        // My turn - wait for user input
        getchar();

        // Send ROLL command
        char buffer[256] = "ROLL";
        write(fd_write, buffer, strlen(buffer) + 1);

        // Receive dice result
        memset(buffer, 0, sizeof(buffer));
        if (read(fd_read, buffer, sizeof(buffer)) > 0) 
        {
            int roll; 
            sscanf(buffer, "ROLLED %d", &roll);

            snprintf(my_last_action, sizeof(my_last_action), 
                "You rolled a %d! Moved to R%d.", 
                roll, g_ptr->PP[player_id]);
            
            d_grid(my_last_action, "Turn complete.");
            sleep(1);
            
            // Force redraw next iteration to show updated positions
            last_displayed_turn = -1;
        }
    }
    
    close(fd_write);
    close(fd_read);
}

void cleanup() 
{
    if (g_ptr != NULL) 
        munmap(g_ptr, sizeof(GameInfo));
    
    char fifo_name[256];
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_to_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
    snprintf(fifo_name, sizeof(fifo_name), "%s%d_from_server", FIFO_PREFIX, player_id);
    unlink(fifo_name);
}
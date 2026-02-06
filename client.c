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
#include <sys/select.h>

// setting: min 3 players, and max 5 players, the first player race to R20 will be the winner, each player uses unique FIFO path
#define MXP 5 // mxp = maximum 5 player
#define WC 20 // wc= win condition
#define fifo_p "/tmp/player_" // fifo_p = fifo prefox 

// Game state structure
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
    int TW[MXP]; //TWN = total winning
    pthread_mutex_t shm_lock;
    pthread_mutex_t table_sync;
    int mnpr; // mnpr = min player require
};

struct GameInfo *gptr = NULL; // gptr = game pointer
int my_player_id = -1;
char my_name[50];

// Function declarations
void ssm(); // ssm = setting share memeory 
void Cfifo(); // Cfifo = create fifo 
void sg(const char *last_action, const char *current_status); // sg = show grid 
void play();
void cr(); // cr = clean resourcws 
int Fslot(); // Fslot = find available slot 
int winput(); // winput = waiting for input 

// Wait for user input with timeout
int winput() 
{
    fd_set read_fds;
    struct timeval timeout;
    
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    
    int result;
    result = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
    
    if (result > 0) 
    {
        getchar();
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) 
{
    if (argc != 2) 
    {
        printf("Usage: %s <YourName>\n", argv[0]);
        printf("Example: %s Alice\n", argv[0]);
        return 1;
    }
    
    strncpy(my_name, argv[1], sizeof(my_name) - 1);
    my_name[sizeof(my_name) - 1] = '\0';
    
    printf("===========================================\n");
    printf("  Dice Race Game Player\n");
    printf("  Player: %s\n", my_name);
    printf("===========================================\n");
    
    ssm();
    
    my_player_id = Fslot();
    
    if (my_player_id == -1) 
    {
        fprintf(stderr, "ERROR: Game is full (%d players maximum)\n", MXP);
        cr();
        return 1;
    }
    
    printf("Assigned to slot: %d\n", my_player_id + 1);
    
    pthread_mutex_lock(&gptr->shm_lock);
    strncpy(gptr->PN[my_player_id], my_name, 49);
    gptr->PN[my_player_id][49] = '\0';
    pthread_mutex_unlock(&gptr->shm_lock);
    
    Cfifo();
    printf("Connected to server successfully!\n");
    printf("Waiting for other players to join...\n");
    printf("(Minimum %d players required)\n\n", gptr->mnpr);
    
    char join_message[100];
    snprintf(join_message, sizeof(join_message), "%s joined the game", my_name);
    
    int lastPC;// lastPC = last player count
    lastPC = -1;
    
    while (gptr->game_active == 0) 
    {
        int cc; // cc = current count
        cc = gptr->CP;
        
        if (cc != lastPC) 
        {
            lastPC = cc;
            printf("Players connected: %d/%d (minimum)\n", 
                   cc, gptr->mnpr);
            sg(join_message, "Waiting for players...");
        }
        sleep(1);
    }
    
    printf("\n===========================================\n");
    printf("  Game Started!\n");
    printf("  Race to position %d!\n", WC);
    printf("===========================================\n\n");
    
    play();
    
    sleep(1);
    
    sg("GAME OVER!", "Final Results");
    
    char result_filename[256];
    snprintf(result_filename, sizeof(result_filename), "result_%s.txt", my_name);
    
    FILE *result_file;
    result_file = fopen(result_filename, "w");
    
    if (result_file != NULL) 
    {
        fprintf(result_file, "Game Results for %s\n", my_name);
        fprintf(result_file, "Winner: %s\n", gptr->PN[gptr->FW]);
        fprintf(result_file, "My Total Wins: %d\n", gptr->TW[my_player_id]);
        fprintf(result_file, "Final Position: R%d\n", gptr->PP[my_player_id]);
        fclose(result_file);
    }

    printf("\n##########################################\n");
    if (gptr->FW == my_player_id) 
    {
        printf("#                                        #\n");
        printf("#     CONGRATULATIONS! YOU WON!          #\n");
        printf("#                                        #\n");
    } 
    else 
    {
        printf("#                                        #\n");
        printf("#      Game Over! %s won the game!       #\n", 
               gptr->PN[gptr->FW]);
        printf("#                                        #\n");
    }
    printf("##########################################\n");
    
    printf("\nTotal Wins for %s: %d\n", my_name, gptr->TW[my_player_id]);
    printf("------------------------------------------\n");
    
    cr();
    return 0;
}

void ssm() 
{
    int shm_fd;
    shm_fd = shm_open("/dice_game_shm", O_RDWR, 0666);
    
    if (shm_fd == -1) 
    {
        fprintf(stderr, "ERROR: Cannot connect to server\n");
        fprintf(stderr, "Error: %s\n", strerror(errno));
        fprintf(stderr, "Make sure server is running!\n");
        exit(1);
    }
    
    gptr = mmap(NULL, sizeof(struct GameInfo), 
                    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    if (gptr == MAP_FAILED) 
    {
        perror("Memory mapping failed");
        exit(1);
    }
    
    close(shm_fd);
}

int Fslot() 
{
    pthread_mutex_lock(&gptr->shm_lock);
    
    int available_slot;
    available_slot = -1;
    
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (gptr->player_active[i] == 0) 
        {
            available_slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&gptr->shm_lock);
    return available_slot;
}

void Cfifo() 
{
    char fifo_path[256];
    
    snprintf(fifo_path, sizeof(fifo_path), "%s%d_to_server", fifo_p, my_player_id);
    int result;
    result = mkfifo(fifo_path, 0666);
    if (result == -1 && errno != EEXIST) 
    {
        perror("FIFO creation failed");
    }
    
    snprintf(fifo_path, sizeof(fifo_path), "%s%d_from_server", fifo_p, my_player_id);
    result = mkfifo(fifo_path, 0666);
    if (result == -1 && errno != EEXIST) 
    {
        perror("FIFO creation failed");
    }
}

void sg(const char *last_action, const char *current_status) 
{
    printf("\033[H\033[J");
    
    printf("==========================================\n");
    printf("    DICE RACE - ROUND %d\n", gptr->round);
    printf("==========================================\n");
    
    int row;
    for (row = WC; row >= 1; row = row - 1) 
    {
        printf("|");
        
        int p;
        for (p = 0; p < MXP; p = p + 1) 
        {
            char display_char;
            display_char = ' ';
            
            if (gptr->player_active[p] == 1 && gptr->PP[p] == row) 
            {
                display_char = gptr->PN[p][0];
            }
            
            printf("  %c  |", display_char);
        }
        
        printf(" R%-2d\n", row);
        printf("------------------------------------------------------\n");
    }
    
    printf("|");
    int p;
    for (p = 0; p < MXP; p = p + 1) 
    {
        char display_char;
        display_char = ' ';
        
        if (gptr->player_active[p] == 1 && gptr->PP[p] == 0) 
        {
            display_char = gptr->PN[p][0];
        }
        
        printf("  %c  |", display_char);
    }
    printf(" Start (R0)\n");
    printf("------------------------------------------------------\n");
    
    printf("\nCurrent Standings:\n");
    int i;
    for (i = 0; i < MXP; i = i + 1) 
    {
        if (gptr->player_active[i] == 1) 
        {
            printf("  %-10s | Position: R%-2d\n", 
                   gptr->PN[i], 
                   gptr->PP[i]);
        }
    }
    
    printf("\n------------------------------------------\n");
    
    if (last_action != NULL && strlen(last_action) > 0) 
    {
        printf(">> %s\n", last_action);
    }
    
    if (current_status != NULL) 
    {
        printf(">> %s\n", current_status);
    }
}

void play() 
{
    char fifo_write_path[256];
    char fifo_read_path[256];
    
    snprintf(fifo_write_path, sizeof(fifo_write_path), 
             "%s%d_to_server", fifo_p, my_player_id);
    snprintf(fifo_read_path, sizeof(fifo_read_path), 
             "%s%d_from_server", fifo_p, my_player_id);
    
    int fd_write;
    int fd_read;
    
    fd_write = open(fifo_write_path, O_WRONLY);
    fd_read = open(fifo_read_path, O_RDONLY);
    
    if (fd_write == -1 || fd_read == -1) 
    {
        return;
    }
    
    srand(time(NULL) ^ getpid());
    
    char my_last_action[256];
    strcpy(my_last_action, "");
    
    char current_status[256];

    while (1) 
    {
        if (gptr->game_active ==0) 
        {
            break;
        }
        
        if (gptr->CT != my_player_id) 
        {
            snprintf(current_status, sizeof(current_status), 
                     "Waiting for %s's turn...", 
                     gptr->PN[gptr->CT]);
            
            sg(my_last_action, current_status);
            usleep(500000);
            continue;
        }

        if (gptr->game_active ==0) {
            break;
        }

        int key_pressed;
        key_pressed=0;
        
        while (gptr->game_active == 1 && key_pressed ==0){
            sg(my_last_action, "YOUR TURN! Press ENTER to roll...");
            key_pressed = winput();
            
            if (gptr->game_active ==0) {
                break;
            }
        }

        if (gptr->game_active ==0) {
            break;
        }

        char buffer[256];
        strcpy(buffer, "ROLL");
        write(fd_write, buffer, strlen(buffer) + 1);

        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read;
        bytes_read = read(fd_read, buffer, sizeof(buffer));
        
        if (bytes_read > 0) 
        {
            int dice_value;
            sscanf(buffer, "ROLLED %d", &dice_value);

            snprintf(my_last_action, sizeof(my_last_action), "You rolled a %d! Moved to R%d", dice_value, gptr->PP[my_player_id]);
            
            sg(my_last_action, "Turn completed");
            
            if (gptr->game_active == 0) 
            {
                break;
            }
            
            sleep(2);
        }
        
        if (gptr->game_active == 0) 
        {
            break;
        }
    }
    
    close(fd_write);
    close(fd_read);
}

void cr() 
{
    if (gptr != NULL) 
    {
        munmap(gptr, sizeof(struct GameInfo));
    }
    
    char fifo_path[256];
    
    snprintf(fifo_path, sizeof(fifo_path), "%s%d_to_server", fifo_p, my_player_id);
    unlink(fifo_path);
    
    snprintf(fifo_path, sizeof(fifo_path), "%s%d_from_server", fifo_p, my_player_id);
    unlink(fifo_path);
}
# Operating Systems Assignment - Group 8

CC = gcc
CFLAGS = -Wall -pthread -g
LIBS = -lrt -pthread

# Default target
all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c $(LIBS)

client: client.c
	$(CC) $(CFLAGS) -o client client.c $(LIBS)

# Clean build artifacts and runtime files
clean:
	rm -f server client game.log scores.txt
	rm -f /tmp/player_*
	rm -f core

# Clean everything including shared memory
cleanall: clean
	rm -f /dev/shm/dice_game_shm

.PHONY: all clean cleanall
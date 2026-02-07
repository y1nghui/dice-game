# OS Assignment - Dice Roll Game - Group 8

==============================
HOW TO COMPILE (MAKE) AND RUN
==============================

COMPILATION:
------------
    $ make clean
    $ make

RUNNING THE GAME:
-----------------

STEP 1: Start the Server
-------------------------
    $ ./server

STEP 2: Connect Clients (in separate terminals)
------------------------------------------------
Minimum 3 players required, maximum 5 players allowed.

    $ ./client Alice
    $ ./client Bob
    $ ./client Charlie

Optional 4th and 5th players:
    $ ./client David
    $ ./client Eve

STEP 3: Play the Game
----------------------
- Wait for your turn
- Press ENTER when prompted "YOUR TURN! Press ENTER to roll"
- First player to reach R20 wins

STEP 4: End the Game
--------------------
The game ends automatically when a player reaches R20.
To force quit: Press Ctrl+C in the server terminal.

Clean up after game:
    $ make clean

==================
GAME RULES SUMMARY
==================

OBJECTIVE:
----------
Be the first player to reach position R20 (Row 20).

PLAYERS:
--------
- Minimum: 3 players
- Maximum: 5 players
- Each player must provide a unique name

GAMEPLAY:
---------
1. Game starts when minimum 3 players connect
2. Turns proceed in Round Robin order
3. On your turn, press ENTER to roll the dice
4. Dice generates random number 1-6
5. Your position advances by the dice value
6. First player to reach R20 wins

RULES:
------
- Players assigned to slots (1-5) on first-come, first-served basis
- Turn order: Player 0 → 1 → 2 → 3 → 4 → 0 (cycle repeats)
- Inactive/disconnected players are automatically skipped
- Winner's score is saved to scores.txt
- Game board displays rows R0 (Start) to R20 (Finish)
- Each player represented by first letter of their name

===============
MODE SUPPORTED
===============

MODE: Single-Machine (IPC Mode)
--------------------------------

DEPLOYMENT:
- All components run on the same Linux machine
- No network/multi-machine support

COMMUNICATION MECHANISMS:
-------------------------
1. POSIX Shared Memory
   - Path: /dice_game_shm
   - Purpose: Store game state (positions, current turn, player status)
   - All clients and server read/write to same memory segment

2. Named Pipes (FIFOs)
   - Location: /tmp/player_X_to_server (client sends commands)
   - Location: /tmp/player_X_from_server (client receives responses)
   - Purpose: Bidirectional command/response communication
   - Each client has 2 unique FIFO pipes

3. Process-Shared Mutexes
   - Type: PTHREAD_PROCESS_SHARED
   - Purpose: Synchronize access to shared memory
   - Prevents race conditions

ARCHITECTURE:
-------------
Server Side:
- 1 main server process
- 2 POSIX threads (logger thread + scheduler thread)
- 3-5 child processes (forked for each connected client)

Client Side:
- Each client runs as separate process
- Connects to shared memory for reading game state
- Uses FIFO pipes for sending/receiving commands

IPC SUMMARY:
------------
- Shared Memory: Fast read access to game state
- FIFO Pipes: Reliable command/response communication
- Mutexes: Thread-safe concurrent access

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ncurses.h>

// Constants
#define NUM_PROCESSES 3
#define MAX_BLOCKS_DEFAULT 20

// Shared ledger structure
typedef struct {
    int block_id;
    int process_id;
    time_t timestamp;
    char data[256];
    unsigned long nonce;
    char prev_hash[65];
} Block;

Block *ledger;
int block_count = 0;
int max_blocks = MAX_BLOCKS_DEFAULT;
int difficulty = 4;

// Synchronization primitives
sem_t ledger_sem;
pthread_mutex_t block_mutex;
pthread_cond_t block_cond;
int block_solved = 0;
int current_block = 0;
int simulation_running = 0;

// ncurses windows
WINDOW *status_win, *control_win;
pthread_mutex_t ncurses_mutex = PTHREAD_MUTEX_INITIALIZER;

// Simple hash function
unsigned long simple_hash(const char *data, unsigned long nonce, const char *prev_hash) {
    unsigned long hash = 0;
    char input[512];
    snprintf(input, sizeof(input), "%s%s%lu", prev_hash, data, nonce);
    for (int i = 0; input[i]; i++) {
        hash = hash * 31 + input[i];
    }
    return hash;
}

// Check if hash meets difficulty
int meets_difficulty(unsigned long hash) {
    unsigned long mask = (1UL << (difficulty * 4)) - 1;
    return (hash & mask) == 0;
}

// Mining function
void *mine(void *arg) {
    int process_id = *(int *)arg;
    char data[256];
    unsigned long nonce;
    char prev_hash[65] = "0";

    while (1) {
        pthread_mutex_lock(&block_mutex);
        while (!simulation_running || current_block >= max_blocks) {
            pthread_cond_wait(&block_cond, &block_mutex);
            if (!simulation_running && current_block >= max_blocks) {
                pthread_mutex_unlock(&block_mutex);
                return NULL;
            }
        }
        snprintf(data, sizeof(data), "P%d ran for 10ms, block %d", process_id, current_block);

        while (block_solved && simulation_running) {
            pthread_cond_wait(&block_cond, &block_mutex);
        }
        if (!simulation_running || current_block >= max_blocks) {
            pthread_mutex_unlock(&block_mutex);
            break;
        }
        pthread_mutex_unlock(&block_mutex);

        pthread_mutex_lock(&ncurses_mutex);
        wprintw(status_win, "Process %d working on PoW (nonce: 0)...\n", process_id);
        wrefresh(status_win);
        pthread_mutex_unlock(&ncurses_mutex);

        nonce = 0;
        while (simulation_running && current_block < max_blocks) {
            unsigned long hash = simple_hash(data, nonce, prev_hash);
            if (meets_difficulty(hash)) {
                pthread_mutex_lock(&block_mutex);
                if (!block_solved && current_block < max_blocks) {
                    block_solved = 1;

                    pthread_mutex_lock(&ncurses_mutex);
                    wprintw(status_win, "Process %d solved PoW with nonce %lu\n", process_id, nonce);
                    wrefresh(status_win);
                    pthread_mutex_unlock(&ncurses_mutex);

                    sem_wait(&ledger_sem);
                    ledger[block_count].block_id = block_count;
                    ledger[block_count].process_id = process_id;
                    ledger[block_count].timestamp = time(NULL);
                    strncpy(ledger[block_count].data, data, sizeof(ledger[block_count].data));
                    ledger[block_count].nonce = nonce;
                    strncpy(ledger[block_count].prev_hash, prev_hash, sizeof(ledger[block_count].prev_hash));
                    block_count++;
                    sem_post(&ledger_sem);

                    snprintf(prev_hash, sizeof(prev_hash), "%lx", hash);
                    current_block++;
                    block_solved = 0;
                    pthread_cond_broadcast(&block_cond);
                }
                pthread_mutex_unlock(&block_mutex);
                break;
            }
            nonce++;
            if (nonce % 10000 == 0) {
                usleep(10000); // 10ms delay to slow down for visibility
            }
        }
    }
    return NULL;
}

// Print the ledger in status window
void print_ledger() {
    pthread_mutex_lock(&ncurses_mutex);
    wclear(status_win);
    wprintw(status_win, "=== Ledger ===\n");
    for (int i = 0; i < block_count; i++) {
        char time_str[26];
        ctime_r(&ledger[i].timestamp, time_str);
        time_str[strlen(time_str) - 1] = '\0';
        wprintw(status_win, "Block %2d | Proc %d | %s | %s | Nonce: %lu | Prev Hash: %.8s...\n",
                ledger[i].block_id, ledger[i].process_id, time_str,
                ledger[i].data, ledger[i].nonce, ledger[i].prev_hash);
    }
    wprintw(status_win, "=============\n");
    wrefresh(status_win);
    pthread_mutex_unlock(&ncurses_mutex);
}

// Update control window
void update_control_win(int highlight) {
    const char *options[] = {
        "1. Start simulation",
        "2. Stop simulation",
        "3. Set max blocks",
        "4. Set difficulty",
        "5. View ledger",
        "6. Exit"
    };
    int num_options = 6;

    pthread_mutex_lock(&ncurses_mutex);
    wclear(control_win);
    box(control_win, 0, 0);
    mvwprintw(control_win, 1, 2, "Mining Simulation Menu");
    mvwprintw(control_win, 2, 2, "Max Blocks: %d | Difficulty: %d | Blocks Mined: %d | %s",
              max_blocks, difficulty, block_count, simulation_running ? "Running" : "Stopped");
    for (int i = 0; i < num_options; i++) {
        if (i == highlight) {
            wattron(control_win, A_REVERSE);
            mvwprintw(control_win, i + 4, 2, "%s", options[i]);
            wattroff(control_win, A_REVERSE);
        } else {
            mvwprintw(control_win, i + 4, 2, "%s", options[i]);
        }
    }
    wrefresh(control_win);
    pthread_mutex_unlock(&ncurses_mutex);
}

// UI thread function
void *ui_thread(void *arg) {
    int highlight = 0;
    int num_options = 6;
    int ch;

    while (1) {
        update_control_win(highlight);
        ch = getch();

        switch (ch) {
            case KEY_UP:
                highlight = (highlight == 0) ? num_options - 1 : highlight - 1;
                break;
            case KEY_DOWN:
                highlight = (highlight == num_options - 1) ? 0 : highlight + 1;
                break;
            case 10: // Enter key
                switch (highlight) {
                    case 0: // Start simulation
                        pthread_mutex_lock(&block_mutex);
                        if (!simulation_running) {
                            simulation_running = 1;
                            block_solved = 0;
                            current_block = block_count; // Resume from last block
                            pthread_cond_broadcast(&block_cond);
                            pthread_mutex_lock(&ncurses_mutex);
                            wprintw(status_win, "Simulation started.\n");
                            wrefresh(status_win);
                            pthread_mutex_unlock(&ncurses_mutex);
                        }
                        pthread_mutex_unlock(&block_mutex);
                        break;

                    case 1: // Stop simulation
                        pthread_mutex_lock(&block_mutex);
                        if (simulation_running) {
                            simulation_running = 0;
                            pthread_cond_broadcast(&block_cond);
                            pthread_mutex_lock(&ncurses_mutex);
                            wprintw(status_win, "Simulation stopped.\n");
                            wrefresh(status_win);
                            pthread_mutex_unlock(&ncurses_mutex);
                        }
                        pthread_mutex_unlock(&block_mutex);
                        break;

                    case 2: // Set max blocks
                        pthread_mutex_lock(&ncurses_mutex);
                        echo();
                        wclear(control_win);
                        box(control_win, 0, 0);
                        mvwprintw(control_win, 1, 2, "Enter max blocks (1-100): ");
                        wrefresh(control_win);
                        char input[10];
                        wgetnstr(control_win, input, sizeof(input));
                        int new_max = atoi(input);
                        noecho();
                        if (new_max > 0 && new_max <= 100) {
                            pthread_mutex_lock(&block_mutex);
                            max_blocks = new_max;
                            ledger = realloc(ledger, max_blocks * sizeof(Block));
                            if (ledger == NULL) {
                                endwin();
                                perror("realloc failed");
                                exit(1);
                            }
                            pthread_mutex_unlock(&block_mutex);
                            wprintw(status_win, "Max blocks set to %d.\n", max_blocks);
                        } else {
                            wprintw(status_win, "Invalid input. Must be 1-100.\n");
                        }
                        wrefresh(status_win);
                        pthread_mutex_unlock(&ncurses_mutex);
                        break;

                    case 3: // Set difficulty
                        pthread_mutex_lock(&ncurses_mutex);
                        echo();
                        wclear(control_win);
                        box(control_win, 0, 0);
                        mvwprintw(control_win, 1, 2, "Enter difficulty (1-8): ");
                        wrefresh(control_win);
                        wgetnstr(control_win, input, sizeof(input));
                        int new_diff = atoi(input);
                        noecho();
                        if (new_diff >= 1 && new_diff <= 8) {
                            difficulty = new_diff;
                            wprintw(status_win, "Difficulty set to %d.\n", difficulty);
                        } else {
                            wprintw(status_win, "Invalid input. Must be 1-8.\n");
                        }
                        wrefresh(status_win);
                        pthread_mutex_unlock(&ncurses_mutex);
                        break;

                    case 4: // View ledger
                        print_ledger();
                        break;

                    case 5: // Exit
                        pthread_mutex_lock(&block_mutex);
                        simulation_running = 0;
                        pthread_cond_broadcast(&block_cond);
                        pthread_mutex_unlock(&block_mutex);
                        pthread_mutex_lock(&ncurses_mutex);
                        wprintw(status_win, "Exiting...\n");
                        wrefresh(status_win);
                        pthread_mutex_unlock(&ncurses_mutex);
                        endwin();
                        return NULL;
                }
                break;
        }
    }
    return NULL;
}

int main() {
    pthread_t threads[NUM_PROCESSES];
    pthread_t ui;
    int process_ids[NUM_PROCESSES];

    // Initialize ncurses
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    // Create windows
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    status_win = newwin(max_y - 10, max_x, 0, 0);
    scrollok(status_win, TRUE);
    control_win = newwin(10, max_x, max_y - 10, 0);

    // Dynamically allocate ledger
    ledger = malloc(max_blocks * sizeof(Block));
    if (ledger == NULL) {
        endwin();
        perror("malloc failed");
        exit(1);
    }

    // Initialize synchronization primitives
    if (sem_init(&ledger_sem, 0, 1) != 0) {
        endwin();
        perror("sem_init failed");
        exit(1);
    }
    if (pthread_mutex_init(&block_mutex, NULL) != 0) {
        endwin();
        perror("pthread_mutex_init failed");
        exit(1);
    }
    if (pthread_cond_init(&block_cond, NULL) != 0) {
        endwin();
        perror("pthread_cond_init failed");
        exit(1);
    }

    // Create mining threads
    for (int i = 0; i < NUM_PROCESSES; i++) {
        process_ids[i] = i;
        if (pthread_create(&threads[i], NULL, mine, &process_ids[i]) != 0) {
            endwin();
            perror("pthread_create failed");
            exit(1);
        }
    }

    // Create UI thread
    if (pthread_create(&ui, NULL, ui_thread, NULL) != 0) {
        endwin();
        perror("pthread_create UI failed");
        exit(1);
    }

    // Wait for UI thread to finish
    pthread_join(ui, NULL);

    // Wait for mining threads to finish
    for (int i = 0; i < NUM_PROCESSES; i++) {
        pthread_join(threads[i], NULL);
    }

    // Cleanup
    free(ledger);
  sem_destroy(&ledger_sem);
    pthread_mutex_destroy(&block_mutex);
    pthread_cond_destroy(&block_cond);
    delwin(status_win);
    delwin(control_win);
    endwin();

    printf("Simulation ended.\n");
    return 0;
}

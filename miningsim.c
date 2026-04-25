#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include <ncurses.h>

/* ── Constants ──────────────────────────────────────────────────────────────*/
#define NUM_MINERS          3
#define MAX_BLOCKS_CAP      100
#define MAX_BLOCKS_DEFAULT  20
#define LOG_RING_SIZE       512
#define LOG_MSG_LEN         200

/* ── Block ──────────────────────────────────────────────────────────────────*/
typedef struct {
    int           block_id;
    int           miner_id;
    time_t        timestamp;
    char          data[256];
    unsigned long nonce;
    char          prev_hash[65];
    char          this_hash[65];
} Block;

/* ── Ledger (dynamic array) ─────────────────────────────────────────────────*/
static Block *ledger            = NULL;
static int    block_count       = 0;
static int    max_blocks        = MAX_BLOCKS_DEFAULT;
static int    difficulty        = 4;
static char   global_prev_hash[65] = "0000000000000000";

/* ── Control flags ──────────────────────────────────────────────────────────*/
static int simulation_running   = 0;   /* miners should mine                */
static int terminate            = 0;   /* all threads must exit             */
static int block_solved         = 0;   /* current round already claimed     */

/* ── Synchronisation ────────────────────────────────────────────────────────*/
static pthread_mutex_t ctrl_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ctrl_cond     = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t ncurses_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Thread-safe log ring buffer ────────────────────────────────────────────*/
static char            log_ring[LOG_RING_SIZE][LOG_MSG_LEN];
static int             log_head = 0, log_tail = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_cond  = PTHREAD_COND_INITIALIZER;

/* ── ncurses windows ────────────────────────────────────────────────────────*/
static WINDOW *status_win  = NULL;
static WINDOW *control_win = NULL;

/* ============================================================
 *  LOG RING — thread-safe message queue
 * ============================================================ */
static void log_push(const char *msg)
{
    pthread_mutex_lock(&log_mutex);
    int next = (log_head + 1) % LOG_RING_SIZE;
    if (next != log_tail) {
        strncpy(log_ring[log_head], msg, LOG_MSG_LEN - 1);
        log_ring[log_head][LOG_MSG_LEN - 1] = '\0';
        log_head = next;
        pthread_cond_signal(&log_cond);
    }
    pthread_mutex_unlock(&log_mutex);
}

static void log_printf(const char *fmt, ...)
{
    char buf[LOG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_push(buf);
}

/* ============================================================
 *  HASH & DIFFICULTY
 * ============================================================ */
static unsigned long compute_hash(const char *data,
                                  unsigned long nonce,
                                  const char *prev)
{
    unsigned long h = 5381UL;
    char input[640];
    snprintf(input, sizeof(input), "%s|%s|%lu", prev, data, nonce);
    for (int i = 0; input[i]; i++)
        h = h * 31UL + (unsigned char)input[i];
    return h;
}

/* Count that lowest (difficulty * 4) bits are all zero — no shift overflow */
static int meets_difficulty(unsigned long hash)
{
    if (difficulty <= 0) return 1;
    int nibbles = difficulty > 16 ? 16 : difficulty;
    unsigned long h = hash;
    for (int i = 0; i < nibbles; i++) {
        if (h & 0xFUL) return 0;
        h >>= 4;
    }
    return 1;
}

/* ============================================================
 *  DISPLAY THREAD — drains log ring into status_win
 * ============================================================ */
static void *display_thread(void *arg)
{
    (void)arg;

    while (1) {
        /* Wait for a message or terminate signal */
        pthread_mutex_lock(&log_mutex);
        while (log_head == log_tail) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 60000000L;  /* 60 ms timeout */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec  += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&log_cond, &log_mutex, &ts);

            /* Check terminate while ring is empty */
            pthread_mutex_unlock(&log_mutex);
            pthread_mutex_lock(&ctrl_mutex);
            int done = terminate;
            pthread_mutex_unlock(&ctrl_mutex);
            if (done) return NULL;
            pthread_mutex_lock(&log_mutex);
        }

        char msg[LOG_MSG_LEN];
        strncpy(msg, log_ring[log_tail], LOG_MSG_LEN - 1);
        msg[LOG_MSG_LEN - 1] = '\0';
        log_tail = (log_tail + 1) % LOG_RING_SIZE;
        pthread_mutex_unlock(&log_mutex);

        /* Write to ncurses window */
        pthread_mutex_lock(&ncurses_mutex);
        wprintw(status_win, "%s\n", msg);
        wrefresh(status_win);
        pthread_mutex_unlock(&ncurses_mutex);
    }
    return NULL;
}

/* ============================================================
 *  MINER THREAD
 * ============================================================ */
static void *miner(void *arg)
{
    int id = *(int *)arg;

    while (1) {
        /* ── Phase 1: wait until we should be working ── */
        pthread_mutex_lock(&ctrl_mutex);
        while (!terminate && (!simulation_running || block_count >= max_blocks))
            pthread_cond_wait(&ctrl_cond, &ctrl_mutex);

        if (terminate) { pthread_mutex_unlock(&ctrl_mutex); break; }

        /* ── Phase 2: wait for current round slot to open ── */
        while (!terminate && block_solved)
            pthread_cond_wait(&ctrl_cond, &ctrl_mutex);

        if (terminate || !simulation_running || block_count >= max_blocks) {
            pthread_mutex_unlock(&ctrl_mutex);
            continue;
        }

        /* ── Capture stable round parameters ── */
        int   round = block_count;
        char  prev[65];
        memcpy(prev, global_prev_hash, sizeof(prev));
        pthread_mutex_unlock(&ctrl_mutex);

        /* ── Build block payload ── */
        char data[256];
        snprintf(data, sizeof(data), "Miner-%d Block-%d ts=%ld",
                 id, round, (long)time(NULL));

        log_printf("[Miner %d] PoW started — block %d  difficulty=%d",
                   id, round, difficulty);

        /* ── Proof-of-Work ── */
        unsigned long nonce = 0;
        int won = 0;

        while (1) {
            unsigned long h = compute_hash(data, nonce, prev);

            if (meets_difficulty(h)) {
                /* Try to claim the block */
                pthread_mutex_lock(&ctrl_mutex);
                if (!block_solved && block_count == round &&
                        simulation_running && !terminate) {
                    block_solved = 1;
                    won = 1;

                    /* Write block into ledger */
                    ledger[block_count].block_id  = block_count;
                    ledger[block_count].miner_id  = id;
                    ledger[block_count].timestamp = time(NULL);
                    ledger[block_count].nonce     = nonce;
                    /* Safe copies: dest size - 1 + explicit null */
                    memcpy(ledger[block_count].data, data,
                           sizeof(ledger[block_count].data) - 1);
                    ledger[block_count].data[sizeof(ledger[block_count].data)-1] = '\0';
                    memcpy(ledger[block_count].prev_hash, prev,
                           sizeof(ledger[block_count].prev_hash) - 1);
                    ledger[block_count].prev_hash[64] = '\0';
                    snprintf(ledger[block_count].this_hash,
                             sizeof(ledger[block_count].this_hash),
                             "%016lx", h);

                    /* Advance global chain */
                    memcpy(global_prev_hash, ledger[block_count].this_hash,
                           sizeof(global_prev_hash) - 1);
                    global_prev_hash[64] = '\0';

                    block_count++;
                    block_solved = 0;   /* open next round */
                    pthread_cond_broadcast(&ctrl_cond);
                }
                pthread_mutex_unlock(&ctrl_mutex);
                break;
            }

            nonce++;

            /* Check for abort every 5000 iterations */
            if (nonce % 5000 == 0) {
                pthread_mutex_lock(&ctrl_mutex);
                int abort = terminate || !simulation_running || block_count != round;
                pthread_mutex_unlock(&ctrl_mutex);
                if (abort) break;
            }
        }

        if (won) {
            log_printf("[Miner %d] *** BLOCK %d ACCEPTED ***  nonce=%-10lu  hash=%s",
                       id, round, nonce, ledger[round].this_hash);
        }
    }
    return NULL;
}

/* ============================================================
 *  LEDGER VIEW
 * ============================================================ */
static void show_ledger(void)
{
    /* Snapshot under lock */
    pthread_mutex_lock(&ctrl_mutex);
    int   cnt  = block_count;
    Block *snap = malloc((cnt > 0 ? cnt : 1) * sizeof(Block));
    if (snap && cnt > 0) memcpy(snap, ledger, cnt * sizeof(Block));
    pthread_mutex_unlock(&ctrl_mutex);

    pthread_mutex_lock(&ncurses_mutex);
    wclear(status_win);
    scrollok(status_win, TRUE);
    wsetscrreg(status_win, 0, getmaxy(status_win) - 1);

    wprintw(status_win,
            "══════════════════ BLOCKCHAIN LEDGER (%d blocks) ══════════════════\n",
            cnt);

    if (!snap || cnt == 0) {
        wprintw(status_win, "  (no blocks mined yet)\n");
    } else {
        for (int i = 0; i < cnt; i++) {
            char ts[32];
            struct tm tminfo;
            localtime_r(&snap[i].timestamp, &tminfo);
            strftime(ts, sizeof(ts), "%H:%M:%S", &tminfo);
            wprintw(status_win,
                    " #%02d | Miner:%d | %s | nonce:%-12lu\n"
                    "      hash: %s\n"
                    "      prev: %s\n"
                    "      data: %s\n",
                    snap[i].block_id, snap[i].miner_id, ts,
                    snap[i].nonce,
                    snap[i].this_hash,
                    snap[i].prev_hash,
                    snap[i].data);
        }
    }
    wprintw(status_win, "═══════════════════════════════════════════════════════\n");
    wrefresh(status_win);
    pthread_mutex_unlock(&ncurses_mutex);
    free(snap);
}

/* ============================================================
 *  MENU DRAW
 * ============================================================ */
static void draw_menu(int highlight)
{
    static const char *options[] = {
        "1. Start simulation",
        "2. Stop  simulation",
        "3. Set max blocks  ",
        "4. Set difficulty  ",
        "5. View ledger     ",
        "6. Exit            "
    };
    const int N = 6;

    pthread_mutex_lock(&ctrl_mutex);
    int bc  = block_count;
    int mb  = max_blocks;
    int dif = difficulty;
    int run = simulation_running;
    pthread_mutex_unlock(&ctrl_mutex);

    pthread_mutex_lock(&ncurses_mutex);
    wclear(control_win);
    box(control_win, 0, 0);
    wattron(control_win, A_BOLD);
    mvwprintw(control_win, 1, 2,
              "  Blockchain Mining Simulator  |  POSIX Threads + PoW");
    wattroff(control_win, A_BOLD);
    mvwprintw(control_win, 2, 2,
              "  Blocks: %d/%d  |  Difficulty: %d nibble(s)  |  [ %s ]",
              bc, mb, dif, run ? "RUNNING" : "STOPPED");

    for (int i = 0; i < N; i++) {
        if (i == highlight) {
            wattron(control_win, A_REVERSE | A_BOLD);
            mvwprintw(control_win, i + 4, 4, " > %s < ", options[i]);
            wattroff(control_win, A_REVERSE | A_BOLD);
        } else {
            mvwprintw(control_win, i + 4, 4, "   %s   ", options[i]);
        }
    }
    mvwprintw(control_win, N + 5, 2,
              "  UP/DOWN to navigate   ENTER to select");
    wrefresh(control_win);
    pthread_mutex_unlock(&ncurses_mutex);
}

/* Prompt integer in control_win — must not hold ncurses_mutex on entry */
static int prompt_int(const char *prompt, int lo, int hi)
{
    char buf[16] = {0};

    pthread_mutex_lock(&ncurses_mutex);
    echo();
    curs_set(1);
    wclear(control_win);
    box(control_win, 0, 0);
    mvwprintw(control_win, 2, 2, "  %s (%d-%d): ", prompt, lo, hi);
    wrefresh(control_win);
    wgetnstr(control_win, buf, (int)sizeof(buf) - 1);
    noecho();
    curs_set(0);
    pthread_mutex_unlock(&ncurses_mutex);

    int val = atoi(buf);
    return (val >= lo && val <= hi) ? val : -1;
}

/* ============================================================
 *  UI THREAD
 * ============================================================ */
static void *ui_thread(void *arg)
{
    (void)arg;
    int highlight = 0;
    const int N   = 6;

    while (1) {
        draw_menu(highlight);

        pthread_mutex_lock(&ncurses_mutex);
        int ch = getch();
        pthread_mutex_unlock(&ncurses_mutex);

        switch (ch) {
        case KEY_UP:
            highlight = (highlight == 0) ? N - 1 : highlight - 1;
            break;
        case KEY_DOWN:
            highlight = (highlight == N - 1) ? 0 : highlight + 1;
            break;
        case '\n': case KEY_ENTER: {
            int sel = highlight;

            /* ─ 0: Start ─ */
            if (sel == 0) {
                pthread_mutex_lock(&ctrl_mutex);
                if (!simulation_running) {
                    if (block_count >= max_blocks) {
                        pthread_mutex_unlock(&ctrl_mutex);
                        log_push("[UI] Max blocks reached. Increase max blocks first.");
                        break;
                    }
                    simulation_running = 1;
                    block_solved       = 0;
                    pthread_cond_broadcast(&ctrl_cond);
                    log_push("[UI] Simulation STARTED.");
                } else {
                    log_push("[UI] Already running.");
                }
                pthread_mutex_unlock(&ctrl_mutex);
            }

            /* ─ 1: Stop ─ */
            else if (sel == 1) {
                pthread_mutex_lock(&ctrl_mutex);
                if (simulation_running) {
                    simulation_running = 0;
                    pthread_cond_broadcast(&ctrl_cond);
                    log_push("[UI] Simulation STOPPED.");
                } else {
                    log_push("[UI] Not running.");
                }
                pthread_mutex_unlock(&ctrl_mutex);
            }

            /* ─ 2: Set max blocks ─ */
            else if (sel == 2) {
                int v = prompt_int("Enter max blocks", 1, MAX_BLOCKS_CAP);
                if (v > 0) {
                    pthread_mutex_lock(&ctrl_mutex);
                    Block *tmp = realloc(ledger, (size_t)v * sizeof(Block));
                    if (!tmp) {
                        log_push("[ERR] realloc failed. Max blocks unchanged.");
                    } else {
                        ledger = tmp;
                        max_blocks = v;
                        if (block_count > max_blocks) block_count = max_blocks;
                        log_printf("[UI] Max blocks → %d", max_blocks);
                    }
                    pthread_mutex_unlock(&ctrl_mutex);
                } else {
                    log_push("[UI] Invalid input (1-100 required).");
                }
            }

            /* ─ 3: Set difficulty ─ */
            else if (sel == 3) {
                int v = prompt_int("Enter difficulty", 1, 8);
                if (v > 0) {
                    pthread_mutex_lock(&ctrl_mutex);
                    difficulty = v;
                    pthread_mutex_unlock(&ctrl_mutex);
                    log_printf("[UI] Difficulty → %d nibble(s) = %d leading zero bits",
                               v, v * 4);
                } else {
                    log_push("[UI] Invalid input (1-8 required).");
                }
            }

            /* ─ 4: View ledger ─ */
            else if (sel == 4) {
                show_ledger();
            }

            /* ─ 5: Exit ─ */
            else if (sel == 5) {
                pthread_mutex_lock(&ctrl_mutex);
                simulation_running = 0;
                terminate          = 1;
                pthread_cond_broadcast(&ctrl_cond);
                pthread_mutex_unlock(&ctrl_mutex);
                pthread_mutex_lock(&log_mutex);
                pthread_cond_broadcast(&log_cond);
                pthread_mutex_unlock(&log_mutex);
                return NULL;
            }
            break;
        }
        default:
            break;
        }
    }
    return NULL;
}

/* ============================================================
 *  MAIN
 * ============================================================ */
int main(void)
{
    /* ncurses setup */
    initscr();
    start_color();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);

    int max_y = 24, max_x = 80;
    getmaxyx(stdscr, max_y, max_x);
    if (max_y < 20) max_y = 24;
    if (max_x < 60) max_x = 80;

    int ctrl_rows = 13;
    status_win  = newwin(max_y - ctrl_rows, max_x, 0, 0);
    control_win = newwin(ctrl_rows, max_x, max_y - ctrl_rows, 0);
    scrollok(status_win, TRUE);
    wsetscrreg(status_win, 0, max_y - ctrl_rows - 1);

    /* Allocate ledger */
    ledger = calloc((size_t)max_blocks, sizeof(Block));
    if (!ledger) {
        endwin();
        fprintf(stderr, "calloc failed\n");
        return 1;
    }

    /* Init condition variables (mutexes already static-initialised) */
    pthread_cond_init(&ctrl_cond, NULL);
    pthread_cond_init(&log_cond,  NULL);

    /* Spawn display thread */
    pthread_t disp_tid;
    pthread_create(&disp_tid, NULL, display_thread, NULL);

    /* Spawn miner threads */
    pthread_t miner_tids[NUM_MINERS];
    int       miner_ids[NUM_MINERS];
    for (int i = 0; i < NUM_MINERS; i++) {
        miner_ids[i] = i;
        pthread_create(&miner_tids[i], NULL, miner, &miner_ids[i]);
    }

    /* Spawn UI thread */
    pthread_t ui_tid;
    pthread_create(&ui_tid, NULL, ui_thread, NULL);

    /* Welcome */
    log_push("[SIM] Blockchain Mining Simulator ready.");
    log_push("[SIM] Select 'Start simulation' from the menu to begin.");
    log_push("[SIM] Miners: 3 POSIX threads | Sync: mutex + condition variable");

    /* Wait for UI to exit (user selected Exit) */
    pthread_join(ui_tid, NULL);

    /* Signal all threads to terminate */
    pthread_mutex_lock(&ctrl_mutex);
    terminate = 1;
    simulation_running = 0;
    pthread_cond_broadcast(&ctrl_cond);
    pthread_mutex_unlock(&ctrl_mutex);
    pthread_mutex_lock(&log_mutex);
    pthread_cond_broadcast(&log_cond);
    pthread_mutex_unlock(&log_mutex);

    /* Join all threads */
    for (int i = 0; i < NUM_MINERS; i++)
        pthread_join(miner_tids[i], NULL);
    pthread_join(disp_tid, NULL);

    /* Cleanup */
    int final_count = block_count;
    free(ledger);
    pthread_cond_destroy(&ctrl_cond);
    pthread_cond_destroy(&log_cond);
    delwin(status_win);
    delwin(control_win);
    endwin();

    printf("\nSimulation complete. Blocks mined: %d\n", final_count);
    return 0;
}

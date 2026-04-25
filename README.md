# ⛏️ Blockchain-Inspired Mining Simulator

A multi-threaded blockchain mining simulation built in C using **POSIX threads**, **semaphores**, **condition variables**, and **ncurses** for a live terminal UI. Developed as an Operating Systems project to demonstrate concurrent programming, synchronisation primitives, and proof-of-work consensus.

---

## 📸 Demo

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ [SIM] Blockchain Mining Simulator ready.                                    │
│ [SIM] Select 'Start simulation' from the menu to begin.                    │
│ [Miner 0] PoW started — block 0  difficulty=4                              │
│ [Miner 1] PoW started — block 0  difficulty=4                              │
│ [Miner 2] PoW started — block 0  difficulty=4                              │
│ [Miner 1] *** BLOCK 0 ACCEPTED ***  nonce=102534   hash=6553b2decece0000   │
│ [Miner 0] *** BLOCK 1 ACCEPTED ***  nonce=59494    hash=f6767942501b0000   │
├─────────────────────────────────────────────────────────────────────────────┤
│   Blockchain Mining Simulator  |  POSIX Threads + PoW                      │
│   Blocks: 2/20  |  Difficulty: 4 nibble(s)  |  [ RUNNING ]                 │
│                                                                             │
│    > 1. Start simulation <                                                  │
│      2. Stop  simulation                                                    │
│      3. Set max blocks                                                      │
│      4. Set difficulty                                                      │
│      5. View ledger                                                         │
│      6. Exit                                                                │
│                                                                             │
│   UP/DOWN to navigate   ENTER to select                                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🚀 Features

- **3 concurrent POSIX miner threads** racing to solve proof-of-work puzzles
- **Adjustable difficulty** (1–8 nibbles) and **max block count** (1–100) at runtime
- **Proper blockchain** — each block stores the hash of the previous block, creating a tamper-evident chain
- **Thread-safe ring buffer** for zero-deadlock log message passing between threads
- **Interactive ncurses TUI** — split-screen live log pane + arrow-key menu
- **Clean shutdown** — all threads joined, memory freed, terminal restored on exit

---

## 🛠️ Tech Stack

| Technology | Purpose |
|---|---|
| `C (C99)` | Systems language — direct memory and thread control |
| `POSIX Threads (pthreads)` | `pthread_create`, `pthread_join`, `pthread_mutex_*`, `pthread_cond_*` |
| `Condition Variables` | Sleeping/waking miner threads on state changes |
| `ncurses` | Interactive split-screen terminal UI |
| `djb2 Hash` | Polynomial rolling hash for proof-of-work |
| `Ring Buffer` | Thread-safe lock-free-style message queue |

---

## 📁 Project Structure

```
mining-simulator/
├── mining_sim.c      ← Full program source (620 lines)
└── README.md         ← This file
```

**Threads spawned at runtime:**

```
main()              — setup, memory allocation, thread orchestration
├── miner()  × 3   — compete to solve PoW, write blocks to ledger
├── ui_thread()    — handles keyboard input, draws ncurses menu
└── display_thread() — drains log ring buffer into status window
```

---

## ⚙️ Build & Run

### Prerequisites

```bash
# Ubuntu / Debian / WSL2
sudo apt update
sudo apt install -y gcc libncurses-dev
```

### Compile

```bash
gcc -Wall -Wextra -O2 -o mining_sim mining_sim.c -lpthread -lncurses
```

### Run

```bash
./mining_sim
```

> **Terminal requirement:** At least 80×24 characters. Use a standalone terminal (gnome-terminal, Windows Terminal, iTerm2) — avoid running inside VS Code's embedded terminal for best ncurses rendering.

---

## 🎮 Usage

| Key | Action |
|---|---|
| `↑` / `↓` | Navigate menu |
| `Enter` | Select option |

### Menu Options

| Option | Description |
|---|---|
| Start simulation | Wakes all miner threads via `pthread_cond_broadcast` |
| Stop simulation | Sets `simulation_running=0`, miners sleep in `pthread_cond_wait` |
| Set max blocks | Adjust how many blocks to mine (1–100) |
| Set difficulty | Adjust PoW difficulty (1–8 nibbles). Higher = exponentially slower |
| View ledger | Shows full blockchain with hash chain verification |
| Exit | Broadcasts terminate flag, joins all threads, restores terminal |

---

## 🔗 How the Blockchain Works

Each block contains:

```
block_id  | miner_id | timestamp | nonce | prev_hash | this_hash | data
```

`this_hash` is computed as:

```
djb2( prev_hash + "|" + data + "|" + nonce )
```

The result must have `difficulty` leading zero nibbles. For example at difficulty=4:

```
Block #00  prev: 0000000000000000  hash: 6553b2decece0000  ← ends in 0000 ✅
Block #01  prev: 6553b2decece0000  hash: f6767942501b0000  ← ends in 0000 ✅
Block #02  prev: f6767942501b0000  hash: 4061f8c772d00000  ← ends in 0000 ✅
```

Changing any past block changes its hash, breaking the chain link of every subsequent block — tamper-evident by design.

---

## 📊 Difficulty Reference

| Difficulty | Leading zero bits | Avg nonces needed | Approx time (3 miners) |
|---|---|---|---|
| 1 | 4 bits | ~16 | < 1ms |
| 2 | 8 bits | ~256 | < 1ms |
| 3 | 12 bits | ~4,096 | ~2–5ms |
| **4 (default)** | **16 bits** | **~65,536** | **~30–100ms** |
| 5 | 20 bits | ~1,048,576 | ~0.5–2s |
| 6 | 24 bits | ~16,777,216 | ~8–20s |
| 7 | 28 bits | ~268,435,456 | ~2–5 min |
| 8 | 32 bits | ~4,294,967,296 | very slow |

---

## 🐛 Bugs Fixed (from original)

Eight concurrency bugs were identified and fixed in this implementation:

| # | Bug | Fix |
|---|---|---|
| 1 | Variable re-declared across `switch` cases (UB) | All locals declared before `switch` |
| 2 | `ncurses_mutex` locked while holding `block_mutex` (deadlock) | Ring buffer decouples log writing from display |
| 3 | `sem_wait` called while mutex held (priority inversion) | Semaphore removed — mutex already protects ledger |
| 4 | Each miner had its own local `prev_hash` (broken chain) | Single global `prev_hash` under `ctrl_mutex` |
| 5 | `1UL << (difficulty*4)` overflows at difficulty≥16 (UB) | Iterative nibble check, shifts by 4 at a time |
| 6 | Miners stuck in `pthread_cond_wait` after UI exits (zombie threads) | Global `terminate` flag + broadcast + join sequence |
| 7 | Shared globals read without lock inside mining loop (data race) | Periodic mutex-protected abort check every 5000 iterations |
| 8 | `getch()` called without `ncurses_mutex` (ncurses not thread-safe) | All ncurses calls wrapped with `ncurses_mutex` |

---

## 🔬 Key OS Concepts Demonstrated

- **Race condition prevention** — `ctrl_mutex` protects `block_count`, `ledger[]`, `global_prev_hash`
- **Condition variable pattern** — miners sleep with `pthread_cond_wait` (zero CPU when idle), wake on `pthread_cond_broadcast`
- **Deadlock prevention** — strict lock ordering; ring buffer eliminates nested mutex acquisition
- **Spurious wakeup handling** — all `pthread_cond_wait` calls inside `while` loops, never `if`
- **Thread-safe producer-consumer** — ring buffer with separate `log_mutex` decoupled from blockchain state
- **Snapshot pattern** — ledger copied under lock before UI rendering to minimise lock hold time
- **Clean shutdown** — `terminate` flag + broadcast + `pthread_join` for every thread

---

## 📝 Sample Ledger Output

```
══════════════════ BLOCKCHAIN LEDGER (5 blocks) ══════════════════
 #00 | Miner:1 | 18:53:50 | nonce:102534
      hash: 6553b2decece0000
      prev: 0000000000000000
      data: Miner-1 Block-0 ts=1777123430

 #01 | Miner:0 | 18:53:50 | nonce:59494
      hash: f6767942501b0000
      prev: 6553b2decece0000
      data: Miner-0 Block-1 ts=1777123430

 #02 | Miner:0 | 18:53:50 | nonce:23909
      hash: 4061f8c772d00000
      prev: f6767942501b0000
      data: Miner-0 Block-2 ts=1777123430
═════════════════════════════════════════════════════════════════
```

---

## 🖥️ Platform Support

| Platform | Supported |
|---|---|
| Ubuntu / Debian | ✅ |
| WSL2 (Windows) | ✅ |
| macOS | ✅ (needs `brew install ncurses`) |
| Fedora / RHEL | ✅ (use `dnf install ncurses-devel`) |
| Native Windows | ❌ (no POSIX threads) |

---

## 👤 Author

**Jovisha** — Operating Systems Project  
Built with C · POSIX · ncurses

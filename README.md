# Simple Cache Controller FSM Simulation

This project implements a **simple cache controller simulator in C++** using a **finite-state machine (FSM)**. The controller follows the simple cache organization with the four states:

* **Idle**
* **Compare Tag**
* **Write-Back**
* **Allocate**

The purpose of the simulator is to show, cycle by cycle, how the internal state of the cache controller changes for a sequence of `lw` and `sw` requests, and how those state changes affect the interface signals between the **CPU**, **cache**, and **memory**.

---

## Cache organization used in the simulator

The simulator uses the following cache characteristics:

* **Direct-mapped cache**
* **Write-back using write allocate**
* **Block size = 4 words = 16 bytes = 128 bits**
* **Cache size = 16 KiB**
* **Number of cache lines = 1024**
* **32-bit addresses**
* **One valid bit and one dirty bit per cache line**

From these values, the address fields are:

* **Tag = 18 bits**
* **Index = 10 bits**
* **Block offset = 4 bits**

---

## Interface signals modeled

### CPU ↔ Cache interface

* 1-bit **Read/Write** signal
* 1-bit **Valid** signal
* 32-bit **address**
* 32-bit **data from CPU to cache**
* 32-bit **data from cache to CPU**
* 1-bit **Ready** signal

### Cache ↔ Memory interface

* 1-bit **Read/Write** signal
* 1-bit **Valid** signal
* 32-bit **address**
* 128-bit **data from cache to memory**
* 128-bit **data from memory to cache**
* 1-bit **Ready** signal

The memory in this project never misses, but it takes a fixed number of cycles to complete a block read or block write.

---

## Project files

* `cache_main.cpp` — main C++ source file containing the cache, memory, parser, and FSM logic
* `instructions.txt` — input file containing the assembly-like request stream
* `output.txt` — generated cycle-by-cycle simulation trace
* `COA_A2_230041114.pdf` — report document
* `README.md` — build, run, and project description

---

## Input format

The simulator reads requests from the text file instructions.txt . Each line contains one memory request in a simplified assembly-like format:

```text
lw x1, 0x00000000
lw x2, 0x00000004
sw x3, 0x00000008
lw x4, 0x00004000
lw x5, 0x00000008
sw x6, 0x00008000
lw x7, 0x00004000
```

### Supported instructions

* `lw <register>, <address>`
* `sw <register>, <address>`

Example:

```text
lw x1, 0x00000000
sw x3, 0x00000008
```

Lines beginning with `#` can be used as comments.

---

## FSM behavior implemented

The controller uses the following four states.

### 1. Idle

This state waits for a valid read or write request from the processor.

### 2. Compare Tag

This state checks whether the requested access is a **hit** or a **miss**.

* If the line is valid and the tag matches, it is a **hit**.
* For a **load**, the selected word is returned and `Ready` is asserted.
* For a **store**, the selected word is updated, the dirty bit is set, and `Ready` is asserted.
* On a **miss**, the controller goes either to **Write-Back** or **Allocate**.

### 3. Write-Back

If the old cache block is dirty, the controller writes the full 128-bit block back to memory and waits until memory asserts `Ready`.

### 4. Allocate

The controller fetches the new block from memory and waits until memory asserts `Ready`. Then the block is placed into the cache and the controller returns to **Compare Tag** to complete the original request.

---

## What the simulation demonstrates

The default request stream was chosen so that the simulation covers all important cases:

* **Read miss on an invalid line**
* **Read hit on an allocated block**
* **Write hit that sets the dirty bit**
* **Conflict miss causing write-back**
* **Store miss with write allocate**
* **Later re-access to verify correctness of write-back**

The generated trace in `output.txt` shows:

* current cycle number
* current FSM state
* CPU-side signals
* memory-side signals
* reason for the current transition
* final cache-line status after each request

---

## How to build

Compile the program using `g++` with C++17 support:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_main.cpp -o cache_fsm
```

---

## How to run

Run the simulator with the input file and desired output file:

```bash
./cache_fsm instructions.txt output.txt
```

After execution, the terminal will print a completion message and the detailed trace will be written to `output.txt`.

---

## Example output behavior

The trace shows the cache-controller operation cycle by cycle. For example, the simulator demonstrates behaviors such as:

* cache miss and old block is clean, so the FSM goes to Allocate
* cache miss and old block is dirty, so the FSM goes to Write-Back
* memory read is complete; FSM returns to Compare Tag to finish the request
* cache hit; data are read from the selected word and the Ready signal is sent
* cache hit; data are written to the selected word, dirty bit is set, and the Ready signal is sent

This makes the program useful both as a working simulator and as a learning aid for understanding the FSM-based cache controller.

---

## Notes

* The memory model uses a deterministic default value pattern for words that were not previously written.
* The cache is **blocking**, meaning the CPU waits until the current request is completed.
* The design is intentionally simple so that the FSM behavior remains clear in the output.

---

## Author

**Name: Sadia Afrin Aurthy**
**Section: 01**
**SID: 230041114**


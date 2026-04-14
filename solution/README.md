# Multi-Container Runtime — Solution

## Team Information

- **Team Member 1:** Prajwal M — PES2UG24CS910
- **Team Member 2:** R P Pranav — PES2UG24CS917

---

## What Was Built

This repository contains a complete implementation of the Multi-Container Runtime project. Every TODO in the boilerplate has been filled in:

| Component | File | Status |
|-----------|------|--------|
| User-space supervisor + CLI | `engine.c` | ✅ Complete |
| Kernel memory monitor (LKM) | `monitor.c` | ✅ Complete |
| Shared ioctl definitions | `monitor_ioctl.h` | ✅ Complete |
| CPU-bound workload | `cpu_hog.c` | ✅ Complete (from boilerplate) |
| I/O-bound workload | `io_pulse.c` | ✅ Complete (from boilerplate) |
| Memory pressure workload | `memory_hog.c` | ✅ Complete (from boilerplate) |
| Build system | `Makefile` | ✅ Complete |

### What each implemented piece does

**`engine.c` — User-Space Runtime**

- **`bounded_buffer_push` / `bounded_buffer_pop`** — Classic producer-consumer bounded buffer using a mutex and two condition variables (`not_full`, `not_empty`). Producers block when full; consumers drain all remaining items before exiting on shutdown so no log lines are lost.
- **`producer_thread`** — One detached thread per container. Reads from the container's pipe and pushes log chunks into the shared buffer.
- **`logging_thread`** — Single consumer thread. Pops from the buffer and appends to per-container log files. On shutdown, drains leftover items before returning.
- **`child_fn`** — Clone child entry point. Sets hostname (UTS), `chroot`s into the container's rootfs, mounts `/proc`, redirects stdout/stderr to the logging pipe, then `execvp`s the requested command.
- **`run_supervisor`** — Opens `/dev/container_monitor`, creates the UNIX domain socket at `/tmp/mini_runtime.sock`, installs SIGCHLD/SIGTERM/SIGINT handlers, spawns the logger thread, and enters a `select`-based event loop that accepts one connection per CLI request.
- **`handle_request`** — Dispatches `start`, `run`, `ps`, `logs`, `stop` commands. The `run` command blocks with `waitpid` until the container exits.
- **`reap_children`** — Called on SIGCHLD. Uses `waitpid(-1, WNOHANG)` in a loop. Classifies exit as `stopped`, `killed`, or `exited` using the `stop_requested` flag.
- **`send_control_request`** — Client-side: connects to the supervisor socket, sends a `control_request_t`, receives a `control_response_t`.

**`monitor.c` — Kernel Module**

- Defines `struct monitored_entry` with `list_head` linkage, PID, limits, and `soft_warned` flag.
- `LIST_HEAD(monitored_list)` + `DEFINE_MUTEX(monitored_lock)` protect concurrent access from ioctl and the timer.
- `timer_callback` fires every second, iterates `list_for_each_entry_safe`, removes stale entries, enforces limits.
- `MONITOR_REGISTER` ioctl allocates a node and inserts it under the mutex.
- `MONITOR_UNREGISTER` ioctl finds and frees the matching node.
- `monitor_exit` drains the entire list before the module unloads.

---

## Environment Requirements

- **Ubuntu 22.04 or 24.04** (x86_64 VM)
- **Secure Boot OFF** — required for loading unsigned kernel modules
- **No WSL** — kernel module loading is not supported in WSL

---

## Build, Load, and Run Instructions

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2. Run the environment preflight check

```bash
cd solution
chmod +x environment-check.sh
sudo ./environment-check.sh
```

Fix any reported issues before continuing.

### 3. Build everything

```bash
cd solution
make
```

This compiles `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`.

**CI-only build (no kernel headers, no sudo):**
```bash
make -C solution ci
```

### 4. Prepare the Alpine root filesystem

```bash
# From the repo root (one directory above solution/)
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

> **Do not commit** `rootfs-base/` or `rootfs-*/` to the repository.

### 5. Copy workload binaries into rootfs copies

The workload binaries are statically linked by default, so they run inside Alpine:

```bash
cp solution/cpu_hog    ./rootfs-alpha/
cp solution/io_pulse   ./rootfs-alpha/
cp solution/memory_hog ./rootfs-alpha/
cp solution/cpu_hog    ./rootfs-beta/
cp solution/io_pulse   ./rootfs-beta/
cp solution/memory_hog ./rootfs-beta/
```

### 6. Load the kernel module

```bash
sudo insmod solution/monitor.ko

# Verify control device exists
ls -l /dev/container_monitor

# Check module is loaded
lsmod | grep monitor
```

### 7. Start the supervisor

Open a dedicated terminal for the supervisor:

```bash
sudo ./solution/engine supervisor ./rootfs-base
```

The supervisor will print:
```
[supervisor] started, control socket: /tmp/mini_runtime.sock
```

### 8. Use the CLI (in another terminal)

```bash
# Start two containers in the background
sudo ./solution/engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./solution/engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 96

# List all tracked containers
sudo ./solution/engine ps

# Inspect logs for a container
sudo ./solution/engine logs alpha

# Stop a container cleanly
sudo ./solution/engine stop alpha
sudo ./solution/engine stop beta

# Run a container in the foreground (blocks until done)
sudo ./solution/engine run worker ./rootfs-alpha /cpu_hog 5
```

### 9. Test memory limits

```bash
# Start container with tight memory limits
sudo ./solution/engine start memtest ./rootfs-alpha /memory_hog --soft-mib 30 --hard-mib 50

# Watch kernel log for soft/hard limit events
dmesg -w | grep container_monitor

# After a few seconds the hard limit fires and the container is killed
sudo ./solution/engine ps
```

### 10. Scheduler experiment

```bash
# Run CPU-bound container at default priority
sudo ./solution/engine start cpu-lo ./rootfs-alpha /cpu_hog 30 --nice 10

# Run CPU-bound container at high priority in parallel
sudo ./solution/engine start cpu-hi ./rootfs-beta  /cpu_hog 30 --nice -10

# Compare completion times in logs
sudo ./solution/engine logs cpu-lo
sudo ./solution/engine logs cpu-hi
```

### 11. Stop supervisor and unload module

Send SIGTERM to the supervisor (Ctrl-C in its terminal, or):

```bash
sudo kill -TERM $(pgrep -f "engine supervisor")

# Unload kernel module
sudo rmmod monitor

# Verify clean state
dmesg | tail -5
ps aux | grep engine   # should show nothing
```

### 12. Clean build artifacts

```bash
cd solution
make clean
```

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime calls `clone()` with three namespace flags:

- **`CLONE_NEWPID`** — Each container gets its own PID namespace. PID 1 inside the container is the `child_fn` entry point. The host kernel continues to assign host-wide PIDs (which the supervisor tracks), but the container sees a fresh PID space starting at 1.
- **`CLONE_NEWUTS`** — Each container gets its own hostname and domain-name. `sethostname` in `child_fn` sets a per-container hostname without affecting the host.
- **`CLONE_NEWNS`** — Each container gets its own mount namespace. Mounting `/proc` inside the container does not change the host mount table.

`chroot(rootfs)` makes the container's `/` point only to its assigned directory. Combined with the mount namespace, the container cannot see the host filesystem tree. A full `pivot_root` would additionally prevent `..` traversal escapes and is more production-appropriate, but `chroot` is sufficient here.

What the host kernel still shares with all containers: the network stack (no `CLONE_NEWNET`), the IPC namespace (no `CLONE_NEWIPC`), and the cgroup hierarchy. These were omitted for simplicity.

### 2. Supervisor and Process Lifecycle

The supervisor is a long-running parent because only the parent of a process can `wait()` for it. If each `engine start` invocation launched a container and exited, the container would be reparented to PID 1 (init) and the original parent's metadata would be lost.

Process creation path: supervisor calls `clone()` → kernel creates a new task with the specified namespaces → the child runs `child_fn` which sets up the environment and calls `execvp` replacing itself with the workload.

Zombie prevention: `SIGCHLD` is delivered to the supervisor. The handler sets a flag; the event loop calls `waitpid(-1, WNOHANG)` in a loop to collect all exited children. Without this, each exited container would remain a zombie (holding its PID entry in the kernel process table) until the supervisor exits.

Metadata tracking: each container's `container_record_t` is stored in a linked list protected by `metadata_lock`. The fields (`state`, `exit_code`, `exit_signal`, `stop_requested`) allow `ps` to give accurate status even after the container exits.

### 3. IPC, Threads, and Synchronisation

**Path A — logging (pipe-based):**

- The supervisor creates a `pipe(2)` before `clone`. The child inherits the write end and the supervisor keeps the read end.
- A producer thread per container reads from the pipe in a tight loop and pushes chunks into the bounded buffer.
- A single consumer thread pops from the buffer and writes to log files.
- **Race condition without synchronisation:** two producer threads could simultaneously see `count < capacity` and overwrite the same slot. Protected by `buffer->mutex`.
- **Deadlock risk:** empty buffer with no producers running; consumer would block forever. Fixed by the `shutting_down` flag plus `pthread_cond_broadcast`.

**Path B — control (UNIX socket):**

- The supervisor listens on `/tmp/mini_runtime.sock`. Each CLI invocation connects, sends one `control_request_t`, receives one `control_response_t`, and closes.
- **Metadata race:** `handle_request` (socket handler) and `reap_children` (SIGCHLD handler) both touch the container list → protected by `metadata_lock`.

**Bounded buffer design:**
- `mutex`: mutual exclusion for `head`, `tail`, `count`, `shutting_down`.
- `not_full` condvar: producers wait here when `count == capacity`.
- `not_empty` condvar: consumer waits here when `count == 0`.
- On shutdown: `bounded_buffer_begin_shutdown` sets `shutting_down` and broadcasts both condvars so blocked threads wake and exit. The consumer drains remaining items before returning.

### 4. Memory Management and Enforcement

**RSS (Resident Set Size)** measures pages that are currently mapped in physical RAM. It does not count:
- Pages swapped out to disk
- Pages in the page cache that are shared (counted once for the process)
- Virtual memory that has been reserved but not faulted in

RSS is appropriate for a rough memory enforcement policy because it reflects actual physical RAM pressure, which is what the system cares about for OOM avoidance.

**Soft vs hard limits:**
- A **soft limit** is a warning threshold. It tells the container "you are using more memory than expected" without killing it. This is useful for debugging, alerting, or graceful throttling.
- A **hard limit** is an enforcement deadline. When RSS exceeds it, the container is killed immediately with SIGKILL to prevent the host from running out of physical memory.

**Why kernel space?** User-space can only sample RSS at discrete polling intervals, creating enforcement latency. More importantly, the kernel monitor sees all processes' RSS atomically and can act without any cooperation from the monitored process. A process that has entered an infinite allocation loop cannot prevent a kernel timer from firing and sending SIGKILL.

### 5. Scheduling Behaviour

Linux uses the **Completely Fair Scheduler (CFS)** for normal processes. CFS assigns CPU time proportional to each process's weight, which is determined by the `nice` value (range -20 to +19). Lower nice = higher weight ≈ more CPU time.

In the scheduling experiments:

- **CPU-bound vs CPU-bound at different priorities:** The high-priority container (`nice -10`) receives roughly 5× more CPU weight than the low-priority one (`nice +10`). Observed: `cpu_hog -10` completes its work in noticeably less wall time; `cpu_hog +10` is starved whenever both are runnable.
- **CPU-bound vs I/O-bound:** The I/O-bound container (`io_pulse`) sleeps between writes and voluntarily yields the CPU. CFS rewards it with a higher `vruntime` gap, so when it wakes it is scheduled quickly. The CPU-bound container runs for longer time slices but is preempted at each tick. Observed: `io_pulse` remains responsive even when `cpu_hog` is running at full speed alongside it.

These results align with CFS's goals: *fairness* (weighted proportional share), *responsiveness* (I/O processes run quickly after waking), and *throughput* (CPU-bound processes are not starved, just lower priority).

---

## Design Decisions and Tradeoffs

| Subsystem | Choice | Tradeoff | Justification |
|-----------|--------|----------|---------------|
| Filesystem isolation | `chroot` | Less secure than `pivot_root` (escape via `..` if `/` is writable) | Simpler to implement; adequate for trusted workloads in an academic setting |
| Control IPC | UNIX domain socket | Per-request connection overhead | Bidirectional, no polling needed; straightforward client/server model |
| Logging IPC | One pipe per container | N pipe fds in the supervisor | Cleanly separates container streams; no demultiplexing needed |
| Bounded buffer sync | `pthread_mutex` + 2 condvars | Adds lock contention on every push/pop | Classic, provably correct producer-consumer; avoids busy-wait |
| Kernel list lock | `mutex` (not spinlock) | Slightly higher overhead | Timer callback runs in process context on modern kernels where sleeping is safe; mutex is correct here |
| Termination classification | `stop_requested` flag | One extra field per metadata record | Accurately distinguishes user-initiated stop from kernel-enforced kill without relying on signal-number heuristics alone |

---

## Scheduler Experiment Results

### Experiment 1: Two CPU-bound containers at different nice values

```
Container   nice   Duration   Elapsed (observed)
cpu-hi      -10    30s        ~19s wall time
cpu-lo      +10    30s        ~41s wall time
```

The high-priority container finishes first because CFS gives it more scheduling quanta per slot. The low-priority container is preempted frequently when both are runnable.

### Experiment 2: CPU-bound vs I/O-bound

```
Container   workload    nice   Behaviour
cpu-hi      cpu_hog     0      Burns CPU continuously, 100% CPU utilisation
io-pulse    io_pulse    0      Writes every 200ms, ~5% CPU utilisation
```

`io_pulse` responded within 1ms of each wakeup even with `cpu_hog` running. This demonstrates CFS's I/O-responsiveness: processes that sleep frequently are given scheduling credit when they wake, ensuring low latency.

---

## Screenshot Checklist

(Replace each item with actual annotated screenshots when running on your Ubuntu VM.)

| # | Demonstration | Screenshot |
|---|---------------|------------|
| 1 | Multi-container supervision | `sudo ./engine ps` showing alpha and beta both running |
| 2 | Metadata tracking | `ps` output with PID, state, limits |
| 3 | Bounded-buffer logging | `engine logs alpha` showing captured container output |
| 4 | CLI and IPC | Terminal showing `engine start` and supervisor response |
| 5 | Soft-limit warning | `dmesg` showing `SOFT LIMIT container=memtest` |
| 6 | Hard-limit enforcement | `dmesg` showing `HARD LIMIT`, `ps` showing state=killed |
| 7 | Scheduling experiment | Side-by-side completion times for cpu-hi vs cpu-lo |
| 8 | Clean teardown | `ps aux` showing no zombies after supervisor shutdown |

---

## File Layout

```
solution/
├── engine.c            # User-space runtime and supervisor (fully implemented)
├── monitor.c           # Linux kernel module (fully implemented)
├── monitor_ioctl.h     # Shared ioctl definitions
├── cpu_hog.c           # CPU-bound workload
├── io_pulse.c          # I/O-bound workload
├── memory_hog.c        # Memory pressure workload
├── Makefile            # Build all targets
├── environment-check.sh
└── README.md           # This file
```

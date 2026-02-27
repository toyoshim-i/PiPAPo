# PicoPiAndPortable

**A UNIX-like Micro OS for RP2040/RP2350 — Design Specification v0.2**

February 2026

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Memory Architecture](#2-memory-architecture)
3. [File System](#3-file-system)
4. [Kernel Design](#4-kernel-design)
5. [System Calls](#5-system-calls)
6. [busybox Support Strategy](#6-busybox-support-strategy)
7. [Boot Sequence](#7-boot-sequence)
8. [Device Drivers](#8-device-drivers)
9. [Development Roadmap](#9-development-roadmap)
10. [Technical Challenges and Risks](#10-technical-challenges-and-risks)
11. [Development Environment](#11-development-environment)
12. [Design Principles Summary](#12-design-principles-summary)

---

## 1. Project Overview

### 1.1 Background and Motivation

The RP2040 is a dual-core ARM Cortex-M0+ microcontroller developed by the Raspberry Pi Foundation. It operates at 133MHz and includes 264KB of on-chip SRAM but has no internal flash memory. This project aims to build a UNIX-like operating system on the RP2040 by leveraging external flash and an SD card, following the traditional UNIX design philosophy. Future porting to the RP2350 (Cortex-M33) with enhanced MPU capabilities is also in scope.

### 1.2 Project Goals

- Place the root file system (/bin, /sbin, /etc) on external flash as romfs, executing directly via XIP
- Mount the SD card at /usr to store additional applications and user data
- Implement a traditional UFS (Unix File System)
- Provide a POSIX-subset system call interface
- Run busybox (statically linked) with an interactive ash shell
- Maintain a clear porting path to the RP2350 (Cortex-M33 + enhanced MPU)

### 1.3 Target Hardware

| Item | RP2040 (Initial Target) | RP2350 (Future Target) |
|---|---|---|
| CPU | Dual Cortex-M0+ @ 133MHz | Dual Cortex-M33 @ 150MHz |
| On-chip SRAM | 264KB (6 banks) | 520KB |
| MMU / MPU | MPU with 4 regions (no MMU) | MPU with 8 regions + SAU (TrustZone) |
| Floating Point | None | Single-precision FPU |
| External Flash | 2–16MB (QSPI, XIP capable) | Same (PSRAM also supported) |
| SD Card | microSD (SPI interface) | Same |
| Notes | No Thumb-2 instructions | Thumb-2, DSP extensions, HW divide |

### 1.4 RP2350 Porting Outlook

The RP2350's Cortex-M33 is based on the ARMv8-M architecture, with the MPU expanded to 8 regions. The OS design includes an abstraction layer for memory protection boundaries that is independent of the number of MPU regions. On the RP2350, the additional regions enable per-process stack/heap protection on top of kernel region protection. Furthermore, the RP2350 optionally supports PSRAM (QSPI-connected external SRAM, up to 16MB), which can be used to extend the page pool and dramatically increase the number of concurrent processes and per-process memory. Note that the RP2350 does not have a hardware MMU, but practical memory protection and near-virtual-memory behavior can be achieved through the enhanced MPU and PSRAM.

---

## 2. Memory Architecture

### 2.1 Two-Layer Storage + RAM Configuration

The flash block cache mode considered in v0.1 was rejected due to the high cost of sector erase during writes (50–400ms), the implementation complexity of wear leveling, and contention with the XIP code region for flash capacity. Flash is dedicated exclusively to read-only code execution and romfs, resulting in a simpler and more reliable design.

| Layer | Media | Capacity | Purpose | File System |
|---|---|---|---|---|
| Root (Flash) | External QSPI Flash | 2–16MB | /, /bin, /sbin, /etc — XIP-executable system core | romfs (read-only) |
| User (SD) | microSD (SPI) | 2–32GB | /usr, /home, /var, /tmp — user data and additional apps | UFS (read-write) |
| RAM | On-chip SRAM | 264KB | Kernel, process execution, I/O buffers | — |

### 2.2 External Flash Layout

The external flash is partitioned as follows. The romfs image is generated at build time, and the entire flash is written as a single image.

| Offset | Size (approx.) | Contents |
|---|---|---|
| 0x00000000 | 4KB | Stage 1 bootloader (RP2040 QSPI initialization) |
| 0x00001000 | 48–64KB | Kernel code (.text, .rodata) — XIP execution |
| 0x00011000 | Remaining space | romfs image (/bin, /sbin, /etc file tree) — XIP execution |

The statically linked busybox binary (200–400KB in minimal Thumb configuration) is placed within the romfs at /bin. Since all romfs content is executed directly from flash via XIP, there is no need to copy code into SRAM — only stack and heap are allocated in RAM.

### 2.3 SRAM Memory Map

The 264KB of on-chip SRAM is partitioned as follows. With the flash block cache eliminated, the I/O buffer has been expanded to 24KB to serve as a file system metadata cache (UFS superblock, inodes, etc.).

| Region | Address Range | Size | Purpose |
|---|---|---|---|
| Kernel Data | 0x20000000 – 0x20003FFF | 16KB | Kernel BSS, stack, global data |
| Page Pool | 0x20004000 – 0x20037FFF | 208KB | User process pages (4KB × 52 pages) |
| I/O Buffer | 0x20038000 – 0x2003DFFF | 24KB | SD card I/O, FS metadata cache |
| DMA / Reserved | 0x2003E000 – 0x20041FFF | 16KB | DMA, PIO, Core 1 stack, interrupt stack |

Since the kernel's code (.text) and read-only data (.rodata) are executed via XIP from flash, only the data sections reside in SRAM. This reduces the kernel's SRAM footprint from 48KB to 16KB.

### 2.4 Paging Mechanism

As the Cortex-M0+ lacks an MMU, a software-based overlay paging scheme is implemented.

- Page size: fixed at 4KB
- User process data (stack, heap) is managed in page-sized units
- Code segments are either XIP from flash or loaded on demand from SD
- The SRAM page pool (52 pages, 208KB) is managed with an LRU algorithm
- Page-in source: binaries in flash romfs → direct XIP (no page-in needed)
- Page-in source: binaries on SD → loaded into SRAM (~1–5ms per 4KB)
- Dirty pages (heap, stack) are written back to SD on swap-out

---

## 3. File System

### 3.1 Design Philosophy

The custom picoFS considered in v0.1 was rejected in favor of implementing a traditional UFS (Unix File System). UFS has a long track record in BSD-family UNIX systems, and its structures — inodes, indirect blocks, cylinder groups — are well documented. This reduces the learning curve and allows leveraging existing knowledge.

### 3.2 VFS (Virtual File System)

Following standard UNIX practice, a VFS layer abstracts the underlying file system implementations. The VFS layer requires each FS driver to provide the following operation vectors: open, close, read, write, lseek, stat, readdir, mkdir, unlink, link, symlink, rename, mount, umount.

### 3.3 Supported File Systems

| FS Type | Mount Point | Media | Mode | Description |
|---|---|---|---|---|
| romfs | / | External Flash | Read-only | Kernel, core commands, config files. XIP-capable |
| UFS | /usr | SD Card | Read-write | Additional apps, libraries, user data |
| UFS | /home | SD Card | Read-write | User home directories |
| UFS | /var | SD Card | Read-write | Logs, runtime data |
| tmpfs | /tmp | SRAM | Read-write | Temporary files (RAM-backed, capacity limited) |
| devfs | /dev | RAM | Read-only | Device files: null, zero, ttyS0, etc. |
| procfs | /proc | RAM | Read-only | Process info: pid, status, meminfo, etc. |

### 3.4 romfs Design

The romfs on flash is a simple read-only file system. It is inspired by Linux's romfs but designed with XIP compatibility as the top priority.

- File data is stored with 4-byte alignment (required for ARM Thumb instruction fetch)
- ELF binary .text sections are placed at flash physical addresses for direct XIP execution
- Directories use a linked list or sorted array; linear search is sufficient given the small number of entries
- Symbolic link support (essential for the /bin/ls → /bin/busybox multicall layout)
- A mkromfs host tool generates the flash image at build time

### 3.5 UFS Design

The UFS on SD card is a simplified implementation based on 4.4BSD's FFS (Fast File System). The following simplifications are made for RP2040's resource constraints:

- Block size: 4KB (matched to SD card page size to avoid read-modify-write)
- Fragment size: eliminated (block size = fragment size = 4KB)
- Cylinder groups: single group (partitioning has little benefit in small-capacity environments)
- Inode size: 64 bytes (reduced from BSD's standard 128 bytes by removing unnecessary fields)
- Direct blocks: 10, indirect blocks: single-level only (max file size ~4MB; double-indirect added as needed)
- UNIX permissions (owner/group/other), timestamps (mtime, ctime) supported
- Hard links and symbolic links supported

### 3.6 File System Layout

The overall directory tree is as follows. The flash romfs provides the root (/), and UFS partitions on the SD card are mounted under /usr and other paths.

- `/` — romfs (Flash) — system root
  - `/bin` — core commands (busybox symlink farm)
  - `/sbin` — system administration commands (init, mount, etc.)
  - `/etc` — configuration files (inittab, fstab, passwd, profile)
  - `/dev` — devfs mount point (auto-mounted by kernel)
  - `/proc` — procfs mount point (auto-mounted by kernel)
  - `/tmp` — tmpfs mount point
- `/usr` — UFS (SD) — additional programs and data
  - `/usr/bin` — additional commands
  - `/usr/lib` — shared libraries (future use)
  - `/usr/share` — data files
- `/home` — UFS (SD) — user home directories
- `/var` — UFS (SD) — logs, runtime data
  - `/var/log` — syslog, etc.
  - `/var/run` — PID files, etc.

---

## 4. Kernel Design

### 4.1 Kernel Architecture

A monolithic kernel architecture is adopted. A microkernel design is disadvantageous in this environment due to the overhead of message passing and the buffer cost in limited SRAM. The monolithic approach is superior in both code size and performance. The same monolithic architecture will be maintained for the RP2350 port.

### 4.2 Kernel Components

| Component | Flash (.text) | SRAM (.bss/.data) | Description |
|---|---|---|---|
| Process Management | 6KB | 2KB | PCB, scheduler, context switch |
| Memory Management | 6KB | 3KB | Page pool management, overlay, swapper |
| VFS + romfs Driver | 6KB | 1KB | VFS layer, romfs read, mount table |
| UFS Driver | 8KB | 2KB | Inode management, block allocation, no-journal simplified version |
| devfs + procfs | 3KB | 1KB | Pseudo file systems |
| Device Drivers | 8KB | 2KB | UART, SPI (SD), QSPI, GPIO, I2C, ADC |
| System Calls | 4KB | 0.5KB | POSIX-subset dispatcher |
| Bootloader | 4KB | 0.5KB | Flash/SD initialization, mount |
| MPU Abstraction Layer | 2KB | 0.5KB | RP2040 (4) / RP2350 (8) region support |
| **Total** | **~47KB** | **~12.5KB** | **Flash XIP + SRAM data** |

Since kernel code runs via XIP from flash, only ~12.5KB of data needs to reside in SRAM. This fits comfortably within the 16KB kernel data region allocated in the memory map.

### 4.3 Process Model

A minimal process model is implemented to support busybox operation.

**PCB (Process Control Block):** Holds the process ID, parent PID, register context (R0–R12, SP, LR, PC, xPSR), page table, file descriptor table (up to 16 fds per process), current directory, signal mask, and signal handlers. PCB size is approximately 256 bytes per process.

**vfork + exec model:** fork() requires duplicating the address space, which is prohibitively expensive without an MMU. Instead, vfork() is adopted — the child process runs in the parent's address space and immediately calls execve(). The parent is blocked during vfork(). A Copy-on-Write capable fork() will be considered for the RP2350 port.

**Maximum concurrent processes:** Due to SRAM constraints, up to 8 processes can run simultaneously. The PCB table is fixed-size (8 entries × 256B = 2KB) and placed in the kernel data region. Pages belonging to sleeping processes are swapped out to SD via LRU as needed.

### 4.4 Scheduler

A preemptive round-robin scheduler is adopted. The SysTick timer generates a 10ms time slice, giving each process an equal share of CPU time. Processes waiting for I/O voluntarily sleep and yield control to the scheduler. For dual-core utilization, Core 0 handles kernel and user process execution, while Core 1 is dedicated to SD card I/O (DMA transfers, page-in/out). Inter-core synchronization uses the RP2040's hardware spinlocks (32 available).

### 4.5 ELF Loader and Position-Independent Code

All user programs are compiled as position-independent code (PIC) for ARM Thumb using the -fpic option. The ELF loader processes the relocation table at load time to place programs at arbitrary addresses. Binaries in flash romfs require no relocation for XIP execution (they are placed at fixed flash base addresses at build time). Binaries on SD (/usr/bin, etc.) are loaded into SRAM with relocation applied at load time.

### 4.6 MPU Abstraction and Memory Protection

The RP2040's MPU has only 4 regions, but provides a minimum level of protection. Region 0 is assigned to kernel data (privileged access only), Region 1 to the entire flash (read + execute), Region 2 to the currently running process's stack/heap, and Region 3 to I/O buffers and peripherals. Region 2 is reconfigured on each context switch. An MPU abstraction layer allows the RP2350 (8 regions) to provide finer-grained protection for per-process stack, heap, and code regions.

---

## 5. System Calls

A minimal POSIX subset is implemented to support busybox operation. The SVC instruction transitions to kernel mode (privileged mode), with arguments in registers r0–r3 and the syscall number in r7, following the ARM EABI convention.

### 5.1 Process Management

| Syscall | Number | Description |
|---|---|---|
| _exit(status) | 1 | Terminate process |
| vfork() | 190 | Create child process (parent blocked) |
| execve(path, argv, envp) | 11 | Execute program (ELF load) |
| waitpid(pid, status, opts) | 7 | Wait for child process termination |
| getpid() / getppid() | 20/64 | Get process ID |
| kill(pid, sig) | 37 | Send signal |
| sigaction(sig, act, old) | 67 | Set signal handler |
| nanosleep(req, rem) | 162 | Sleep |

### 5.2 File Operations

| Syscall | Number | Description |
|---|---|---|
| open(path, flags, mode) | 5 | Open file (dispatched via VFS to romfs/UFS/devfs) |
| close(fd) | 6 | Close file |
| read(fd, buf, count) | 3 | Read |
| write(fd, buf, count) | 4 | Write |
| lseek(fd, offset, whence) | 19 | Seek |
| stat / fstat / lstat | 106/108/107 | Get file information (inode data) |
| dup(fd) / dup2(fd, fd2) | 41/63 | Duplicate file descriptor |
| pipe(fds) | 42 | Create pipe (SRAM ring buffer) |
| ioctl(fd, req, arg) | 54 | Device control |
| getcwd / chdir | 183/12 | Current directory operations |
| mkdir / rmdir | 39/40 | Create/remove directory (UFS only) |
| unlink / link / symlink | 10/9/83 | File operations (UFS only) |
| rename | 38 | Rename file (same FS only) |
| getdents | 141 | Read directory entries |

### 5.3 Memory and System

| Syscall | Number | Description |
|---|---|---|
| brk / sbrk | 45 | Set/extend heap boundary |
| mmap (simplified) | 90 | Anonymous mappings only (allocated from page pool) |
| munmap | 91 | Free memory |
| time / gettimeofday | 13/78 | Get time |
| uname | 122 | System information (returns PicoPiAndPortable, armv6m, etc.) |
| mount / umount | 21/22 | File system mount operations |
| fcntl | 55 | File descriptor control |

---

## 6. busybox Support Strategy

### 6.1 Build Approach

busybox is cross-compiled for ARM Thumb (armv6m) using musl libc with full static linking. In minimal configuration (ash + basic coreutils), the binary is approximately 200–400KB. This binary is placed in the flash romfs at /bin/busybox and executed directly via XIP.

### 6.2 Minimum Applet Set

| Category | Applets | Notes |
|---|---|---|
| Shell | ash | POSIX-compliant lightweight shell. Pipes, redirection, variable expansion |
| File Operations | ls, cp, mv, rm, cat, mkdir, rmdir, ln, chmod | Basic file operations |
| Text Processing | echo, printf, grep, head, tail, wc, sort, sed | Pipeline processing |
| Process Management | ps, kill, sleep | Process control |
| System | mount, umount, df, free, uname, dmesg | System administration |
| Initialization | init | PID 1 process. Launches ash based on /etc/inittab |

### 6.3 XIP Execution Model

The busybox binary resides on flash and is executed directly by the CPU via XIP. Since busybox is a multicall binary, all applets are contained in a single binary and invoked through symbolic links (e.g., /bin/ls → /bin/busybox). SRAM consumption per applet execution is limited to each process's stack (4–8KB) and heap only — the code segment consumes no SRAM at all. This means that even with 4 concurrent processes (e.g., ash shell + 3-stage pipeline), no additional memory is needed for code.

### 6.4 musl libc Porting

The Linux system call wrapper layer in musl libc is rewritten for this OS. Specifically, musl's arch/arm/syscall_arch.h is replaced to issue system calls via the SVC instruction targeting PicoPiAndPortable's syscall numbers. Key porting areas include:

- syscall(): SVC instruction trap mapped to PicoPiAndPortable's syscall numbers
- pthread: single-thread stub in the initial stage; TLS via a simplified implementation (single global variable)
- mmap: anonymous mappings only (malloc backend); file mappings not supported
- signal: POSIX-compliant sigaction/sigprocmask; required by busybox's ash and wait
- stdio: standard fd-based operations; internal buffering uses musl's user-space implementation as-is

---

## 7. Boot Sequence

The boot sequence from power-on to shell prompt is described below.

**Stage 0 — ROM Boot (RP2040 built-in):** The on-chip boot ROM loads and executes the Stage 1 bootloader from the first 256 bytes of external QSPI flash.

**Stage 1 — Flash Init (4KB):** Configures the QSPI controller (clock divider, XIP mode enable, Quad Read setup). From this point on, the entire flash is memory-mapped and accessible via XIP.

**Stage 2 — Kernel Early Init:** Sets the system clock to 133MHz. Zeroes SRAM. Copies the interrupt vector table to SRAM (or references the table on flash). Initializes UART (115200bps) — console output begins here.

**Stage 3 — Kernel Init:** Initializes the memory manager (builds the page pool, registers 52 pages on the free list). Configures the MPU (4-region layout). Initializes the romfs driver (validates the romfs header on flash, mounts as root). Starts Core 1 (begins I/O worker thread standby).

**Stage 4 — SD Mount:** Initializes the SPI0 bus, detects the SD card, and runs the CMD0/CMD8/ACMD41 initialization sequence. Reads the partition table on the SD card and mounts UFS partitions at /usr, /home, and /var.

**Stage 5 — User Space:** Launches /sbin/init (busybox init) as PID 1 (XIP execution from romfs). init reads /etc/inittab and spawns ash on the console (/dev/ttyS0). The shell reads /etc/profile and displays the prompt.

The boot time target is under 2 seconds from power-on to shell prompt. SD card initialization (200–500ms) is the largest bottleneck. If no SD card is present, the system boots with romfs only, providing a limited shell.

---

## 8. Device Drivers

All drivers are built into the kernel (statically linked). Loadable modules are not adopted, as the complexity outweighs the benefits in the RP2040 environment.

| Driver | Device File | Description |
|---|---|---|
| UART | /dev/ttyS0 | Serial console (115200bps). Default for stdin/stdout/stderr |
| SPI (SD) | /dev/mmcblk0 | SD card block device. Uses SPI0, DMA transfer capable |
| QSPI Flash | (direct mapped) | XIP memory-mapped. Not exposed as a block device |
| GPIO | /dev/gpio | GPIO control (direction, read/write values; sysfs-style interface) |
| I2C | /dev/i2c0 | I2C bus (external sensors, etc.) |
| ADC | /dev/adc | Analog input (including on-chip temperature sensor) |
| null / zero | /dev/null, /dev/zero | Pseudo devices |
| random | /dev/urandom | Random number source based on RP2040's ring oscillator |
| Watchdog | /dev/watchdog | Watchdog timer |

---

## 9. Development Roadmap

| Phase | Estimated Duration | Milestone |
|---|---|---|
| Phase 0: Environment Setup | 2 weeks | Toolchain, UART output, minimal bootloader, flash XIP verification |
| Phase 1: Kernel Foundation | 4 weeks | Memory management, context switch, SVC, MPU setup, Core 1 startup |
| Phase 2: romfs + VFS | 3 weeks | mkromfs tool, romfs driver, VFS layer, root mount |
| Phase 3: Process Execution | 4 weeks | ELF loader, vfork/exec, pipe, signals, XIP execution verification |
| Phase 4: SD + UFS | 4 weeks | SPI driver, SD card initialization, UFS read/write, /usr mount |
| Phase 5: musl + busybox | 4 weeks | musl porting, busybox build, syscall coverage, interactive ash |
| Phase 6: Stabilization | 4 weeks | Error handling, OOM killer, performance tuning |
| Phase 7: RP2350 Port | 4 weeks | MPU 8-region support, PSRAM support, Thumb-2 optimization |

---

## 10. Technical Challenges and Risks

### 10.1 XIP Constraints and Mitigations

XIP execution depends on flash read speed. Cache misses trigger QSPI accesses, causing stalls of several cycles. The RP2040 includes a 16KB XIP cache, and hot code paths such as loops are automatically cached. Since busybox applets generally consist of small code, the XIP cache is expected to be highly effective. However, latency-critical code such as interrupt handlers must be placed in SRAM.

### 10.2 Memory Protection Without MMU

The RP2040's 4-region MPU cannot achieve full process isolation. A malicious process could read or write another process's memory. This OS is designed as a single-user, trusted-program execution environment, with full memory protection to be improved incrementally via the RP2350 port.

### 10.3 UFS Implementation Complexity

A complete UFS implementation represents significant work. In the initial phase, the implementation will have no journaling and minimal fsck, with crash consistency at the level of "writes since the last sync may be lost." During the stabilization phase, metadata soft updates (BSD-style) or a simple log-based approach will be considered.

### 10.4 busybox Compatibility

busybox is developed with the assumption of a Linux kernel, and some applets depend on Linux-specific system calls (clone, epoll, inotify, specific /proc entries, etc.). The ash shell and basic command set are prioritized, with Linux-specific features returning stubs or ENOSYS. Syscall coverage will be expanded incrementally.

### 10.5 SD Card Reliability

SD card communication in SPI mode supports CRC-based error detection, but is vulnerable to sudden card removal or power loss. For power loss during writes, UFS metadata write ordering (safe order: inode → data → directory) is enforced to enable recovery via fsck.

---

## 11. Development Environment

| Item | Tool / Configuration |
|---|---|
| Compiler | arm-none-eabi-gcc (ARM GCC Toolchain) |
| C Standard Library | musl libc (armv6m-thumb static cross-compiled) |
| Build System | CMake + Raspberry Pi Pico SDK (RP2040) / Pico 2 SDK (RP2350) |
| Debugger | OpenOCD + GDB (SWD interface) |
| Emulator | QEMU (arm-system) for kernel unit testing |
| Serial Communication | minicom / screen (115200bps) |
| romfs Tool | mkromfs (custom) — generates flash image on host |
| UFS Tool | mkufs (custom) — SD card formatting / newfs equivalent |
| Source Code | C (kernel), ARM Thumb assembly (boot, context switch) |

---

## 12. Design Principles Summary

- **KISS Principle:** Favor simple, predictable designs over "clever but complex" mechanisms like flash block caching
- **Traditional UNIX:** Follow the classic two-disk layout of romfs (/) + UFS (/usr), leveraging proven design patterns
- **Maximize XIP:** Execute all kernel and busybox code via XIP from flash, reserving SRAM exclusively for data
- **busybox First:** All design decisions prioritize running busybox ash as the primary goal
- **Incremental Development:** Start with single-process shell execution, then expand to multi-process and eventually multi-user
- **Investment in the Future:** Ensure the RP2350 porting path from the outset through the MPU abstraction layer and process model design
- **POSIX-compliant by default, Linux-specific as needed:** uname returns "PicoPiAndPortable"
- **Correctness First:** Prioritize correct behavior above all else; optimize later based on profiling

# PicoPiAndPortable

**A UNIX-like Micro OS for RP2040/RP2350 — Design Specification v0.7**

March 2026

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
- Mount the SD card's VFAT partition for universal PC/Mac interoperability
- Support UFS image files on the VFAT partition, mounted via loopback for full UNIX semantics
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

### 1.4 Build Targets

Three build targets are defined, each producing a separate binary. The kernel source is shared; only drivers, pin definitions, boot sequences, and linker scripts differ per target.

| Target Signature | Board | SD Card | UART | Description |
|---|---|---|---|---|
| `qemu` | QEMU mps2-an500 | No (RAM block device for testing) | CMSDK UART | Emulated Cortex-M0+ for automated testing |
| `pico1` | Raspberry Pi Pico | No | UART0 (GP0/GP1) | Official Pico board — romfs-only operation |
| `pico1calc` | ClockworkPi PicoCalc | Yes (SPI0, full-size slot) | UART0 (GP0/GP1) | Full-featured target with SD card |

**`qemu`** — An emulated Cortex-M0+ environment (QEMU mps2-an500) used for automated kernel testing. Uses a CMSDK UART for console output and a RAM-backed block device for storage testing. SysTick-driven preemption is unreliable on this platform; cooperative scheduling via `sched_yield()` is used instead.

**`pico1`** — The official Raspberry Pi Pico board (RP2040, 2MB flash, no SD card slot). Boots with flash romfs as the sole filesystem, skipping all SD/VFAT/loopback mount stages. Useful for kernel development and testing on real hardware without PicoCalc-specific peripherals.

**`pico1calc`** — The ClockworkPi PicoCalc board (RP2040 + full-size SD card slot on SPI0). This is the full-featured target matching the complete filesystem hierarchy described in this spec: romfs root on flash, VFAT on SD card, and UFS images mounted via loopback.

Both `pico1` and `pico1calc` share the same RP2040 clock initialization, stage 1 bootloader, and core drivers. They differ in board-specific GPIO pin assignments and SD card support.

### 1.5 RP2350 Porting Outlook

The RP2350's Cortex-M33 is based on the ARMv8-M architecture, with the MPU expanded to 8 regions. The OS design includes an abstraction layer for memory protection boundaries that is independent of the number of MPU regions. On the RP2350, the additional regions enable per-process stack/heap protection on top of kernel region protection. Furthermore, the RP2350 optionally supports PSRAM (QSPI-connected external SRAM, up to 16MB), which can be used to extend the page pool and dramatically increase the number of concurrent processes and per-process memory. Note that the RP2350 does not have a hardware MMU, but practical memory protection and near-virtual-memory behavior can be achieved through the enhanced MPU and PSRAM.

Target signatures `pico2` and `pico2calc` are reserved for the RP2350 port (Phase 12).

---

## 2. Memory Architecture

### 2.1 Two-Layer Storage + RAM Configuration

The flash block cache mode considered in v0.1 was rejected due to the high cost of sector erase during writes (50–400ms), the implementation complexity of wear leveling, and contention with the XIP code region for flash capacity. Flash is dedicated exclusively to read-only code execution and romfs, resulting in a simpler and more reliable design.

| Layer | Media | Capacity | Purpose | File System |
|---|---|---|---|---|
| Root (Flash) | External QSPI Flash | 2–16MB | /, /bin, /sbin, /etc — XIP-executable system core | romfs (read-only) |
| User (SD) | microSD (SPI) | 2–32GB | VFAT partition with UFS image files for /usr, /home, /var | VFAT + UFS (loopback) |
| RAM | On-chip SRAM | 264KB | Kernel, process execution, I/O buffers | — |

### 2.2 External Flash Layout

The external flash is partitioned as follows. The romfs image is generated at build time, and the entire flash is written as a single image. The boot region size differs per target because the PicoCalc's third-party UF2 bootloader ([pelrun/uf2loader](https://github.com/pelrun/uf2loader)) reserves the first 16KB of flash.

**pico1** (Official Raspberry Pi Pico — 2 MB flash):

| Offset | Size | Contents |
|---|---|---|
| 0x00000000 | 4KB | Boot2 (256 B QSPI init) + stage1 (VTOR redirect) |
| 0x00001000 | 80KB | Kernel code (.text, .rodata) — XIP execution |
| 0x00015000 | ~1.9MB | romfs image (/bin, /sbin, /etc file tree) — XIP execution |

**pico1calc** (ClockworkPi PicoCalc — 16 MB flash):

| Offset | Size | Contents |
|---|---|---|
| 0x00000000 | 16KB | Boot2 (256 B QSPI init) + stage1 (reserved by UF2 bootloader) |
| 0x00004000 | 96KB | Kernel code (.text, .rodata) — XIP execution |
| 0x0001C000 | ~16MB | romfs image (/bin, /sbin, /etc file tree) — XIP execution |

The statically linked busybox binary (200–400KB in minimal Thumb configuration) is placed within the romfs at /bin. Since all romfs content is executed directly from flash via XIP, there is no need to copy code into SRAM — only stack and heap are allocated in RAM.

### 2.3 SRAM Memory Map

The 264KB of on-chip SRAM is partitioned as follows. The I/O buffer is sized at 24KB to accommodate both VFAT metadata (FAT table, directory entries) and UFS metadata (superblock, inodes) simultaneously.

| Region | Address Range | Size | Purpose |
|---|---|---|---|
| Kernel Data | 0x20000000 – 0x20004FFF | 20KB | Kernel BSS, stack, global data |
| Page Pool | 0x20005000 – 0x20037FFF | 204KB | User process pages (4KB × 51 pages) |
| I/O Buffer | 0x20038000 – 0x2003DFFF | 24KB | SD card I/O, VFAT/UFS metadata cache |
| DMA / Reserved | 0x2003E000 – 0x20041FFF | 16KB | DMA, PIO, Core 1 stack, interrupt stack |

Since the kernel's code (.text) and read-only data (.rodata) are executed via XIP from flash, only the data sections reside in SRAM. This reduces the kernel's SRAM footprint from 48KB to 20KB.

### 2.4 Paging Mechanism

As the Cortex-M0+ lacks an MMU, a software-based overlay paging scheme is implemented.

- Page size: fixed at 4KB
- User process data (stack, heap) is managed in page-sized units
- Code segments are either XIP from flash or loaded on demand from SD
- The SRAM page pool (51 pages, 204KB) is managed with an LRU algorithm
- Page-in source: binaries in flash romfs → direct XIP (no page-in needed)
- Page-in source: binaries on SD → loaded into SRAM (~1–5ms per 4KB)
- Dirty pages (heap, stack) are written back to SD on swap-out

---

## 3. File System

### 3.1 Design Philosophy

In v0.2, the SD card was formatted directly with UFS. While this provided full UNIX semantics, it made the SD card unreadable by PCs and Macs without specialized tools. v0.3 revises this approach: the SD card uses a standard VFAT (FAT32) partition, ensuring universal interoperability. UFS functionality is provided through image files stored on the VFAT partition, which are mounted via a loopback mechanism. This is analogous to how many embedded Linux systems use ext4 images on FAT partitions, or how macOS disk images (.dmg) work.

### 3.2 VFS (Virtual File System)

Following standard UNIX practice, a VFS layer abstracts the underlying file system implementations. The VFS layer requires each FS driver to provide the following operation vectors: open, close, read, write, lseek, stat, readdir, mkdir, unlink, link, symlink, rename, mount, umount.

The VFS layer also implements the loopback mount mechanism, which interposes a block device emulation layer on top of a regular file, enabling any file system driver to mount an image file as if it were a raw device.

### 3.3 Supported File Systems

| FS Type | Mount Point | Media | Mode | Description |
|---|---|---|---|---|
| romfs | / | External Flash | Read-only | Kernel, core commands, config files. XIP-capable |
| VFAT | /mnt/sd | SD Card | Read-write | FAT32 partition. PC/Mac interoperable |
| UFS (loopback) | /usr | Image file on VFAT | Read-write | /mnt/sd/ppap_usr.img mounted as UFS |
| UFS (loopback) | /home | Image file on VFAT | Read-write | /mnt/sd/ppap_home.img mounted as UFS |
| UFS (loopback) | /var | Image file on VFAT | Read-write | /mnt/sd/ppap_var.img mounted as UFS |
| tmpfs | /tmp | SRAM | Read-write | Temporary files (RAM-backed, capacity limited) |
| devfs | /dev | RAM | Read-only | Device files: null, zero, ttyS0, etc. |
| procfs | /proc | RAM | Read-only | Process info: pid, status, meminfo, etc. |

### 3.4 SD Card Layout

The SD card is formatted as a single FAT32 partition, readable by any PC, Mac, or Linux machine. PicoPiAndPortable's system files coexist with arbitrary user files on the same partition.

```
SD Card (FAT32)
├── ppap_usr.img          # UFS image → mounted at /usr
├── ppap_home.img         # UFS image → mounted at /home
├── ppap_var.img          # UFS image → mounted at /var
├── ppap_swap.img         # Swap file (optional)
├── ppap.conf             # Boot configuration (optional overrides)
└── (arbitrary user files) # Freely accessible from PC/Mac
```

The UFS image files use the `ppap_` prefix to avoid name collisions with user files. Image sizes are configured at setup time (e.g., ppap_usr.img = 64MB, ppap_home.img = 128MB, ppap_var.img = 32MB) and can be resized offline from a PC using the mkufs tool.

This layout provides several advantages:

- **Interoperability:** The SD card can be read and written from any PC/Mac without special drivers. Users can drop files into the FAT32 partition and access them from PicoPiAndPortable via /mnt/sd.
- **Easy provisioning:** Setting up a new SD card only requires formatting as FAT32 and copying the image files — no special partitioning tools needed.
- **Data exchange:** Files can be passed between PicoPiAndPortable and a host PC simply by placing them on the FAT32 area. The /mnt/sd mount point allows direct access from running processes.
- **Backup:** UFS images can be backed up as single files by copying them on a PC.

### 3.5 Loopback Block Device

The loopback device (`/dev/loop0`, `/dev/loop1`, ...) provides a block device interface on top of a regular file. This is the key mechanism enabling UFS images on a VFAT partition.

**Operation:** When a loopback mount is requested (e.g., mount -o loop /mnt/sd/ppap_usr.img /usr), the kernel:

1. Opens the image file via the VFAT driver, obtaining an fd
2. Creates a loopback block device that translates block read/write operations into file read/write + lseek operations on the underlying fd
3. Passes the loopback block device to the UFS driver for mounting

**I/O path:** A read from /usr/bin/foo traverses: UFS → loopback device → VFAT file I/O → SD card SPI driver. While this adds one layer of indirection compared to raw UFS on SD, the overhead is minimal — the loopback layer performs only offset calculation and fd operations, with no data copying.

**Implementation:** The loopback device maintains a mapping of (block number, block size) → (file offset). For 4KB blocks, block N maps to file offset N × 4096. The implementation is straightforward: approximately 200 lines of C, with read_block and write_block functions that call lseek + read/write on the underlying fd.

### 3.6 romfs Design

The romfs on flash is a simple read-only file system. It is inspired by Linux's romfs but designed with XIP compatibility as the top priority.

- File data is stored with 4-byte alignment (required for ARM Thumb instruction fetch)
- ELF binary .text sections are placed at flash physical addresses for direct XIP execution
- Directories use a linked list or sorted array; linear search is sufficient given the small number of entries
- Symbolic link support (essential for the /bin/ls → /bin/busybox multicall layout)
- A mkromfs host tool generates the flash image at build time

### 3.7 VFAT (FAT32) Driver

The VFAT driver provides read-write access to FAT32 partitions. Given RP2040's resource constraints, the implementation focuses on correctness and compatibility over performance.

- Supports FAT32 only (FAT12/FAT16 not required for SD cards ≥ 2GB)
- Long File Name (LFN) support via VFAT extensions (required for UFS image filenames)
- Cluster sizes: 4KB–32KB (matched to SD card allocation units)
- FAT table caching: the hot portion of the FAT table is cached in the I/O buffer region
- Write support: file creation, extension, truncation, deletion
- Limitations: no file permissions (everything appears as mode 0755/0644), no symbolic links, timestamps limited to FAT's 2-second resolution
- The VFAT driver does not need to support the full range of FAT quirks (e.g., DBCS codepages); ASCII + UTF-8 filenames via LFN are sufficient

### 3.8 UFS Design

The UFS on image files is a simplified implementation based on 4.4BSD's FFS (Fast File System). The following simplifications are made for RP2040's resource constraints:

- Block size: 4KB (matched to both VFAT cluster alignment and SD card page size)
- Fragment size: eliminated (block size = fragment size = 4KB)
- Cylinder groups: single group (partitioning has little benefit in image files)
- Inode size: 64 bytes (reduced from BSD's standard 128 bytes by removing unnecessary fields)
- Direct blocks: 10, indirect blocks: single-level only (max file size ~4MB; double-indirect added as needed)
- UNIX permissions (owner/group/other), timestamps (mtime, ctime) supported
- Hard links and symbolic links supported

### 3.9 File System Layout

The overall directory tree is as follows. The flash romfs provides the root (/), VFAT on SD is mounted at /mnt/sd, and UFS image files are loop-mounted at their respective paths.

- `/` — romfs (Flash) — system root
  - `/bin` — core commands (busybox symlink farm)
  - `/sbin` — system administration commands (init, mount, etc.)
  - `/etc` — configuration files (inittab, fstab, passwd, profile)
  - `/dev` — devfs mount point (auto-mounted by kernel)
  - `/proc` — procfs mount point (auto-mounted by kernel)
  - `/tmp` — tmpfs mount point
- `/mnt/sd` — VFAT (SD Card) — direct FAT32 access
- `/usr` — UFS (ppap_usr.img via loopback) — additional programs and data
  - `/usr/bin` — additional commands
  - `/usr/lib` — shared libraries (future use)
  - `/usr/share` — data files
- `/home` — UFS (ppap_home.img via loopback) — user home directories
- `/var` — UFS (ppap_var.img via loopback) — logs, runtime data
  - `/var/log` — syslog, etc.
  - `/var/run` — PID files, etc.

### 3.10 fstab Configuration

The mount configuration is stored in /etc/fstab (on romfs) and processed by init at boot time:

```
# device                      mountpoint  fstype  options
/dev/mmcblk0p1                /mnt/sd     vfat    rw
/mnt/sd/ppap_usr.img          /usr        ufs     loop,ro
/mnt/sd/ppap_home.img         /home       ufs     loop,rw
/mnt/sd/ppap_var.img          /var        ufs     loop,rw
none                          /tmp        tmpfs   rw,size=8k
none                          /dev        devfs   rw
none                          /proc       procfs  ro
```

Mount order matters: /mnt/sd must be mounted before the loopback mounts that depend on files within it. The init process handles this sequentially.

---

## 4. Kernel Design

### 4.1 Kernel Architecture

A monolithic kernel architecture is adopted. A microkernel design is disadvantageous in this environment due to the overhead of message passing and the buffer cost in limited SRAM. The monolithic approach is superior in both code size and performance. The same monolithic architecture will be maintained for the RP2350 port.

### 4.2 Kernel Components

| Component | Flash (.text) | SRAM (.bss/.data) | Description |
|---|---|---|---|
| Process Management | 6KB | 2KB | PCB, scheduler, context switch |
| Memory Management | 6KB | 3KB | Page pool management, overlay, swapper |
| VFS + Loopback | 6KB | 1.5KB | VFS layer, loopback block device, mount table |
| romfs Driver | 3KB | 0.5KB | romfs read, XIP address resolution |
| VFAT Driver | 8KB | 2KB | FAT32 read/write, LFN support, FAT cache |
| UFS Driver | 8KB | 2KB | Inode management, block allocation, simplified |
| devfs + procfs | 3KB | 1KB | Pseudo file systems |
| Device Drivers | 8KB | 2KB | UART, SPI (SD), QSPI, GPIO, I2C, ADC |
| System Calls | 4KB | 0.5KB | POSIX-subset dispatcher |
| Bootloader | 4KB | 0.5KB | Flash/SD initialization, mount |
| MPU Abstraction Layer | 2KB | 0.5KB | RP2040 (4) / RP2350 (8) region support |
| **Total** | **~58KB** | **~15.5KB** | **Flash XIP + SRAM data** |

Compared to v0.2 (~47KB code, ~12.5KB data), the addition of the VFAT driver and loopback layer increases the kernel by approximately 11KB of code and 3KB of data. Code remains on flash (XIP), and the data footprint fits within the 20KB kernel data region. The kernel data region was expanded from 16KB to 20KB in Phase 6 to accommodate additional per-process state (8 user pages per PCB).

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
| open(path, flags, mode) | 5 | Open file (dispatched via VFS to romfs/VFAT/UFS/devfs) |
| close(fd) | 6 | Close file |
| read(fd, buf, count) | 3 | Read |
| write(fd, buf, count) | 4 | Write |
| lseek(fd, offset, whence) | 19 | Seek |
| stat / fstat / lstat | 106/108/107 | Get file information (inode data) |
| dup(fd) / dup2(fd, fd2) | 41/63 | Duplicate file descriptor |
| pipe(fds) | 42 | Create pipe (SRAM ring buffer) |
| ioctl(fd, req, arg) | 54 | Device control |
| getcwd / chdir | 183/12 | Current directory operations |
| mkdir / rmdir | 39/40 | Create/remove directory (UFS and VFAT) |
| unlink / link / symlink | 10/9/83 | File operations (link/symlink UFS only) |
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
| mount / umount | 21/22 | File system mount operations (including loopback) |
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
| Block Devices | losetup | Loop device setup (maps image files to /dev/loopN) |

### 6.3 XIP Execution Model

The busybox binary resides on flash and is executed directly by the CPU via XIP. Since busybox is a multicall binary, all applets are contained in a single binary and invoked through symbolic links (e.g., /bin/ls → /bin/busybox). SRAM consumption per applet execution is limited to each process's stack (4–8KB) and heap only — the code segment consumes no SRAM at all. This means that even with 4 concurrent processes (e.g., ash shell + 3-stage pipeline), no additional memory is needed for code.

### 6.4 musl libc Porting

The Linux system call wrapper layer in musl libc is rewritten for this OS. Specifically, musl's arch/arm/syscall_arch.h is replaced to issue system calls via the SVC instruction targeting PicoPiAndPortable's syscall numbers. Key porting areas include:

- syscall(): SVC instruction trap mapped to PicoPiAndPortable's syscall numbers
- pthread: single-thread stub in the initial stage; TLS via a simplified implementation (single global variable)
- mmap: anonymous mappings only (malloc backend); file mappings not supported
- signal: POSIX-compliant sigaction/sigprocmask; required by busybox's ash and wait
- stdio: standard fd-based operations; internal buffering uses musl's user-space implementation as-is

### 6.5 Third-Party Applications

Beyond busybox, the platform supports porting existing UNIX applications that fit within the RP2040's memory constraints (128 KB data+bss per process, Thumb-1 ISA).

**Rogue 5.4.4** — The classic BSD dungeon crawler, ported with a minimal VT100 curses shim (~800 lines of C) that translates curses calls to ANSI escape sequences. The upstream source is unmodified; PPAP-specific headers (`config.h`, `curses.h`, `pwd.h`) are injected via `-isystem`. The binary is 158 KB stripped ELF (139 KB text in flash, 75 KB data+bss in SRAM). See `docs/port-rogue.md` for the full porting plan and audit.

The porting pattern — git submodule + patches directory + standalone build script + CMake integration — is designed to be reusable for future application ports.

---

## 7. Boot Sequence

The boot sequence from power-on to shell prompt is described below.

**Stage 0 — ROM Boot (RP2040 built-in):** The on-chip boot ROM loads and executes the Stage 1 bootloader from the first 256 bytes of external QSPI flash.

**Stage 1 — Flash Init (4–16KB):** The Pico SDK boot2 (256 B) configures the QSPI controller for XIP. stage1.S then sets VTOR to the kernel's vector table origin (target-dependent: 0x10001000 on pico1, 0x10004000 on pico1calc) and jumps to the kernel entry point. On PicoCalc, the first 16KB is reserved by the UF2 bootloader.

**Stage 2 — Kernel Early Init:** Sets the system clock to 133MHz. Zeroes SRAM. Copies the interrupt vector table to SRAM (or references the table on flash). Initializes UART (115200bps) — console output begins here.

**Stage 3 — Kernel Init:** Initializes the memory manager (builds the page pool, registers 51 pages on the free list). Configures the MPU (4-region layout). Initializes the romfs driver (validates the romfs header on flash, mounts as root). Starts Core 1 (begins I/O worker thread standby).

**Stage 4 — SD and VFAT Mount:** Initializes the SPI0 bus, detects the SD card, and runs the CMD0/CMD8/ACMD41 initialization sequence. Reads the MBR/partition table on the SD card, locates the FAT32 partition, and mounts it at /mnt/sd.

**Stage 5 — Loopback Mounts:** Opens ppap_usr.img, ppap_home.img, and ppap_var.img on the VFAT partition. Creates loopback block devices (/dev/loop0, /dev/loop1, /dev/loop2). Mounts each as UFS at /usr, /home, and /var respectively.

**Stage 6 — User Space:** Launches /sbin/init (busybox init) as PID 1 (XIP execution from romfs). init reads /etc/inittab and spawns ash on the console (/dev/ttyS0). The shell reads /etc/profile and displays the prompt.

The boot time target is under 2 seconds from power-on to shell prompt on `pico1calc`. SD card initialization (200–500ms) is the largest bottleneck. The sequential VFAT mount + 3 loopback mounts add approximately 100–300ms.

On the `pico1` target, Stages 4 and 5 are skipped entirely — the system boots with romfs as the sole filesystem. If no SD card is detected on `pico1calc`, the same romfs-only fallback applies, providing a limited shell.

---

## 8. Device Drivers

All drivers are built into the kernel (statically linked). Loadable modules are not adopted, as the complexity outweighs the benefits in the RP2040 environment.

| Driver | Device File | Description |
|---|---|---|
| UART | /dev/ttyS0 | Serial console (115200bps). Default for stdin/stdout/stderr |
| SPI (SD) | /dev/mmcblk0 | SD card block device. Uses SPI0, DMA transfer capable (`pico1calc` only) |
| QSPI Flash | (direct mapped) | XIP memory-mapped. Not exposed as a block device |
| Loopback | /dev/loop0–3 | Loopback block devices for UFS image file mounting (`pico1calc` only) |
| GPIO | /dev/gpio | GPIO control (direction, read/write values; sysfs-style interface) |
| I2C | /dev/i2c0 | I2C bus (external sensors, etc.) |
| ADC | /dev/adc | Analog input (including on-chip temperature sensor) |
| null / zero | /dev/null, /dev/zero | Pseudo devices |
| random | /dev/urandom | Random number source based on RP2040's ring oscillator |
| Watchdog | /dev/watchdog | Watchdog timer |
| SPI LCD | (fbcon) | ST7365P 320×320 LCD via SPI1 (`pico1calc` only). Drives the framebuffer text console |
| I2C Keyboard | (kbd) | STM32 keyboard controller on I2C1 (`pico1calc` only). Polled input with keymap translation |
| Backlight | /dev/backlight | LCD backlight brightness control (write 0–255) (`pico1calc` only) |
| Power | /dev/power | System power control (write "off" to power down) (`pico1calc` only) |
| Battery | /proc/battery | Battery voltage and percentage readout via ADC (`pico1calc` only) |

---

## 9. Development Roadmap

| Phase | Estimated Duration | Milestone |
|---|---|---|
| Phase 0: Environment Setup | 2 weeks | Toolchain, UART output, minimal bootloader, flash XIP verification |
| Phase 1: Kernel Foundation | 4 weeks | Memory management, context switch, SVC, MPU setup, Core 1 startup |
| Phase 2: romfs + VFS | 3 weeks | mkromfs tool, romfs driver, VFS layer, root mount |
| Phase 3: Process Execution | 4 weeks | ELF loader, vfork/exec, pipe, signals, XIP execution verification |
| Phase 4: SD + VFAT | 3 weeks | SPI driver, SD card initialization, FAT32 read/write, /mnt/sd mount |
| Phase 5: UFS + Loopback | 4 weeks | UFS driver, loopback block device, image file mounting at /usr etc. |
| Phase 6: musl + busybox | 4 weeks | musl porting, busybox build, syscall coverage, interactive ash |
| Phase 7: Board Support Packages | 2 weeks | Split target-specific code into per-board directories; define `qemu`, `pico1`, `pico1calc` targets |
| Phase 8: PIE Binary Optimization | 2–3 days | .rodata flash migration, split init/sh binaries, PPAP_HAS_BLKDEV gating, per-target romfs |
| Phase 9: Dual-Core Scheduling | 3 weeks | Hardware spinlocks, per-core scheduling, both cores execute user processes |
| Phase 10: Stabilization | 3 weeks | Error handling, OOM visibility, input validation, FS correctness |
| Rogue 5.4.4 Port | — | Classic dungeon crawler with minimal VT100 curses shim; verified on QEMU and hardware |
| Phase 11: PicoCalc Device Support | 4 weeks | SPI LCD driver, I2C keyboard, framebuffer text console (40×20 / 80×40), VT100 emulator, multi-TTY, `ttyctl` utility |
| Phase 12: RP2350 Port | 4 weeks | MPU 8-region support, PSRAM support, Thumb-2 optimization; add `pico2`/`pico2calc` targets |

Note: Phase 4 (VFAT) and Phase 5 (UFS + loopback) were a single phase in v0.2. They are split here because the two-layer approach requires the VFAT driver to be functional before loopback mounts can be tested.

Note: Phase 7 (Board Support Packages) was added in v0.4 to formalize multi-board support before stabilization. Prior phases developed code for `qemu` and `pico1calc` (as the implicit RP2040 target); Phase 7 restructures that code into per-target directories and adds the `pico1` target for the official Raspberry Pi Pico board.

Note: Phase 8 (PIE Binary Optimization) was inserted in v0.5 to address SRAM pressure from resident processes. Phase 9 (Dual-Core Scheduling) was added in v0.6 to utilize the RP2040's second core for user process execution. Stabilization was renumbered to Phase 10. Phase 11 (PicoCalc Device Support) adds `pico1calc`-specific peripherals: the embedded SPI/I2C display for console output and framebuffer graphics, and the I2C keyboard for interactive input. The RP2350 port was renumbered to Phase 12.

---

## 10. Technical Challenges and Risks

### 10.1 XIP Constraints and Mitigations

XIP execution depends on flash read speed. Cache misses trigger QSPI accesses, causing stalls of several cycles. The RP2040 includes a 16KB XIP cache, and hot code paths such as loops are automatically cached. Since busybox applets generally consist of small code, the XIP cache is expected to be highly effective. However, latency-critical code such as interrupt handlers must be placed in SRAM.

### 10.2 Memory Protection Without MMU

The RP2040's 4-region MPU cannot achieve full process isolation. A malicious process could read or write another process's memory. This OS is designed as a single-user, trusted-program execution environment, with full memory protection to be improved incrementally via the RP2350 port.

### 10.3 Loopback I/O Overhead

The loopback mount introduces an additional I/O indirection layer: UFS block operations are translated into VFAT file operations, which are then translated into SD card block operations. In the worst case, a single UFS block read may require multiple FAT table lookups to resolve the VFAT file's cluster chain. To mitigate this, the VFAT driver caches the cluster chain of open image files in memory (each chain entry is 4 bytes; a 64MB image with 4KB clusters requires a 64KB chain, but only the hot portion needs caching). Additionally, since UFS image files are typically large and contiguous, the cluster chains tend to be sequential, enabling fast linear scans.

### 10.4 VFAT Write Atomicity

FAT32 metadata updates (FAT table, directory entries) are not atomic. A power loss during write can leave the file system in an inconsistent state. For the VFAT partition itself, this is acceptable — the data is either user files (tolerant of minor corruption) or UFS image files (whose internal consistency is managed by UFS). For UFS image files, the UFS driver's metadata write ordering provides crash resilience within the image, and fsck can repair UFS-level inconsistencies. The worst case is a corrupted VFAT cluster chain for an image file, which would require VFAT-level chkdsk — this is mitigatable by keeping the image files defragmented.

### 10.5 Kernel Size Growth

Adding the VFAT driver (~8KB code, ~2KB data) and loopback layer (~2KB code, ~0.5KB data) increases the kernel footprint compared to v0.2. The kernel code on flash grows from ~47KB to ~58KB, which is well within the flash allocation for the kernel region (80KB on pico1, 96KB on pico1calc). The SRAM data grew beyond the original 16KB allocation; the kernel data region was expanded to 20KB in Phase 6 (reducing the page pool from 52 to 51 pages) to accommodate the larger per-process state needed for busybox support.

### 10.6 busybox Compatibility

busybox is developed with the assumption of a Linux kernel, and some applets depend on Linux-specific system calls (clone, epoll, inotify, specific /proc entries, etc.). The ash shell and basic command set are prioritized, with Linux-specific features returning stubs or ENOSYS. Syscall coverage will be expanded incrementally.

### 10.7 SD Card Reliability

SD card communication in SPI mode supports CRC-based error detection, but is vulnerable to sudden card removal or power loss. The VFAT layer is treated as best-effort for write consistency. The UFS layer within image files enforces metadata write ordering (inode → data → directory) to enable recovery via fsck. Users should be advised to run `sync` before removing the SD card.

---

## 11. Development Environment

| Item | Tool / Configuration |
|---|---|
| Compiler | arm-none-eabi-gcc (ARM GCC Toolchain) |
| C Standard Library | musl libc (armv6m-thumb static cross-compiled) |
| Build System | CMake + Raspberry Pi Pico SDK (RP2040) / Pico 2 SDK (RP2350). Three build targets: `ppap_qemu`, `ppap_pico1`, `ppap_pico1calc` |
| Debugger | OpenOCD + GDB (SWD interface) |
| Emulator | QEMU (arm-system) for kernel unit testing |
| Serial Communication | minicom / screen (115200bps) |
| romfs Tool | mkromfs (custom) — generates flash image on host |
| UFS Tool | mkufs (custom) — creates UFS image files on host |
| SD Card Setup | Format as FAT32, copy image files and optional ppap.conf |
| Source Code | C (kernel), ARM Thumb assembly (boot, context switch) |

---

## 12. Design Principles Summary

- **KISS Principle:** Favor simple, predictable designs over "clever but complex" mechanisms like flash block caching
- **Traditional UNIX:** romfs (/) + UFS (loopback on VFAT) maintains UNIX semantics while embracing real-world interoperability
- **Universal Interoperability:** The SD card is standard FAT32, readable by any PC/Mac. No special tools needed to set up or exchange files
- **Maximize XIP:** Execute all kernel and busybox code via XIP from flash, reserving SRAM exclusively for data
- **busybox First:** All design decisions prioritize running busybox ash as the primary goal
- **Incremental Development:** Start with single-process shell execution, then expand to multi-process and eventually multi-user
- **Multi-board from day one:** Target-specific code (drivers, pin definitions, boot sequences, linker scripts) is isolated in per-board directories, enabling the same kernel to run on QEMU, official Pico, and PicoCalc with minimal `#ifdef` usage
- **Investment in the Future:** Ensure the RP2350 porting path from the outset through the MPU abstraction layer and process model design
- **POSIX-compliant by default, Linux-specific as needed:** uname returns "PicoPiAndPortable"
- **Correctness First:** Prioritize correct behavior above all else; optimize later based on profiling

---

## Appendix A: Revision History

| Version | Date | Summary of Changes |
|---|---|---|
| v0.1 | Feb 2026 | Initial design with 3-layer memory hierarchy (SD/Flash cache/SRAM), picoFS |
| v0.2 | Feb 2026 | Removed flash block cache, adopted UFS on raw SD, added RP2350 porting plan |
| v0.3 | Mar 2026 | SD card changed to VFAT (FAT32) for interoperability; UFS provided via loopback-mounted image files on the VFAT partition; added loopback block device, VFAT driver, fstab configuration |
| v0.4 | Mar 2026 | Defined three build targets (`qemu`, `pico1`, `pico1calc`); added Phase 7 (Board Support Packages) to roadmap; renumbered Stabilization → Phase 8, RP2350 Port → Phase 9 |
| v0.5 | Mar 2026 | Inserted Phase 8 (PIE Binary Optimization: .rodata flash migration, split init/sh, PPAP_HAS_BLKDEV, per-target romfs); added Phase 10 (PicoCalc Device Support: display + I2C keyboard); renumbered Stabilization → Phase 9, RP2350 Port → Phase 11 |
| v0.6 | Mar 2026 | Inserted Phase 9 (Dual-Core Scheduling: hardware spinlocks, per-core state, both RP2040 cores run user processes); renumbered Stabilization → Phase 10, PicoCalc → Phase 11, RP2350 → Phase 12 |
| v0.7 | Mar 2026 | Phase 11 complete: SPI LCD driver (ST7365P), I2C keyboard (STM32), framebuffer text console with dual-mode fonts (40×20 / 80×40), VT100/ANSI escape sequence parser, multi-TTY with getty, `ttyctl` terminal utility, curses shim TIOCGWINSZ query; added display/kbd/backlight/power/battery to device driver table |

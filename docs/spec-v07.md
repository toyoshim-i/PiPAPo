# PiPAPo

**A Portable UNIX-like Micro OS — Design Specification v0.7**

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
9. [Feature Summary](#9-feature-summary)
10. [Technical Challenges and Risks](#10-technical-challenges-and-risks)
11. [Development Environment](#11-development-environment)
12. [Design Principles Summary](#12-design-principles-summary)
13. [Documentation Index](#13-documentation-index)

---

## 1. Project Overview

### 1.1 Background and Motivation

PiPAPo (PPAP) is a UNIX-like operating system designed for bare-metal microcontrollers and retro CPUs. The project began targeting the RP2040 (ARM Cortex-M0+) and has since expanded to support the Motorola 68000 (m68k) architecture. The kernel, VFS, process model, and syscall interface are shared across all architectures — only drivers, boot code, context switch, and linker scripts are target-specific.

### 1.2 Project Goals

- Provide a POSIX-subset system call interface, consistent across architectures
- Run busybox (statically linked) with an interactive hush shell
- Place the root file system on flash/ROM as romfs, with optional SD card support via VFAT + UFS loopback
- Support multiple target architectures and boards from a single source tree
- Run Rogue 5.4.4 and other ported UNIX applications

### 1.3 Supported Targets

| Target | Board / Emulator | Architecture | CPU | RAM | Flash/ROM |
|---|---|---|---|---|---|
| `qemu_arm` | QEMU mps2-an500 | ARM | Cortex-M0+ (ARMv6-M) | 4 MB | — (ROM region) |
| `pico1` | Raspberry Pi Pico | ARM | Dual Cortex-M0+ @ 133 MHz | 264 KB | 2 MB QSPI (XIP) |
| `pico1calc` | ClockworkPi PicoCalc | ARM | Dual Cortex-M0+ @ 133 MHz | 264 KB | 16 MB QSPI (XIP) |
| `qemu_m68k` | QEMU virt m68k | m68k | Motorola 68000 | Up to 16 MB (auto-detected) | — (loaded into RAM) |

The kernel source is shared; only drivers, pin definitions, boot sequences, and linker scripts differ per target.

### 1.4 Build Targets

Each target produces a separate binary:

**`qemu_arm`** — An emulated Cortex-M0+ environment (QEMU mps2-an500) used for automated kernel testing. Uses a CMSDK UART for console output and a RAM-backed block device for storage testing.

**`pico1`** — The official Raspberry Pi Pico board (RP2040, 2 MB flash, no SD card slot). Boots with flash romfs as the sole filesystem, skipping all SD/VFAT/loopback mount stages.

**`pico1calc`** — The ClockworkPi PicoCalc board (RP2040 + full-size SD card slot on SPI0). Full-featured ARM target with romfs root on flash, VFAT on SD card, LCD display, and I2C keyboard.

**`qemu_m68k`** — An emulated Motorola 68000 environment (QEMU virt machine) for m68k testing. Uses UART for console output and a RAM-backed block device for storage testing.

### 1.5 Future Targets

- **RP2350 (pico2/pico2calc)** — Cortex-M33/ARMv8-M, 8-region MPU, optional PSRAM (up to 16 MB). Target signatures reserved.
- Additional m68k hardware targets.

---

## 2. Memory Architecture

### 2.1 Storage and RAM Configuration

The storage model depends on the target hardware:

| Layer | Purpose | File System | Targets |
|---|---|---|---|
| Root (flash/ROM) | /, /bin, /sbin, /etc — read-only system core | romfs | All |
| User (SD card) | VFAT partition with UFS image files for /usr, /home, /var | VFAT + UFS (loopback) | pico1calc |
| RAM | Kernel, process execution, I/O buffers | — | All |

On ARM targets with QSPI flash, romfs content is executed directly via XIP (eXecute-In-Place). On m68k, romfs is loaded into RAM.

### 2.2 Flash Layout (ARM / RP2040 targets)

The external flash is partitioned as follows. The boot region size differs per target.

**pico1** (Official Raspberry Pi Pico — 2 MB flash):

| Offset | Size | Contents |
|---|---|---|
| 0x00000000 | 4 KB | Boot2 (256 B QSPI init) + stage1 (VTOR redirect) |
| 0x00001000 | 80 KB | Kernel code (.text, .rodata) — XIP execution |
| 0x00015000 | ~1.9 MB | romfs image (/bin, /sbin, /etc file tree) — XIP execution |

**pico1calc** (ClockworkPi PicoCalc — 16 MB flash):

| Offset | Size | Contents |
|---|---|---|
| 0x00000000 | 16 KB | Boot2 (256 B QSPI init) + stage1 (reserved by UF2 bootloader) |
| 0x00004000 | 96 KB | Kernel code (.text, .rodata) — XIP execution |
| 0x0001C000 | ~16 MB | romfs image (/bin, /sbin, /etc file tree) — XIP execution |

### 2.3 RAM Layout (ARM / RP2040)

The 264 KB of on-chip SRAM is partitioned as follows:

| Region | Address Range | Size | Purpose |
|---|---|---|---|
| Kernel Data | 0x20000000 – 0x20004FFF | 20 KB | Kernel BSS, stack, global data |
| Page Pool | 0x20005000 – 0x20037FFF | 204 KB | User process pages (4 KB × 51 pages) |
| I/O Buffer | 0x20038000 – 0x2003DFFF | 24 KB | SD card I/O, VFAT/UFS metadata cache |
| DMA / Reserved | 0x2003E000 – 0x20041FFF | 16 KB | DMA, PIO, Core 1 stack, interrupt stack |

Since the kernel's code (.text) and read-only data (.rodata) are executed via XIP from flash, only the data sections reside in SRAM.

### 2.4 RAM Layout (m68k / QEMU virt)

The m68k QEMU target has up to 16 MB RAM (auto-detected at boot). Kernel code, data, and romfs are all in RAM. The page pool is allocated from remaining memory after the kernel BSS.

### 2.5 Paging Mechanism

As none of the current targets have an MMU, a software-based overlay paging scheme is implemented.

- Page size: fixed at 4 KB
- User process data (stack, heap) is managed in page-sized units
- Code segments are either XIP from flash (ARM) or loaded into RAM pages (m68k)
- Page pool size depends on target memory
- Dirty pages (heap, stack) can be written back to SD on swap-out (targets with SD card)

---

## 3. File System

### 3.1 Design Philosophy

The SD card uses a standard VFAT (FAT32) partition, ensuring universal PC/Mac interoperability. UFS functionality is provided through image files stored on the VFAT partition, which are mounted via a loopback mechanism. Targets without SD card support (pico1, qemu_arm, qemu_m68k) run with romfs as the sole filesystem.

### 3.2 VFS (Virtual File System)

Following standard UNIX practice, a VFS layer abstracts the underlying file system implementations. The VFS layer requires each FS driver to provide the following operation vectors: open, close, read, write, lseek, stat, readdir, mkdir, unlink, link, symlink, rename, mount, umount.

The VFS layer also implements the loopback mount mechanism, which interposes a block device emulation layer on top of a regular file, enabling any file system driver to mount an image file as if it were a raw device.

### 3.3 Supported File Systems

| FS Type | Mount Point | Media | Mode | Description |
|---|---|---|---|---|
| romfs | / | Flash or RAM | Read-only | Kernel, core commands, config files. XIP-capable on ARM |
| VFAT | /mnt/sd | SD Card | Read-write | FAT32 partition. PC/Mac interoperable |
| UFS (loopback) | /usr | Image file on VFAT | Read-write | /mnt/sd/ppap_usr.img mounted as UFS |
| UFS (loopback) | /home | Image file on VFAT | Read-write | /mnt/sd/ppap_home.img mounted as UFS |
| UFS (loopback) | /var | Image file on VFAT | Read-write | /mnt/sd/ppap_var.img mounted as UFS |
| tmpfs | /tmp | RAM | Read-write | Temporary files (RAM-backed, capacity limited) |
| devfs | /dev | RAM | Read-only | Device files: null, zero, ttyS0, etc. |
| procfs | /proc | RAM | Read-only | Process info: pid, status, meminfo, etc. |

### 3.4 SD Card Layout

The SD card is formatted as a single FAT32 partition, readable by any PC, Mac, or Linux machine. PiPAPo's system files coexist with arbitrary user files on the same partition.

```
SD Card (FAT32)
├── ppap_usr.img          # UFS image → mounted at /usr
├── ppap_home.img         # UFS image → mounted at /home
├── ppap_var.img          # UFS image → mounted at /var
├── ppap_swap.img         # Swap file (optional)
├── ppap.conf             # Boot configuration (optional overrides)
└── (arbitrary user files) # Freely accessible from PC/Mac
```

### 3.5 Loopback Block Device

The loopback device (`/dev/loop0`, `/dev/loop1`, ...) provides a block device interface on top of a regular file. This is the key mechanism enabling UFS images on a VFAT partition.

**Operation:** When a loopback mount is requested (e.g., mount -o loop /mnt/sd/ppap_usr.img /usr), the kernel:

1. Opens the image file via the VFAT driver, obtaining an fd
2. Creates a loopback block device that translates block read/write operations into file read/write + lseek operations on the underlying fd
3. Passes the loopback block device to the UFS driver for mounting

### 3.6 romfs Design

The romfs on flash/ROM is a simple read-only file system. It is designed with XIP compatibility as a priority on targets that support it.

- File data is stored with 4-byte alignment (required for ARM Thumb instruction fetch via XIP)
- On ARM targets, ELF binary .text sections are placed at flash physical addresses for direct XIP execution
- Directories use a linked list; linear search is sufficient given the small number of entries
- Symbolic link support (essential for the /bin/ls → /bin/busybox multicall layout)
- A mkromfs host tool generates the image at build time

### 3.7 VFAT (FAT32) Driver

The VFAT driver provides read-write access to FAT32 partitions (targets with SD card only).

- Supports FAT32 only (FAT12/FAT16 not required for SD cards ≥ 2 GB)
- Long File Name (LFN) support via VFAT extensions
- Write support: file creation, extension, truncation, deletion
- Limitations: no file permissions, no symbolic links

### 3.8 UFS Design

The UFS on image files is a simplified implementation based on 4.4BSD's FFS (Fast File System).

- Block size: 4 KB
- Fragment size: eliminated (block size = fragment size = 4 KB)
- Inode size: 64 bytes
- Direct blocks: 10, indirect blocks: single-level only
- UNIX permissions, timestamps, hard links and symbolic links supported

### 3.9 File System Layout

```
/               romfs (flash/ROM) — system root
├── /bin        core commands (busybox symlink farm)
├── /sbin       system administration commands (init, mount, etc.)
├── /etc        configuration files (inittab, fstab, passwd, profile)
├── /dev        devfs mount point (auto-mounted by kernel)
├── /proc       procfs mount point (auto-mounted by kernel)
├── /tmp        tmpfs mount point
/mnt/sd         VFAT (SD Card) — direct FAT32 access
/usr            UFS (ppap_usr.img via loopback)
/home           UFS (ppap_home.img via loopback)
/var            UFS (ppap_var.img via loopback)
```

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

On targets without SD card, fstab entries that reference `/dev/mmcblk0` are silently skipped.

---

## 4. Kernel Design

### 4.1 Kernel Architecture

A monolithic kernel architecture is adopted. A microkernel design is disadvantageous in this environment due to the overhead of message passing and the buffer cost in limited memory. The same monolithic architecture is used across all targets.

### 4.2 Kernel Components

| Component | Code Size | Data Size | Description |
|---|---|---|---|
| Process Management | 6 KB | 2 KB | PCB, scheduler, context switch |
| Memory Management | 6 KB | 3 KB | Page pool management, overlay, swapper |
| VFS + Loopback | 6 KB | 1.5 KB | VFS layer, loopback block device, mount table |
| romfs Driver | 3 KB | 0.5 KB | romfs read, XIP address resolution |
| VFAT Driver | 8 KB | 2 KB | FAT32 read/write, LFN support, FAT cache |
| UFS Driver | 8 KB | 2 KB | Inode management, block allocation |
| devfs + procfs | 3 KB | 1 KB | Pseudo file systems |
| Device Drivers | 8 KB | 2 KB | UART, SPI, I2C, LCD, etc. (target-dependent) |
| System Calls | 4 KB | 0.5 KB | POSIX-subset dispatcher |
| Boot / Startup | 4 KB | 0.5 KB | Target-specific initialization |
| Memory Protection | 2 KB | 0.5 KB | MPU abstraction (targets with MPU) |
| **Total** | **~58 KB** | **~15.5 KB** | |

### 4.3 Process Model

A minimal process model is implemented to support busybox operation.

**PCB (Process Control Block):** Holds the process ID, parent PID, register context (architecture-specific), page table, file descriptor table (up to 16 fds per process), current directory, signal mask, and signal handlers. PCB size is approximately 256 bytes per process.

**vfork + exec model:** fork() requires duplicating the address space, which is prohibitively expensive without an MMU. Instead, vfork() is adopted — the child process runs in the parent's address space and immediately calls execve(). The parent is blocked during vfork().

**Maximum concurrent processes:** Up to 8 processes can run simultaneously. The PCB table is fixed-size (8 entries × 256 B = 2 KB).

### 4.4 Scheduler

A preemptive round-robin scheduler is adopted. A timer interrupt generates a 10 ms time slice (100 Hz), giving each process an equal share of CPU time. Processes waiting for I/O voluntarily sleep and yield control to the scheduler.

On dual-core targets (RP2040), both cores run user processes. Inter-core synchronization uses hardware spinlocks.

### 4.5 ELF Loader and Position-Independent Code

All user programs are compiled as position-independent code (PIC). The ELF loader processes the relocation table at load time to place programs at arbitrary addresses.

Architecture-specific PIC details:

| | ARM (Thumb) | m68k |
|---|---|---|
| PIC register | r9 (GOT base) | a5 (data base, `-msep-data`) |
| Compiler flags | `-fPIC -msingle-pic-base -mpic-register=r9` | `-fPIC -msep-data` |
| Relocation type | `R_ARM_RELATIVE` | `R_68K_RELATIVE` |
| XIP support | Yes (code runs from flash) | No (code in RAM) |

### 4.6 Memory Protection

Memory protection depends on the target hardware:

- **RP2040 (ARM):** 4-region MPU — kernel data (privileged only), flash (all: RO+exec), current process pages (user RW), peripherals (privileged only). Region 2 is reconfigured on each context switch.
- **RP2350 (future):** 8-region MPU — finer-grained per-process protection.
- **m68k QEMU:** No memory protection currently.

---

## 5. System Calls

A minimal POSIX subset is implemented to support busybox operation. PPAP uses a **16-bit grouped numbering** scheme that is shared across all architectures. The trap mechanism is architecture-specific:

- **ARM:** `svc 0` with syscall number in r7, arguments in r0–r5
- **m68k:** `trap #0` with syscall number in d0, arguments in d1–d5/a0

See [syscall.md](syscall.md) for the complete reference.

### 5.1 Process Management

| Syscall | Number | Description |
|---|---|---|
| exit | 0x0000 | Terminate process |
| vfork | 0x0002 | Create child process (parent blocked) |
| execve | 0x0003 | Execute program (ELF load) |
| waitpid | 0x0004 | Wait for child process termination |
| getpid / getppid | 0x0006/0x0008 | Get process ID |
| kill | 0x0600 | Send signal |
| rt_sigaction | 0x0603 | Set signal handler |
| nanosleep | 0x0500 | Sleep |

### 5.2 File Operations

| Syscall | Number | Description |
|---|---|---|
| open | 0x0102 | Open file (dispatched via VFS) |
| close | 0x0103 | Close file |
| read | 0x0100 | Read |
| write | 0x0101 | Write |
| lseek | 0x010B | Seek |
| stat64 / fstat64 | 0x0301/0x0302 | Get file information |
| dup / dup2 | 0x0104/0x0105 | Duplicate file descriptor |
| pipe | 0x0106 | Create pipe |
| ioctl | 0x0107 | Device control |
| getcwd / chdir | 0x0203/0x0207 | Current directory operations |
| mkdir / rmdir | 0x0204/0x0205 | Create/remove directory |
| unlink | 0x0206 | Delete file |
| getdents64 | 0x030A | Read directory entries |

### 5.3 Memory and System

| Syscall | Number | Description |
|---|---|---|
| brk | 0x0400 | Set/extend heap boundary |
| mmap2 | 0x0401 | Anonymous mappings only (allocated from page pool) |
| munmap | 0x0402 | Free memory |
| clock_gettime64 | 0x0504 | Get time |
| uname | 0x0007 | System information (sysname=PiPAPo, machine=arch) |
| mount / umount2 | 0x0900/0x0901 | File system mount operations |

---

## 6. busybox Support Strategy

### 6.1 Build Approach

busybox is cross-compiled for each target architecture using musl libc with full static linking. In minimal configuration (hush + basic coreutils), the binary is approximately 200–400 KB. On ARM targets, this binary is placed in the flash romfs and executed directly via XIP. On m68k targets, it is loaded into RAM from romfs.

### 6.2 Minimum Applet Set

| Category | Applets | Notes |
|---|---|---|
| Shell | hush | Lightweight shell with bash compatibility. Pipes, redirection, variable expansion |
| File Operations | ls, cp, mv, rm, cat, mkdir, rmdir, ln, chmod | Basic file operations |
| Text Processing | echo, printf, grep, head, tail, wc, sort, sed | Pipeline processing |
| Process Management | ps, kill, sleep | Process control |
| System | mount, umount, df, free, uname, dmesg | System administration |
| Initialization | init | PID 1 process. Launches hush based on /etc/inittab |
| Block Devices | losetup | Loop device setup |

### 6.3 XIP Execution Model (ARM targets)

On ARM targets, the busybox binary resides on flash and is executed directly via XIP. SRAM consumption per applet execution is limited to each process's stack and heap only — the code segment consumes no SRAM at all. On m68k targets, the code is loaded into RAM pages.

### 6.4 musl libc Porting

musl libc is ported to each target architecture. The Linux system call wrapper layer is rewritten to issue system calls via the architecture-specific trap instruction targeting PPAP's unified syscall numbers.

Key porting areas:
- syscall(): Trap instruction mapped to PPAP's syscall numbers (shared across architectures)
- pthread: single-thread stub; TLS via simplified implementation
- mmap: anonymous mappings only (malloc backend); file mappings not supported
- signal: POSIX-compliant sigaction/sigprocmask

### 6.5 Third-Party Applications

Beyond busybox, the platform supports porting existing UNIX applications that fit within memory constraints.

**Rogue 5.4.4** — The classic BSD dungeon crawler, ported with a minimal VT100 curses shim (~800 lines of C). The upstream source is unmodified; PPAP-specific headers are injected via `-isystem`. See `docs/history/port-rogue.md` for details.

The porting pattern — git submodule + patches directory + standalone build script + CMake integration — is designed to be reusable for future application ports across all architectures.

---

## 7. Boot Sequence

The boot sequence from power-on to shell prompt follows a target-specific early phase and a shared kernel initialization:

### 7.1 Early Boot (target-specific)

**ARM (RP2040):**
1. ROM Boot — on-chip boot ROM loads Stage 1 from flash
2. Stage 1 — QSPI init (boot2), sets VTOR, jumps to kernel
3. Reset_Handler — copies .data, zeroes .bss, calls kmain()

**m68k (QEMU):**
1. QEMU loads kernel ELF into RAM
2. Startup code sets up vector table, calls kmain()

### 7.2 Kernel Init (shared)

1. `target_early_init()` — UART console, clock setup
2. Page allocator init
3. Memory protection init (targets with MPU)
4. romfs mount as root (`/`)
5. `target_late_init()` — block devices, display, keyboard (target-specific)
6. Mount remaining filesystems via fstab
7. Launch additional cores (dual-core targets)
8. `execve("/sbin/init")` as PID 1

### 7.3 User Space

init reads /etc/inittab and spawns hush on the console. The shell reads /etc/profile and displays the prompt.

---

## 8. Device Drivers

All drivers are built into the kernel (statically linked). The set of active drivers depends on the target.

### 8.1 Common Devices (all targets)

| Driver | Device File | Description |
|---|---|---|
| UART | /dev/ttyS0 | Serial console (115200 bps) |
| null / zero | /dev/null, /dev/zero | Pseudo devices |
| random | /dev/urandom | Random number source |
| Loopback | /dev/loop0–3 | Loopback block devices for UFS image mounting |

### 8.2 ARM (RP2040) Devices

| Driver | Device File | Description |
|---|---|---|
| SPI (SD) | /dev/mmcblk0 | SD card block device (pico1calc only) |
| QSPI Flash | (direct mapped) | XIP memory-mapped |
| SPI LCD | (fbcon) | ST7365P 320×320 LCD via SPI1 (pico1calc only) |
| I2C Keyboard | (kbd) | STM32 keyboard controller on I2C1 (pico1calc only) |
| Backlight | /dev/backlight | LCD backlight brightness (pico1calc only) |
| Power | /dev/power | System power control (pico1calc only) |
| Battery | /proc/battery | Battery voltage readout (pico1calc only) |

### 8.3 m68k Devices

The m68k QEMU target currently uses a UART for console and a RAM-backed block device for testing. Additional device support will be added as hardware targets are introduced.

---

## 9. Feature Summary

### 9.1 Kernel

- **Process model:** vfork/exec, waitpid, process groups, sessions
- **Scheduler:** preemptive round-robin, dual-core on supported targets, hardware spinlock synchronization
- **Memory:** page allocator (4 KB pages), sbrk/brk heap, MPU-based protection (targets with MPU)
- **Signals:** POSIX sigaction/sigprocmask, signal delivery on return to user-space
- **IPC:** pipe, dup/dup2, file descriptor passing

### 9.2 File Systems

- **VFS:** mount table, path resolution, per-process file descriptor table
- **romfs:** read-only root filesystem, XIP-capable on ARM targets
- **VFAT (FAT32):** SD card filesystem for PC/Mac interoperability
- **UFS:** UNIX filesystem on loopback-mounted image files (full POSIX semantics)
- **devfs / procfs / tmpfs:** in-memory pseudo-filesystems

### 9.3 User Space

- **musl libc:** ported to ARM and m68k with PPAP's unified syscall interface
- **busybox:** statically linked multicall binary with interactive hush shell
- **Rogue 5.4.4:** classic dungeon crawler with minimal VT100 curses shim
- **User programs:** hello, init, getty, ttyctl (terminal management utility)
- **PIE/PIC binaries:** position-independent ELFs with architecture-specific relocation

### 9.4 Display and Input (PicoCalc)

- **SPI LCD:** ST7365P 320×320 display via SPI1 at ~33 MHz
- **Framebuffer console:** dual-mode text rendering (40×20 with 8×16 font, 80×40 with 4×8 font)
- **VT100/ANSI emulator:** cursor movement, scroll regions, 16-color attributes, erase/insert/delete
- **I2C keyboard:** STM32 co-processor on I2C1, polled input with keymap translation
- **Multi-TTY:** /dev/ttyS0 (serial) + /dev/tty1 (LCD+keyboard), getty login on each

### 9.5 Build Targets

| Target | Board | Features |
|---|---|---|
| `ppap_qemu_arm` | QEMU mps2-an500 | CMSDK UART, RAM block device, automated testing |
| `ppap_pico1` | Raspberry Pi Pico | romfs-only, PL011 UART, no SD card |
| `ppap_pico1calc` | ClockworkPi PicoCalc | Full feature set: SD, LCD, keyboard, dual-core |
| `ppap_qemu_m68k` | QEMU virt m68k | UART, RAM block device, automated testing |

Development history is archived in `docs/history/` (phase plans and porting notes).

---

## 10. Technical Challenges and Risks

### 10.1 XIP Constraints (ARM targets)

XIP execution depends on flash read speed. Cache misses trigger QSPI accesses, causing stalls. The RP2040 includes a 16 KB XIP cache. Latency-critical code such as interrupt handlers must be placed in SRAM. This constraint does not apply to m68k targets where code runs from RAM.

### 10.2 Memory Protection Without MMU

None of the current targets have an MMU. The RP2040's 4-region MPU cannot achieve full process isolation. The m68k QEMU target has no memory protection. PPAP is designed as a single-user, trusted-program execution environment.

### 10.3 Loopback I/O Overhead

The loopback mount introduces an additional I/O indirection layer. In the worst case, a single UFS block read may require multiple FAT table lookups. The VFAT driver caches cluster chains to mitigate this.

### 10.4 Multi-Architecture Binary Compatibility

PPAP uses a unified syscall numbering scheme across all architectures, so the kernel-side syscall dispatch table is shared. However, user-space binaries are architecture-specific (ARM Thumb ELFs vs m68k ELFs). The musl libc build and busybox build must be performed separately for each architecture.

### 10.5 busybox Compatibility

busybox is developed with the assumption of a Linux kernel, and some applets depend on Linux-specific system calls. The hush shell and basic command set are prioritized, with Linux-specific features returning stubs or ENOSYS. Syscall coverage is expanded incrementally.

### 10.6 Target-Specific Constraints

| Constraint | ARM (RP2040) | m68k (QEMU) |
|---|---|---|
| RAM | 264 KB (pico1/pico1calc) | Up to 16 MB (auto-detected) |
| ISA limitations | Thumb-1 only, no HW divide | Full 68000 ISA |
| Flash/XIP | Yes | No |
| Multi-core | Yes (dual-core) | No |
| Memory protection | MPU (4 regions) | None |

---

## 11. Development Environment

| Item | Tool / Configuration |
|---|---|
| ARM Compiler | arm-none-eabi-gcc |
| m68k Compiler | m68k-elf-gcc (custom-built) |
| C Standard Library | musl libc (per-architecture static cross-compiled) |
| Build System | CMake + Ninja. ARM targets use Pico SDK integration |
| Debugger | OpenOCD + GDB (ARM hardware); QEMU built-in GDB stub |
| Emulator | QEMU system-arm (mps2-an500), QEMU system-m68k (virt, -cpu m68000) |
| Serial Communication | minicom / screen (115200 bps) |
| romfs Tool | mkromfs (custom) — generates romfs image on host |
| UFS Tool | mkufs (custom) — creates UFS image files on host |
| Source Code | C (kernel), architecture-specific assembly (boot, context switch) |

---

## 12. Design Principles Summary

- **KISS Principle:** Favor simple, predictable designs
- **Traditional UNIX:** romfs (/) + UFS (loopback on VFAT) maintains UNIX semantics while embracing real-world interoperability
- **Universal Interoperability:** The SD card is standard FAT32, readable by any PC/Mac
- **Maximize XIP:** On targets with flash, execute code via XIP, reserving RAM exclusively for data
- **busybox First:** All design decisions prioritize running busybox hush as the primary goal
- **Architecture-neutral kernel:** Shared kernel source with thin target abstraction layer (target.h)
- **Multi-target from day one:** Target-specific code (drivers, pin definitions, boot sequences, linker scripts) is isolated in per-target directories
- **Unified syscall interface:** Same syscall numbers and semantics across all architectures
- **Correctness First:** Prioritize correct behavior above all else; optimize later based on profiling

---

## 13. Documentation Index

| Document | Audience | Description |
|---|---|---|
| [kernel.md](kernel.md) | Kernel developers | Boot sequence, memory layout, process model, scheduler, signals |
| [filesystems.md](filesystems.md) | Kernel developers | VFS layer, romfs, VFAT, UFS, loopback, devfs, procfs, tmpfs |
| [syscall.md](syscall.md) | All developers | Complete system call reference (shared across architectures) |
| [procfs.md](procfs.md) | All developers | /proc filesystem file formats |
| [userland-dev-guide.md](userland-dev-guide.md) | User-space developers | Toolchain, compiler flags, linking (ARM and m68k) |
| [porting.md](porting.md) | Application porters | Third-party porting pattern |
| [PicoCalc.md](reference/PicoCalc.md) | Hardware developers | PicoCalc pinout, I2C keyboard protocol, SD card, serial debug |
| [PicoCalc-LCD.md](reference/PicoCalc-LCD.md) | Driver developers | ST7365P LCD driver architecture, SPI protocol, VT100 emulator |
| [target-68000.md](target-68000.md) | m68k developers | m68k target-specific notes |
| [target-pizero.md](target-pizero.md) | ARM developers | Pi Zero port plan (draft) |
| [testing.md](testing.md) | All developers | Test framework, categories, adding tests |
| [feature-eCPU.md](feature-eCPU.md) | — | CPU emulation layer design (future) |
| [feature-subsystem.md](feature-subsystem.md) | — | OS personality layers design (future) |
| [history/](history/) | — | Development phase plans (archived) |

---

## Appendix A: Revision History

| Version | Date | Summary of Changes |
|---|---|---|
| v0.1 | Feb 2026 | Initial design with 3-layer memory hierarchy, picoFS |
| v0.2 | Feb 2026 | Removed flash block cache, adopted UFS on raw SD, added RP2350 plan |
| v0.3 | Mar 2026 | SD card changed to VFAT (FAT32); UFS via loopback-mounted image files |
| v0.4 | Mar 2026 | Defined three build targets (qemu, pico1, pico1calc) |
| v0.5 | Mar 2026 | PIE binary optimization, per-target romfs |
| v0.6 | Mar 2026 | Dual-core scheduling (RP2040 hardware spinlocks) |
| v0.7 | Mar 2026 | All ARM features complete (MVP). Added m68k target. Architecture-neutral redesign of spec and documentation |

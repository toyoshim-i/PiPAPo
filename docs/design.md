# PPAP Kernel Design

Internal design reference for kernel developers.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│  User Space                                             │
│  busybox (hush shell + applets), rogue, user programs   │
│  musl libc (static) — or bare-metal syscall stubs       │
├────────────────────────┬────────────────────────────────┤
│  Syscall Interface     │  Signal Delivery               │
│  svc 0 (ARM)           │  sigaction, sigprocmask        │
│  trap #0 (m68k)        │  user-stack trampoline         │
├────────────────────────┴────────────────────────────────┤
│  Kernel                                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │ Process  │ │ Memory   │ │ FD/TTY/  │ │ Scheduler │  │
│  │ (PCB,    │ │ (page    │ │ Pipe     │ │ (round-   │  │
│  │  exec,   │ │  alloc,  │ │          │ │  robin,   │  │
│  │  vfork)  │ │  brk,    │ │          │ │  SMP)     │  │
│  │          │ │  mmap)   │ │          │ │           │  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
│  ┌──────────────────────────────────────────────────┐   │
│  │ VFS  (mount table, path resolution, vnodes)      │   │
│  ├─────────┬────────┬──────┬───────┬───────┬───────┤   │
│  │ romfs   │ devfs  │procfs│ tmpfs │ VFAT  │  UFS  │   │
│  └────┬────┴────────┴──────┴───────┴───┬───┴───┬───┘   │
│       │                                │       │        │
│  ┌────┴────────────────────────────────┴───────┴───┐   │
│  │ Block Device Layer  (SD, RAM disk, loopback)    │   │
│  └─────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────┤
│  Architecture Layer (src/arch/)                         │
│  boot.S, switch.S, trap.S, arch.h, cpu.h                │
├──────────────────────┬──────────────────────────────────┤
│  Target Layer        │  Drivers                         │
│  (src/target/)       │  UART, SPI, LCD, I2C, keyboard   │
│  early_init,         │  (target-specific)               │
│  late_init, etc.     │                                  │
└──────────────────────┴──────────────────────────────────┘
```

---

## Boot Sequence

The boot sequence is target-specific but follows a common pattern:

1. **Platform boot** — hardware-specific initialization (ROM boot, flash init, etc.)
2. **Kernel entry** (target-specific startup code) — copies `.data`, zeroes `.bss`, calls `kmain()`
3. **kmain** (`src/kernel/main.c`) — unified entry using target hooks:
   - `target_early_init()` — UART console, clock setup (platform-specific)
   - Page allocator init
   - Memory protection init (if available)
   - romfs mount as root (`/`)
   - `target_late_init()` — block devices, additional hardware (target-specific)
   - Mount remaining filesystems (VFAT, UFS via fstab)
   - Launch additional cores (if supported)
   - `execve("/sbin/init")` as PID 1

### ARM (RP2040) Boot Details

1. **ROM Boot** — RP2040 boot ROM loads 256 B from QSPI flash
2. **Stage 1** (`src/boot/stage1.S`) — sets VTOR to kernel vector table, jumps to Reset_Handler
3. **Reset_Handler** — copies `.data`, zeroes `.bss`, calls `kmain()`

VTOR addresses: `0x10001000` (pico1), `0x10004000` (pico1calc, 16 KB reserved by UF2 bootloader).

### m68k Boot Details

The kernel is loaded by QEMU directly into RAM. The startup code sets up the vector table and jumps to `kmain()`.

## Memory Layout

Memory layout varies per target. Each target defines its own linker script and memory regions.

### ARM (RP2040) — SRAM (264 KB)

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 20 KB | BSS, MSP stack, globals, `.ramfunc` |
| Page pool | `0x20005000` | 204 KB | 51 pages x 4 KB, free-stack allocator |
| I/O buffer | `0x20038000` | 24 KB | SD / filesystem cache |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |

Code and read-only data execute directly via XIP from flash — no SRAM copy needed.
The romfs image follows the kernel in flash and is accessed via memory-mapped reads.

### m68k (QEMU virt) — RAM (runtime-detected, up to 16 MB)

Kernel, data, and romfs are all in RAM. The page pool starts after the kernel
BSS and stack (linker symbol `__page_pool_start`). Available RAM is detected
at boot by `m68k_probe_ram()` — see Page Allocator below.

## Page Allocator

Free-stack design: O(1) alloc and free using a simple stack of page addresses.
No linked-list traversal.

```
page_alloc()  → pop from free stack
page_free()   → push back to free stack (with double-free detection)
```

The static free-stack array is sized by `PAGE_COUNT_MAX` (compile-time maximum),
while the runtime `page_count` variable holds the actual number of usable pages:

- **ARM (RP2040):** `page_count = PAGE_COUNT_MAX = 51` (fixed 204 KB SRAM pool).
- **m68k:** `page_count` is set by `m68k_probe_ram()`, which detects installed
  RAM at boot using a two-phase write-pattern-verify probe (1 MB coarse steps,
  then 4 KB fine steps). The probe ceiling is target-configurable via `RAM_END`
  (e.g., X68000 sets `RAM_END=0xC00000` to exclude VRAM). Probed values are
  saved and restored to avoid corrupting existing memory contents (e.g., the
  vector table at address 0).

On dual-core targets (RP2040), the allocator is protected by a hardware spinlock (`SPIN_PAGE`).

## Process Model

### PCB (`src/kernel/proc/proc.c`)

States: `PROC_FREE` → `PROC_RUNNABLE` → `PROC_SLEEPING` / `PROC_ZOMBIE`

Each process has:
- Kernel stack (separate from user stack)
- User stack page(s)
- File descriptor table (16 entries)
- Signal state (pending mask, handlers)
- Process group and session IDs

### vfork + exec

No MMU means no Copy-on-Write fork. Instead:

- `vfork()` — child runs in parent's address space; parent is blocked
- Child immediately calls `execve()`, which allocates fresh pages
- Parent resumes after child calls exec or _exit

### ELF Loader (`src/kernel/exec/exec.c`)

Loads PIE (Position-Independent Executable) ELF binaries:

1. Validate ELF header (architecture-specific: ARM or m68k, ET_DYN for PIE)
2. Map PT_LOAD segments — on ARM, `.text` stays in flash (XIP); on m68k, `.text` is in RAM
3. Process relocations (architecture-specific: `R_ARM_RELATIVE` on ARM, `R_68K_RELATIVE` on m68k)
4. Set PIC base register (r9 on ARM, a5 on m68k) to GOT/data base
5. Set user stack pointer, return to user mode

On ARM targets, XIP allows code to run directly from flash with zero SRAM footprint for `.text`.

## Scheduler (`src/kernel/proc/sched.c`)

Preemptive round-robin:

- Timer interrupt fires every 10 ms (100 Hz tick rate, `PPAP_TICK_HZ`)
- Timer handler pends a context switch (lowest priority on ARM via PendSV; direct switch on m68k)
- Context switch saves/restores callee-saved registers to/from the current PCB

### Context Switch — Per-Architecture

**ARM (Thumb):** SysTick fires every 10 ms (reload = SystemCoreClock/100 - 1). SysTick handler pends PendSV (lowest priority) for lazy context switch. `PendSV_Handler` (`src/kernel/proc/switch.S`) saves r4-r11 + LR to current PCB, loads next.

**m68k:** A periodic timer interrupt directly calls the context switch routine, which saves/restores d2-d7/a2-a6 and the stack pointer.

### Dual-Core (RP2040 only)

Both RP2040 cores run user processes:

- `current_core[2]` array indexed by `SIO_CPUID` register
- 4 hardware spinlocks: `SPIN_PAGE`, `SPIN_PROC`, `SPIN_VFS`, `SPIN_FS`
- Spinlock discipline: disable IRQs before acquire, restore after release
- Each core has independent SysTick and PendSV

### Tick Accounting

User vs system time is tracked per-core. On ARM, `EXC_RETURN` bit 3 distinguishes user mode (thread mode with PSP) from handler mode. On m68k, the supervisor bit in the status register serves a similar purpose.

## Memory Protection

Memory protection is target-dependent and optional.

### ARM (RP2040) — MPU

4-region layout (RP2040 limit):

| Region | Purpose | Access |
|---|---|---|
| 0 | Kernel data (SRAM) | Privileged RW only |
| 1 | Flash (XIP) | All: RO + Execute |
| 2 | Current process stack/heap | User + Privileged RW |
| 3 | Peripherals + I/O buffers | Privileged RW only |

Region 2 is reprogrammed on every context switch to point to the current
process's pages. `PRIVDEFENA` bit is set so the kernel can access all
memory while user mode is restricted.

### m68k

The m68k QEMU target does not currently have memory protection. All code runs in supervisor mode.

## System Call Interface

System calls use a unified numbering scheme across all architectures (see [syscall.md](syscall.md)). The trap mechanism is architecture-specific:

**ARM:** `svc 0` instruction. Arguments in r0–r5, syscall number in r7, return value in r0.

**m68k:** `trap #0` instruction. Syscall number in d0, arguments in d1–d5/a0, return value in d0.

The SVC/TRAP handler (`src/kernel/syscall/syscall.c`) dispatches via a shared function pointer table.

## TTY Subsystem (`src/kernel/fd/tty.c`)

Multi-TTY with pluggable backends:

- `/dev/ttyS0` — UART serial console (IRQ-driven ring buffers on ARM; polled on m68k QEMU)
- `/dev/tty1` — LCD framebuffer + I2C keyboard (PicoCalc only)

Each TTY has:
- Line discipline (canonical mode with echo, backspace, Ctrl-C/D)
- Backend ops: `putc`, `getc`, `poll` (decouples TTY from device)
- Foreground process group for job control signals
- `getty` spawns on each TTY for login

## Signal Delivery

POSIX-style sigaction/sigprocmask:

- Signals are delivered on return to user-space (checked in syscall exit path)
- Handler runs on the user stack with a sigreturn trampoline
- `SIGKILL`/`SIGSTOP` cannot be caught
- `SIGCHLD` delivered to parent on child exit (enables waitpid wakeup)

The trampoline and signal frame layout are architecture-specific but the kernel-side logic is shared.

## Related Documentation

- [syscall.md](syscall.md) — System call reference
- [procfs.md](procfs.md) — /proc filesystem
- [filesystems.md](filesystems.md) — VFS and filesystem drivers
- [userland-dev-guide.md](userland-dev-guide.md) — User-space development
- [PicoCalc.md](reference/PicoCalc.md) — PicoCalc hardware reference
- [PicoCalc-LCD.md](reference/PicoCalc-LCD.md) — LCD display driver
- [target-68000.md](target-68000.md) — m68k target notes

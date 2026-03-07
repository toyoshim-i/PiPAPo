# PPAP Kernel Architecture

Internal design reference for kernel developers.

---

## Boot Sequence

1. **ROM Boot** — RP2040 boot ROM loads 256 B from QSPI flash
2. **Stage 1** (`src/boot/stage1.S`) — sets VTOR to kernel vector table, jumps to Reset_Handler
3. **Reset_Handler** (`src/boot/startup.S`) — copies `.data`, zeroes `.bss`, calls `kmain()`
4. **kmain** (`src/kernel/main.c`) — unified entry using target hooks:
   - `target_early_init()` — UART, clock (PLL to 133 MHz)
   - Page allocator init (51 free pages on RP2040)
   - MPU init (4-region layout)
   - romfs mount as root (`/`)
   - `target_init()` — SD card, display, keyboard (target-specific)
   - Mount remaining filesystems (VFAT, UFS via fstab)
   - Core 1 launch via SIO FIFO handshake
   - `execve("/sbin/init")` as PID 1

VTOR addresses: `0x10001000` (pico1), `0x10004000` (pico1calc, 16 KB reserved by UF2 bootloader).

## Memory Layout

### SRAM (264 KB)

| Region | Address | Size | Purpose |
|---|---|---|---|
| Kernel data | `0x20000000` | 20 KB | BSS, MSP stack, globals, `.ramfunc` |
| Page pool | `0x20005000` | 204 KB | 51 pages x 4 KB, free-stack allocator |
| I/O buffer | `0x20038000` | 24 KB | SD / filesystem cache |
| DMA / Reserved | `0x2003E000` | 16 KB | DMA, PIO, Core 1 stack |

### Flash

Code and read-only data execute directly via XIP — no SRAM copy needed.
The romfs image follows the kernel in flash and is accessed via memory-mapped reads.

## Page Allocator

Free-stack design: O(1) alloc and free using a simple stack of page addresses.
No linked-list traversal. 51 pages available (204 KB) on RP2040.

```
page_alloc()  → pop from free stack, zero page
page_free()   → push back to free stack (with double-free detection)
```

Protected by `SPIN_PAGE` hardware spinlock on dual-core builds.

## Process Model

### PCB (`src/kernel/proc/proc.c`)

States: `PROC_FREE` → `PROC_RUNNABLE` → `PROC_SLEEPING` / `PROC_ZOMBIE`

Each process has:
- Kernel stack (separate from user PSP)
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

1. Validate ELF header (ARM, ET_DYN for PIE)
2. Map PT_LOAD segments — `.text` stays in flash (XIP), `.data`/`.bss` go to SRAM pages
3. Process R_ARM_RELATIVE relocations (patch GOT entries)
4. Set r9 = GOT base (PIC register for Cortex-M0+)
5. Set PSP to user stack, return to user mode via crafted exception frame

XIP key insight: `xip_addr` on the vnode gives the flash address for `.text`,
so code runs directly from flash with zero SRAM footprint.

## Scheduler (`src/kernel/proc/sched.c`)

Preemptive round-robin:

- **SysTick** fires every 10 ms (reload = 133000000/100 - 1 = 1329999)
- SysTick handler pends **PendSV** (lowest priority) for lazy context switch
- PendSV handler (`src/kernel/proc/switch.S`): saves r4-r11 + LR to current PCB, loads next

### Dual-Core

Both RP2040 cores run user processes:

- `current_core[2]` array indexed by `SIO_CPUID` register
- 4 hardware spinlocks: `SPIN_PAGE`, `SPIN_PROC`, `SPIN_VFS`, `SPIN_FS`
- Spinlock discipline: disable IRQs (`cpsid i`) before acquire, restore after release
- Each core has independent SysTick and PendSV

### Tick Accounting

Cortex-M0+ lacks RETTOBASE. Instead, `EXC_RETURN` bit 3 distinguishes
user mode (1 = thread mode with PSP) from handler mode, for accurate
user/system time tracking.

## MPU Protection

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

## System Call Interface

ARM EABI convention via SVC instruction:

```
r7 = syscall number
r0–r3 = arguments
r0 = return value (negative = -errno)
```

The SVC handler (`src/kernel/syscall/syscall.c`) dispatches via a function
pointer table. See [syscall.md](syscall.md) for the complete reference.

## TTY Subsystem (`src/kernel/fd/tty.c`)

Multi-TTY with pluggable backends:

- `/dev/ttyS0` — UART serial console (IRQ-driven ring buffers)
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

## Related Documentation

- [syscall.md](syscall.md) — System call reference
- [procfs.md](procfs.md) — /proc filesystem
- [filesystems.md](filesystems.md) — VFS and filesystem drivers
- [userland-dev-guide.md](userland-dev-guide.md) — User-space development
- [PicoCalc.md](PicoCalc.md) — PicoCalc hardware reference
- [PicoCalc-LCD.md](PicoCalc-LCD.md) — LCD display driver

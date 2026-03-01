# Phase 1: Kernel Foundation — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 4 weeks

---

## Goals

Build the core kernel infrastructure that all subsequent phases depend on:
a preemptive scheduler, a page allocator, a system call interface, MPU
protection, and Core 1 startup.

**Exit Criteria (all must pass before moving to Phase 2):**
- Page pool initialised: 52 × 4 KB pages on the free list, `page_alloc` / `page_free` work
- Two kernel threads run concurrently, switched by PendSV every 10 ms (SysTick)
- `SVC` instruction dispatches to the correct handler; `_exit`, `getpid`, `write` (→ UART), `nanosleep` implemented
- MPU 4-region layout active; privileged access to kernel data verified
- Core 1 started and responds to SIO FIFO messages from Core 0
- QEMU smoke test runs scheduler and syscalls without hardware (Core 1 and MPU stubbed)

---

## Source Tree After Phase 1

```
src/
  boot/
    startup.S           (existing — add PendSV / SysTick vectors)
  kernel/
    main.c              (existing — call mm_init, proc_init, sched_start)
    main_qemu.c         (existing — same init path, Core 1 stubbed)
    mm/
      page.c / page.h   # Page allocator — free-stack, 52 × 4 KB pages
      kmem.c / kmem.h   # Kernel object pool — fixed-size slab for PCBs
      mpu.c  / mpu.h    # MPU 4-region configuration + per-context switch
    proc/
      proc.h            # PCB definition (pid, state, regs, fd table, …)
      proc.c            # process table, proc_alloc, proc_free
      switch.S          # PendSV handler — save/restore Cortex-M0+ context
      sched.c / sched.h # Round-robin scheduler, tick handler, yield
    syscall/
      syscall.c         # SVC handler, dispatch table
      sys_proc.c        # _exit, getpid, vfork (stub), waitpid (stub)
      sys_io.c          # write (→ UART for now), read (stub)
      sys_time.c        # nanosleep, time
    smp.c / smp.h       # Core 1 launch + SIO FIFO IPC helpers
    xip_test.c/h        (existing)
  drivers/
    uart.c/h            (existing)
    clock.c/h           (existing)
```

---

## Week 1: Page Allocator and Kernel Object Pool

### Step 1 — Page Allocator (`src/kernel/mm/page.c`)

The page pool is the foundation for all dynamic memory in the OS.
Fixed 4 KB pages map directly onto the SRAM layout from the design spec.

**SRAM layout (Phase 0 linker script — already correct):**

| Region | Address | Size | Pages |
|---|---|---|---|
| Kernel data | `0x20000000` | 16 KB | — |
| **Page pool** | `0x20004000` | 208 KB | 52 × 4 KB |
| I/O buffer | `0x20038000` | 24 KB | — |
| DMA/Reserved | `0x2003E000` | 16 KB | — |

**Design choices:**

- **Free stack** (not linked list): page frames are fixed-size; a stack is
  O(1) push/pop with no per-page overhead.  A 52-entry array of `uint8_t *`
  costs only 208 bytes in kernel data.
- No reference counts in Phase 1 (single-owner pages); add in Phase 3 for
  vfork copy-on-write.

**API:**

```c
/* src/kernel/mm/page.h */
#define PAGE_SIZE   4096u
#define PAGE_COUNT  52u

void     mm_init(void);                /* build free stack from PAGE_POOL_BASE */
void    *page_alloc(void);             /* pop from free stack; returns NULL if OOM */
void     page_free(void *page);        /* push back onto free stack */
uint32_t page_free_count(void);        /* for diagnostics / OOM decisions */
```

**Verification (UART output at boot):**
```
MM: page pool 0x20004000–0x20037fff (52 pages × 4096 B = 208 KB)
MM: page_alloc → 0x20004000
MM: page_free  ← 0x20004000
MM: free pages: 52
```

### Step 2 — Kernel Object Pool (`src/kernel/mm/kmem.c`)

The kernel needs small fixed-size allocations for PCBs (≈256 B each), fd
tables, and similar objects.  A general `malloc` would waste space on
alignment and headers.  Instead, a minimal slab allocator provides per-type
free lists carved from a single 4 KB kernel heap page.

**API:**

```c
/* src/kernel/mm/kmem.h */
typedef struct kmem_pool kmem_pool_t;

kmem_pool_t *kmem_pool_create(size_t obj_size, size_t capacity);
void        *kmem_alloc(kmem_pool_t *pool);   /* O(1) */
void         kmem_free(kmem_pool_t *pool, void *obj);
```

Phase 1 uses a single statically-declared PCB pool (8 objects × 256 B = 2 KB),
avoiding dynamic pool allocation.

---

## Week 2: PCB, Context Switch, and Preemption

### Step 3 — PCB Definition and Process Table (`src/kernel/proc/`)

**PCB layout (≈256 bytes):**

```c
/* src/kernel/proc/proc.h */
#define PROC_MAX      8
#define FD_MAX        16

typedef enum { PROC_FREE, PROC_RUNNABLE, PROC_SLEEPING, PROC_ZOMBIE } proc_state_t;

typedef struct {
    /* Saved CPU context — layout must match switch.S offsets */
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11;  /* callee-saved */
    uint32_t sp;                                   /* saved stack pointer */
    /* (r0–r3, r12, lr, pc, xpsr saved on stack by hardware on exception entry) */

    /* Identity */
    pid_t    pid;
    pid_t    ppid;
    proc_state_t state;

    /* Memory */
    void    *stack_page;          /* 4 KB page — process kernel stack */
    void    *user_pages[4];       /* up to 4 user data pages (Phase 3+) */

    /* Files */
    void    *fd_table[FD_MAX];    /* Phase 3+: replace void* with struct file* */
    char     cwd[64];

    /* Scheduling */
    uint32_t ticks_remaining;
    uint32_t sleep_until;         /* for nanosleep */
} pcb_t;

extern pcb_t  proc_table[PROC_MAX];
extern pcb_t *current;            /* pointer to the running PCB */

pcb_t *proc_alloc(void);
void   proc_free(pcb_t *p);
```

**Gotcha:** `current` must never be NULL; `proc_table[0]` is reserved for
the initial kernel thread and is pre-initialised before `sched_start()`.

### Step 4 — Context Switch (`src/kernel/proc/switch.S`)

The Cortex-M0+ hardware automatically saves {r0–r3, r12, lr, pc, xpsr} on
the exception stack frame when entering any exception (including PendSV).
The software context switch only needs to save/restore the callee-saved
registers {r4–r11} and SP.

```asm
/* src/kernel/proc/switch.S */
.thumb_func
.global PendSV_Handler
PendSV_Handler:
    /* --- Save outgoing context --- */
    mrs   r0, psp               /* r0 = outgoing process stack pointer */
    subs  r0, r0, #32           /* make room for r4-r11 */
    stmia r0!, {r4-r7}          /* save low callee-saved regs */
    mov   r4, r8
    mov   r5, r9
    mov   r6, r10
    mov   r7, r11
    stmia r0!, {r4-r7}          /* save high callee-saved regs */
    subs  r0, r0, #32
    ldr   r1, =current
    ldr   r2, [r1]              /* r2 = current PCB pointer */
    str   r0, [r2, #SP_OFFSET]  /* save PSP into PCB.sp */

    /* --- Pick next process (call C scheduler) --- */
    bl    sched_next            /* returns next PCB pointer in r0 */
    str   r0, [r1]              /* current = next */

    /* --- Restore incoming context --- */
    ldr   r0, [r0, #SP_OFFSET]  /* r0 = next process's saved PSP */
    ldmia r0!, {r4-r7}
    ldmia r0!, {r1-r3, r12}     /* r1-r3 = r8-r10, r12 = r11 */
    mov   r8,  r1
    mov   r9,  r2
    mov   r10, r3
    mov   r11, r12
    msr   psp, r0
    bx    lr                    /* return from exception with EXC_RETURN */
```

**Important Cortex-M0+ constraints:**
- No `stmdb` — use `subs` + `stmia` pattern (ARMv6-M only has limited STMIA).
- `r8–r11` cannot be used directly with `stmia`; move to `r4–r7` first.
- PendSV must be the **lowest priority** exception (set `SHPR3` bits [23:16] = 0xFF)
  so it never preempts a real interrupt handler.

### Step 5 — SysTick Preemption

SysTick generates the 10 ms time-slice interrupt and triggers PendSV for
context switching.

```c
/* in sched.c — called from sched_start() */
#define SYSTICK_RELOAD  (133000000u / 100u - 1u)   /* 10 ms at 133 MHz */

static void systick_init(void) {
    SYST_RVR = SYSTICK_RELOAD;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_CLKSOURCE_CPU | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}
```

```c
/* SysTick_Handler — decrements ticks; triggers PendSV when slice expires */
void SysTick_Handler(void) {
    if (current && --current->ticks_remaining == 0) {
        current->ticks_remaining = SCHED_TICKS;
        /* Trigger PendSV (lazy context switch at lowest priority) */
        SCB_ICSR |= SCB_ICSR_PENDSVSET;
    }
    /* also: wake sleeping processes */
    sched_tick();
}
```

**Vectors to add to `startup.S`:**

| Position | Handler |
|---|---|
| 14 (offset 0x38) | `PendSV_Handler` |
| 15 (offset 0x3C) | `SysTick_Handler` |

---

## Week 3: Scheduler and System Call Interface

### Step 6 — Round-Robin Scheduler (`src/kernel/proc/sched.c`)

**API:**

```c
void  sched_start(void);           /* enable SysTick, switch to first process */
pcb_t *sched_next(void);           /* called from PendSV — pick next RUNNABLE */
void  sched_sleep(uint32_t ticks); /* put current process to sleep */
void  sched_wake(pcb_t *p);        /* wake a sleeping process */
void  sched_tick(void);            /* called from SysTick — advance sleep timers */
```

`sched_next()` scans `proc_table` in round-robin order starting after
`current`.  Returns `current` if no other runnable process exists (idle spin).

**Idle strategy:** No separate idle thread in Phase 1.  If `sched_next()`
finds no other runnable process it returns `current` unchanged — the
interrupted process continues running.  A proper idle thread (WFI loop on
Core 0) will be added in Phase 2.

### Step 7 — SVC Handler and Syscall Dispatch (`src/kernel/syscall/`)

ARM EABI Linux convention (compatible with musl libc):
- `r7` = syscall number
- `r0`–`r3` = arguments (up to 4)
- `r0` = return value (negative errno on error)
- `svc 0` instruction triggers the SVC exception

```c
/* src/kernel/syscall/syscall.c */
typedef long (*syscall_fn_t)(long, long, long, long);

static const syscall_fn_t syscall_table[] = {
    [SYS_exit]      = sys_exit,
    [SYS_read]      = sys_read,
    [SYS_write]     = sys_write,
    [SYS_getpid]    = sys_getpid,
    [SYS_nanosleep] = sys_nanosleep,
    /* … */
};

/* SVC_Handler — called from startup.S exception vector */
void SVC_Handler(void) {
    /* Recover stacked frame: r0–r3 are at (sp+0..12), r7 at ... */
    /* Extract syscall number from r7; dispatch */
}
```

**Stacked frame recovery:** The hardware exception frame (pushed to the
process stack by the CPU) layout is:
`[r0, r1, r2, r3, r12, lr, pc, xpsr]` at offsets `[0, 4, 8, 12, 16, 20, 24, 28]`
from PSP at exception entry.  The SVC handler reads PSP and indexes this
frame to recover `r0–r3` (arguments) and `r7` (syscall number, saved by the
compiler as a callee-saved register — already on the process stack before
SVC).

**Gotcha:** `r7` is a callee-saved register; the compiler saves it to the
process stack before issuing `svc 0`.  It is **not** part of the hardware
exception frame.  To retrieve it: read PSP, then find `r7` at the standard
offset below the exception frame.

### Step 8 — Minimal Syscall Implementations

Phase 1 syscalls — enough to run a trivial two-process test:

| Syscall | Number | Implementation |
|---|---|---|
| `_exit(status)` | 1 | Mark PCB as ZOMBIE; call `sched_next()` |
| `write(fd, buf, n)` | 4 | fd 1/2: write `n` bytes to UART; other fds: `-EBADF` |
| `read(fd, buf, n)` | 3 | fd 0: stub returning 0 (EOF) |
| `getpid()` | 20 | Return `current->pid` |
| `nanosleep(req, rem)` | 162 | Convert `tv_nsec/tv_sec` to SysTick ticks; `sched_sleep()` |

`write` to UART is the key integration test: a user-mode process calling
`write(1, "hello\n", 6)` that produces UART output proves SVC dispatch, stack
frame recovery, and fd routing all work.

---

## Week 4: MPU Setup and Core 1

### Step 9 — MPU 4-Region Layout (`src/kernel/mm/mpu.c`)

The Cortex-M0+ MPU has 8 regions (on RP2040, only 8 are wired).  We use 4:

| Region | Base | Size | Attributes | Purpose |
|---|---|---|---|---|
| 0 | `0x20000000` | 16 KB | RW, privileged only | Kernel data — fault on user access |
| 1 | `0x10000000` | 16 MB | RO+X, all modes | Entire flash (XIP) — no write |
| 2 | *per-process* | 8 KB | RW, user+priv | Current process stack (2 pages) |
| 3 | `0x40000000` | 512 MB | RW, privileged | Peripherals and I/O |

Region 2 is reprogrammed on every context switch to point to the new
process's stack pages.

```c
void mpu_init(void);                    /* configure regions 0, 1, 3; enable MPU */
void mpu_switch(pcb_t *next);           /* update region 2 for next process */
```

**MPU register layout (Cortex-M0+ ARM ref §B3.5):**
```
MPU_TYPE   @ 0xE000ED90
MPU_CTRL   @ 0xE000ED94   bit0=ENABLE, bit1=HFNMIENA, bit2=PRIVDEFENA
MPU_RNR    @ 0xE000ED98   region select
MPU_RBAR   @ 0xE000ED9C   base address (must be size-aligned)
MPU_RASR   @ 0xE000EDA0   [1]=ENABLE [5:1]=SIZE [26:24]=AP [28]=XN
```

**Size encoding:** `SIZE` field = log₂(size) − 1.  16 KB → SIZE=13,
1 MB → SIZE=19, 512 MB → SIZE=28.

**Gotcha:** `MPU_CTRL.PRIVDEFENA` (bit 2) enables the background default map
for privileged code.  Set this so the kernel can still access all memory
normally while the MPU only restricts user-mode accesses.

**QEMU note:** QEMU's `mps2-an500` does not emulate the RP2040 MPU.
`mpu_init()` reads `MPU_TYPE`; if it returns 0 (no MPU), skip configuration.
This allows `main_qemu.c` to call `mpu_init()` without a separate `#ifdef`.

### Step 10 — Core 1 Startup (`src/kernel/smp.c`)

The RP2040 boot ROM requires a specific handshake sequence to start Core 1:
send `[0, 0, 1, VTOR, SP, entry]` over the SIO FIFO.

```c
/* src/kernel/smp.c */
void core1_launch(void (*entry)(void));   /* send boot sequence; Core 1 runs entry() */
void sio_fifo_push(uint32_t value);       /* blocking push to Core 1 */
uint32_t sio_fifo_pop(void);              /* blocking pop from Core 1 */
```

**Boot sequence (RP2040 Datasheet §2.8.2):**
1. Drain the FIFO (`SIO_FIFO_RD` until `SIO_FIFO_ST.VLD` = 0)
2. Send the 6-word sequence: `0, 0, 1, VTOR_value, stack_top, entry_addr`
3. Wait for Core 1 to echo back the entry address

Core 1 entry for Phase 1: a simple I/O worker loop that pops a command word
from the FIFO and echoes it back (placeholder for Phase 4 SD I/O).

```c
static void core1_io_worker(void) {
    for (;;) {
        uint32_t cmd = sio_fifo_pop();
        /* Phase 4: dispatch to SD / block device handlers */
        sio_fifo_push(cmd);   /* echo for now */
    }
}
```

**QEMU:** The SIO registers do not exist on `mps2-an500`.  `core1_launch()`
checks `SIO_FIFO_ST` for a sentinel value; on QEMU, skip the launch entirely.
`main_qemu.c` can call `core1_launch(core1_io_worker)` without `#ifdef`.

---

## Deliverables

| File | Description |
|---|---|
| `src/kernel/mm/page.c/h` | Free-stack page allocator (52 × 4 KB) |
| `src/kernel/mm/kmem.c/h` | Fixed-size kernel object pool (PCB slab) |
| `src/kernel/mm/mpu.c/h` | MPU 4-region setup + per-context switch update |
| `src/kernel/proc/proc.h` | PCB definition, process table, states |
| `src/kernel/proc/proc.c` | `proc_alloc`, `proc_free`, `proc_init` |
| `src/kernel/proc/switch.S` | PendSV handler — Cortex-M0+ context save/restore |
| `src/kernel/proc/sched.c/h` | Round-robin scheduler, SysTick tick handler |
| `src/kernel/syscall/syscall.c` | SVC exception handler, dispatch table |
| `src/kernel/syscall/sys_proc.c` | `_exit`, `getpid` |
| `src/kernel/syscall/sys_io.c` | `write` (UART), `read` (stub) |
| `src/kernel/syscall/sys_time.c` | `nanosleep`, `time` |
| `src/kernel/smp.c/h` | Core 1 boot handshake + SIO FIFO helpers |
| `src/boot/startup.S` | Add PendSV and SysTick vectors |
| `src/kernel/main.c` | Call `mm_init`, `proc_init`, `mpu_init`, `core1_launch`, `sched_start` |
| `src/kernel/main_qemu.c` | Same call sequence; MPU/Core 1 self-stub on QEMU |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| PendSV context switch bugs (corrupted registers) | High | Test with exactly 2 hardcoded threads printing alternating output; single-step in GDB |
| SVC frame recovery differs between compiler versions | Medium | Inspect generated assembly; check `r7` offset explicitly with `arm-none-eabi-objdump` |
| SysTick reload value wrong for 133 MHz | Low | Verify with SysTick benchmark from Step 9 (Phase 0) |
| MPU fault during kernel init (before regions configured) | Medium | Enable MPU only after all regions programmed; set `PRIVDEFENA` |
| Core 1 boot handshake timeout | Medium | Use the exact 6-word sequence from the RP2040 datasheet §2.8.2; drain FIFO first |
| QEMU not emulating SIO / MPU | Certain | Read sentinel registers; skip gracefully without `#ifdef` |
| Stack overflow in kernel threads (only 4 KB per process) | Low | Place a guard pattern (`0xDEADBEEF`) at stack bottom; check in SysTick handler |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — §2.8.2 (SIO/Core 1 launch), §2.4 (SysTick), §2.26 (MPU)
- [ARMv6-M Architecture Reference Manual](https://developer.arm.com/documentation/ddi0419/) — §B1.5 (exception model), §B3.5 (MPU), §B3.2 (SysTick)
- [PicoPiAndPortable Design Spec v0.2](PicoPiAndPortable-spec-v02.md) — §4 (Kernel Design), §2.3 (SRAM map)

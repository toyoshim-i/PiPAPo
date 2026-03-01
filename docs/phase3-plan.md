# Phase 3: Process Execution — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 4 weeks

---

## Goals

Build the user-space process execution pipeline: a user application build
workflow, an ELF loader, `execve`/`vfork`/`waitpid` syscalls, pipes, file
descriptor duplication, heap management, and basic signal infrastructure.
After this phase the kernel can load and execute ELF binaries from romfs
(directly via XIP for `.text`, SRAM for `.data`/stack/heap), and multiple
processes can communicate through pipes.

**Exit Criteria (all must pass before moving to Phase 4):**
- User-space ELF build workflow produces a statically linked Thumb PIC binary using raw SVC syscalls (no libc)
- `hello` binary integrated into romfs, loaded by ELF loader, and executed from flash via XIP
- `execve("/bin/hello", ...)` transitions to unprivileged Thread mode and prints "hello world" via `sys_write`
- `vfork()` blocks parent; child calls `execve()`; parent resumes after child exits
- `waitpid()` reaps zombie children and returns exit status
- `pipe()` creates an SRAM ring-buffer pipe; data written by one process is readable by another
- `dup2()` redirects file descriptors (required for shell pipeline setup)
- `brk`/`sbrk` extends the process heap; a simple `malloc`-free test works
- `sigaction` installs a handler; `kill` delivers a signal; handler runs and returns
- QEMU smoke test: vfork + exec + pipe integration runs without hardware

---

## Source Tree After Phase 3

```
src/
  boot/
    startup.S             (existing)
  kernel/
    main.c                (existing — add user-process launch after VFS mounts)
    main_qemu.c           (existing — same)
    mm/
      page.c/h            (existing)
      kmem.c/h            (existing)
      mpu.c/h             (existing — update mpu_switch for user code region)
      xip.c/h             (existing)
    proc/
      proc.h              (existing — add signal fields, brk pointer)
      proc.c              (existing — extend proc_alloc/free for exec/vfork)
      sched.c/h           (existing)
      switch.S            (existing)
    vfs/
      vfs.c/h             (existing — Phase 2 VFS layer)
      namei.c             (existing — Phase 2 path resolution)
    fs/
      romfs.c/h           (existing — Phase 2 romfs driver)
      romfs_format.h      (existing)
      devfs.c/h           (existing — Phase 2 devfs)
      procfs.c/h          (existing — Phase 2 procfs)
    exec/
      elf.c / elf.h       # ELF32 parser: validate, locate segments
      exec.c / exec.h     # execve() implementation: load ELF, setup user mode
    syscall/
      syscall.c           (existing — extend dispatch table)
      syscall.h           (existing — add new syscall numbers)
      svc.S               (existing)
      sys_proc.c          (existing — add vfork, waitpid, execve, kill)
      sys_io.c            (existing — add pipe, dup, dup2)
      sys_fs.c            (existing — Phase 2 open/close/stat/…)
      sys_time.c          (existing)
      sys_mem.c           # New: brk, sbrk
    fd/
      file.h              (existing — add pipe_ops)
      fd.c/h              (existing — add fd_dup, fd_dup2)
      tty.c/h             (existing)
      pipe.c / pipe.h     # Pipe implementation: ring-buffer backed file_ops
    signal/
      signal.c / signal.h # Signal infrastructure: delivery, handler dispatch
    smp.c/h               (existing)
  drivers/
    uart.c/h              (existing)
    uart_qemu.c           (existing)
    clock.c/h             (existing)
user/
  Makefile                # User-space build: arm-none-eabi-gcc -fpic -mthumb
  crt0.S                  # Minimal C runtime: set up stack, call main, call _exit
  syscall.h               # SVC wrappers: write(), exit(), getpid(), …
  syscall.S               # SVC instruction stubs (ARM EABI: r7=nr, svc 0)
  utest.h                 # On-target user-space test framework (ASSERT via sys_write)
  hello.c                 # Hello world — first user-space program
  test_exec.c             # exec + XIP execution test
  test_vfork.c            # vfork + exec + waitpid test
  test_pipe.c             # Pipe + dup2 test
  test_signal.c           # Signal handler test
  test_brk.c              # brk/sbrk heap test
  test_fd.c               # dup/dup2/close/pipe-eof test
  runtests.c              # Test runner: vfork+exec each /bin/test_*, collect results
tests/
  (existing — host-native unit tests)
  test_elf.c              # New: host-native ELF parser unit tests
  test_pipe_unit.c        # New: host-native pipe ring-buffer logic tests
  test_signal_unit.c      # New: host-native signal pending/mask logic tests
tools/
  mkromfs/                (existing — Phase 2)
romfs/
  etc/                    (existing — Phase 2)
  bin/
    hello                 # ELF binary built from user/hello.c
    runtests              # ELF binary — test runner
    test_exec             # ELF binary — exec test
    test_vfork            # ELF binary — vfork test
    test_pipe             # ELF binary — pipe test
    test_signal           # ELF binary — signal test
    test_brk              # ELF binary — brk test
    test_fd               # ELF binary — fd duplication test
```

---

## Week 1: User-Space Build Workflow and ELF Loader

### Step 1 — User Application Build Workflow (`user/`)

Before the ELF loader can be tested, we need user-space binaries to load.
Since musl libc porting is Phase 5, Phase 3 uses raw SVC syscall wrappers
with a minimal C runtime — no libc dependency.

**Compiler flags:**

```sh
CFLAGS = -mthumb -mcpu=cortex-m0plus -march=armv6s-m \
         -ffreestanding -nostdlib -fpic \
         -Os -g -Wall -Werror
LDFLAGS = -nostdlib -T user.ld
```

**Why `-fpic`:** The design spec (§4.5) requires all user programs to be
position-independent.  For romfs XIP binaries, the ELF is placed at a
known flash address at build time, so PIC relocations resolve to the flash
address.  For future SD-loaded binaries (Phase 4), PIC allows loading at
any SRAM address with runtime relocation.

**Minimal C runtime (`user/crt0.S`):**

```asm
.syntax unified
.thumb

.section .text.crt0, "ax"
.global _start
.thumb_func
_start:
    /* argc/argv not supported in Phase 3 — pass 0, NULL */
    movs r0, #0         /* argc = 0 */
    movs r1, #0         /* argv = NULL */
    bl   main
    /* main returned in r0 — call _exit(r0) */
    movs r7, #1         /* SYS_exit = 1 */
    svc  0
    b    .              /* should not reach here */
```

**SVC syscall wrappers (`user/syscall.S`):**

```asm
.syntax unified
.thumb

.macro SYSCALL name, nr
.global \name
.thumb_func
\name:
    push {r7, lr}       /* save r7 (callee-saved) and lr */
    movs r7, #\nr
    svc  0
    pop  {r7, pc}
.endm

SYSCALL sys_exit,      1
SYSCALL sys_read,      3
SYSCALL sys_write,     4
SYSCALL sys_getpid,   20
SYSCALL sys_vfork,   190
SYSCALL sys_execve,   11
SYSCALL sys_waitpid,   7
SYSCALL sys_pipe,     42
SYSCALL sys_dup2,     63
SYSCALL sys_kill,     37
SYSCALL sys_brk,      45
```

**Header (`user/syscall.h`):**

```c
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t ssize_t;
typedef int32_t pid_t;

void    sys_exit(int status)   __attribute__((noreturn));
ssize_t sys_read(int fd, void *buf, size_t n);
ssize_t sys_write(int fd, const void *buf, size_t n);
pid_t   sys_getpid(void);
pid_t   sys_vfork(void);
int     sys_execve(const char *path, char *const argv[], char *const envp[]);
pid_t   sys_waitpid(pid_t pid, int *status, int opts);
int     sys_pipe(int fds[2]);
int     sys_dup2(int oldfd, int newfd);
int     sys_kill(pid_t pid, int sig);
void   *sys_brk(void *addr);

#endif
```

**First test program (`user/hello.c`):**

```c
#include "syscall.h"

int main(void) {
    const char msg[] = "Hello from user space!\n";
    sys_write(1, msg, sizeof(msg) - 1);
    return 0;
}
```

**User linker script (`user/user.ld`):**

The user binary is linked at address 0 (position-independent).  The ELF
loader or romfs placement determines the final execution address.

```ld
ENTRY(_start)

SECTIONS {
    . = 0;
    .text : { *(.text.crt0) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    _end = .;
}
```

**Build integration (CMakeLists.txt):**

A custom CMake command invokes `make -C user/` to produce ELF binaries.
The binaries are then placed into `romfs/bin/` before `mkromfs` runs.
The build order is: user binaries → mkromfs → link kernel + romfs.

**QEMU note:** The same user-space binaries work on QEMU — they use SVC
for I/O, not hardware-specific code.  On QEMU the romfs lives in ROM
at a different base address, but XIP execution works identically.

### Step 2 — ELF32 Parser (`src/kernel/exec/elf.h`, `elf.c`)

A minimal ELF32 parser that validates the binary and locates the loadable
segments.  Only static ELF binaries are supported (no dynamic linking).

**Supported ELF features:**

| Feature | Requirement |
|---|---|
| Class | ELF32 (`ELFCLASS32`) |
| Data | Little-endian (`ELFDATA2LSB`) |
| Type | `ET_EXEC` (static) or `ET_DYN` (PIC) |
| Machine | ARM (`EM_ARM = 40`) |
| Flags | `EF_ARM_EABI_VER5`, `EF_ARM_ABI_FLOAT_SOFT` |
| Segments | `PT_LOAD` only; `PT_DYNAMIC` not supported |

**ELF header definitions (`src/kernel/exec/elf.h`):**

```c
/* Minimal ELF32 definitions — no libc elf.h dependency */

#define EI_NIDENT   16
#define ELFMAG      "\x7fELF"
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define ET_DYN      3
#define EM_ARM      40
#define PT_LOAD     1

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;          /* entry point virtual address */
    uint32_t e_phoff;          /* program header table offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;          /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;         /* offset in file */
    uint32_t p_vaddr;          /* virtual address */
    uint32_t p_paddr;          /* physical address (unused) */
    uint32_t p_filesz;         /* size in file */
    uint32_t p_memsz;          /* size in memory (may be > filesz for BSS) */
    uint32_t p_flags;          /* PF_R=4, PF_W=2, PF_X=1 */
    uint32_t p_align;
} elf32_phdr_t;

#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4
```

**Parser API (`src/kernel/exec/elf.c`):**

```c
/* Validate ELF header: magic, class, endian, machine, type.
 * Returns 0 on success, -ENOEXEC if invalid. */
int elf_validate(const elf32_ehdr_t *ehdr);

/* Find and return the PT_LOAD segments.  Fills phdr[] up to max_phdr.
 * Returns number of PT_LOAD segments found, or negative errno. */
int elf_load_segments(const elf32_ehdr_t *ehdr, const uint8_t *file_base,
                      elf32_phdr_t *phdr, int max_phdr);

/* Return the entry point address from the ELF header. */
uint32_t elf_entry(const elf32_ehdr_t *ehdr);
```

The parser does **not** perform relocation — Phase 3 ELF binaries are
either statically linked at a fixed address (romfs XIP) or use PIC
with GOT-relative addressing that needs no kernel relocation.  Full
relocation support (for SD-loaded binaries) is deferred to Phase 4.

**Gotcha:** ARM Thumb entry points have bit 0 set (Thumb bit).  The ELF
`e_entry` should already include this.  Verify with `arm-none-eabi-nm`
that the `_start` symbol has the `T` flag.

### Step 3 — `execve()` Implementation (`src/kernel/exec/exec.c`)

`execve` is the core system call that replaces the current process image
with a new program.  In Phase 3, it loads an ELF binary from romfs and
transitions the process to unprivileged Thread mode.

**Algorithm:**

1. **Open the binary:** `vfs_lookup(path)` → get vnode with `xip_addr`
2. **Validate ELF:** read the ELF header from `xip_addr` (or via
   `romfs_read` if not XIP); call `elf_validate()`
3. **Locate segments:** call `elf_load_segments()` for the PT_LOAD list
4. **Allocate pages:** for `.data`/`.bss` segments (RW), allocate pages
   from the page pool.  `.text`/`.rodata` segments (RX/RO) on romfs
   execute directly from flash via XIP — no SRAM copy needed
5. **Copy `.data` from flash to SRAM:** `memcpy` from flash XIP address to
   the allocated data page
6. **Zero `.bss`:** the portion of a PT_LOAD segment where `p_memsz >
   p_filesz` is zeroed in the SRAM page
7. **Allocate user stack:** one 4 KB page from the page pool
8. **Set up argument strings on user stack:** copy `argv[]` and `envp[]`
   strings to the top of the stack page, build the `argc, argv, envp`
   layout expected by `_start`
9. **Build user-mode exception frame:** construct a hardware exception
   frame at the top of the user stack:
   ```
   [r0=argc, r1=argv, r2=envp, r3=0, r12=0, lr=0, pc=entry, xpsr=T_bit]
   ```
   `xpsr` bit 24 (Thumb) must be set.
10. **Update PCB:** set `stack_page`, `user_pages[]`, `sp` (PSP), clear
    signal handlers
11. **Free old pages:** release the previous process's pages (if this is
    an exec after vfork, the parent's pages are not freed — only the
    child's temporary allocations)
12. **Update MPU:** `mpu_switch(current)` to protect the new process's
    memory regions
13. **Return to user mode:** set PSP to the prepared stack; PendSV returns
    with `EXC_RETURN = 0xFFFFFFFD` (return to Thread mode using PSP)

**Unprivileged Thread mode transition:**

The Cortex-M0+ uses the `CONTROL` register to select privileged vs
unprivileged Thread mode.  Setting `CONTROL.nPRIV` (bit 0) to 1 makes
Thread mode unprivileged.  Setting `CONTROL.SPSEL` (bit 1) to 1 makes
Thread mode use PSP.

```c
/* In exec path, before returning to the new process: */
static inline void switch_to_user_mode(void) {
    uint32_t ctrl;
    __asm__ volatile("mrs %0, control" : "=r"(ctrl));
    ctrl |= 3;   /* nPRIV=1 (unprivileged), SPSEL=1 (use PSP) */
    __asm__ volatile("msr control, %0\n isb" :: "r"(ctrl));
}
```

**Gotcha — exec after vfork:** In the `vfork()` model, the child runs in
the parent's address space.  `execve()` in the child must allocate *new*
pages and never free the parent's pages.  The `vfork_parent` field in the
PCB (added in Step 5) tracks this relationship.

**API:**

```c
/* src/kernel/exec/exec.h */

/* Load and execute an ELF binary, replacing the current process image.
 * Does not return on success; returns negative errno on failure. */
int do_execve(const char *path, char *const argv[], char *const envp[]);
```

---

## Week 2: XIP Execution, vfork, and waitpid

### Step 4 — XIP Execution Path

The key advantage of romfs: `.text` segments of ELF binaries placed in
flash execute directly via XIP.  No code is copied to SRAM — only `.data`,
`.bss`, stack, and heap consume SRAM pages.

**How it works:**

1. `mkromfs` places ELF file data at 4-byte-aligned offsets in the romfs
   image (guaranteed by Phase 2)
2. The romfs image lives at flash address `0x10011000`
3. When `execve` opens a romfs file, the vnode's `xip_addr` points to the
   start of the ELF data in flash
4. The ELF loader reads `PT_LOAD` segments:
   - **Executable segments** (`PF_X`): leave in flash.  The `p_vaddr` is
     rewritten to the flash physical address for the entry point
   - **Data segments** (`PF_W`): copy `p_filesz` bytes from flash to an
     SRAM page; zero the remaining `p_memsz - p_filesz` bytes (BSS)
5. The `pc` in the exception frame points into flash — the CPU fetches
   instructions via XIP

**Memory usage per process (romfs XIP binary):**

| Region | Location | Size |
|---|---|---|
| `.text` + `.rodata` | Flash (XIP) | 0 SRAM |
| `.data` + `.bss` | SRAM page(s) | 4 KB typically |
| Stack | SRAM page | 4 KB |
| Heap (via brk) | SRAM page(s) | 0–4 KB initially |

For a minimal `hello` binary: one page for stack = 4 KB total SRAM.
This means the 52-page pool supports up to ~25 concurrent simple
processes (with 2 pages reserved for kernel overhead).

**MPU Region 2 update for XIP execution:**

Region 2 currently covers only the process stack.  With XIP execution,
the user's `.data` page(s) also need to be accessible.  Two approaches:

- **Option A:** Expand Region 2 to cover all of the process's SRAM pages
  (stack + data).  If they are in adjacent pages, a single region works.
- **Option B:** Use Region 2 for stack and Region 1 (flash) already covers
  `.text`.  The `.data` pages live in the page pool region, which is
  currently not MPU-protected (falls under the default map via
  `PRIVDEFENA`).

Phase 3 uses Option B: `.data` and stack pages are in the page pool, which
is accessible by default.  Full per-process page isolation requires the
RP2350's 8 MPU regions.

**XIP verification test:**

After `execve("/bin/hello")` succeeds, verify with GDB:
- `pc` is in the `0x100xxxxx` range (flash)
- `sp` is in the `0x200xxxxx` range (SRAM page)
- Stepping through instructions fetches from flash without faulting

### Step 5 — `vfork()` System Call

`vfork()` is the process creation primitive for memory-constrained systems
without an MMU (design spec §4.3).  The parent is suspended while the
child runs in the parent's address space.  The child must immediately call
`execve()` or `_exit()`.

**Algorithm:**

1. `sys_vfork()` is called by the parent process
2. **Allocate child PCB:** `proc_alloc()` → new PID, state = RUNNABLE
3. **Share parent's address space:** child's `stack_page` = parent's
   `stack_page` (temporarily); child's `user_pages[]` = parent's
4. **Record parent link:** `child->vfork_parent = current`
5. **Block parent:** set `current->state = PROC_BLOCKED`
6. **Copy parent's stack frame:** The child's CPU context is a snapshot
   of the parent's — same registers, same stack.  When the child
   starts running, it continues from the `vfork()` call site
7. **Set return values:** parent's stacked `r0` = child's PID (set when
   parent resumes); child's stacked `r0` = 0 (child sees vfork return 0)
8. **Schedule child:** trigger PendSV; child runs next

**The child then calls `execve()`, which:**
- Allocates fresh pages for the new program
- Releases the parent-sharing relationship
- Unblocks the parent: `child->vfork_parent->state = PROC_RUNNABLE`,
  `child->vfork_parent = NULL`
- Replaces the child's process image

**If the child calls `_exit()` instead of `execve()`:**
- The parent is unblocked with the child's exit status
- The child becomes ZOMBIE

**PCB additions:**

```c
/* In proc.h */
typedef enum {
    PROC_FREE, PROC_RUNNABLE, PROC_SLEEPING, PROC_BLOCKED, PROC_ZOMBIE
} proc_state_t;

typedef struct pcb {
    /* ... existing fields ... */
    struct pcb *vfork_parent;   /* non-NULL while child shares parent's space */
    int         exit_status;    /* set by _exit(), read by waitpid() */
    /* ... */
} pcb_t;
```

**Gotcha — vfork stack sharing:** The child must not use more stack than
the parent had at the `vfork()` call point.  In practice, the child's
code path is: `vfork()` returns → `execve()` call → syscall trap.  This
is a very shallow stack depth.  `execve` allocates a new stack page before
the child uses any significant stack.

**Gotcha — scheduler interaction:** `PROC_BLOCKED` is a new state.
`sched_next()` must skip `PROC_BLOCKED` processes (same as `PROC_SLEEPING`
and `PROC_ZOMBIE`).

### Step 6 — `waitpid()` and `_exit()` Enhancements

**`_exit()` (enhanced from Phase 1):**

Phase 1's `sys_exit` simply marks the process as ZOMBIE.  Phase 3 adds:

1. Free all user pages (`user_pages[]`, `stack_page` if allocated by exec)
2. Close all open file descriptors (decrement refcnt, call `close`)
3. Reset signal handlers
4. If `vfork_parent` is set: unblock the parent
5. If parent is alive: mark as ZOMBIE (parent must `waitpid()`)
6. If parent is dead (orphan): immediately free the PCB slot
7. Reparent any children of the exiting process to PID 1 (init)
8. Wake any process blocked in `waitpid()` on this PID

**`waitpid(pid, status, options)`:**

```c
/* sys_proc.c */
long sys_waitpid(long pid, long status_ptr, long options, long _unused) {
    /* pid > 0: wait for specific child
     * pid = -1: wait for any child
     * options & WNOHANG: return immediately if no child exited */

    while (1) {
        pcb_t *child = find_zombie_child(current, (pid_t)pid);
        if (child) {
            int status = encode_wait_status(child->exit_status);
            if (status_ptr)
                *(int *)status_ptr = status;
            pid_t cpid = child->pid;
            proc_free(child);     /* release PCB slot */
            return cpid;
        }
        if (!has_living_child(current, (pid_t)pid))
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        /* Block until a child exits */
        current->state = PROC_BLOCKED;
        sched_yield();
    }
}
```

**Wait status encoding (simplified, compatible with POSIX macros):**

```c
#define W_EXITCODE(ret) ((ret) << 8)

/* WIFEXITED(s) → ((s) & 0x7f) == 0 */
/* WEXITSTATUS(s) → ((s) >> 8) & 0xff */
```

---

## Week 3: Pipes, File Descriptor Duplication, and Heap

### Step 7 — `pipe()` System Call (`src/kernel/fd/pipe.c`)

Pipes are the inter-process communication primitive for shell pipelines.
A pipe is a unidirectional byte stream backed by an SRAM ring buffer.

**Design:**

```c
#define PIPE_BUF_SIZE  512u   /* power of 2 for cheap modulo */

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    volatile uint16_t head;   /* write position */
    volatile uint16_t tail;   /* read position */
    uint8_t  readers;         /* number of open read ends */
    uint8_t  writers;         /* number of open write ends */
} pipe_t;
```

**Allocation:** `pipe_t` (516 bytes) is allocated from a page — one pipe
per allocation.  With `PIPE_BUF_SIZE = 512`, a single 4 KB page can hold
7 pipes, though in practice 2–3 concurrent pipes are the expected maximum.
For simplicity, Phase 3 allocates one pipe from a kernel heap page using
`kmem_alloc()`.

**File operations:**

```c
static const struct file_ops pipe_read_ops  = { pipe_read,  NULL,        pipe_close };
static const struct file_ops pipe_write_ops = { NULL,       pipe_write,  pipe_close };
```

`sys_pipe(int fds[2])`:
1. Allocate a `pipe_t`
2. Allocate two `struct file` objects: one for read end, one for write end
3. Both share the same `pipe_t` via `file->priv`
4. `fds[0]` = read end fd, `fds[1]` = write end fd

**Blocking behavior:**

- `pipe_read()`: if the buffer is empty and writers > 0, block the reader
  (set `PROC_SLEEPING`, yield).  The writer wakes the reader after writing.
  If writers == 0 and buffer empty, return 0 (EOF).
- `pipe_write()`: if the buffer is full and readers > 0, block the writer.
  The reader wakes the writer after reading.  If readers == 0, return
  `-EPIPE` (broken pipe — in Phase 3, no SIGPIPE delivery yet).

**Waking:** `pipe_read` wakes sleeping writers; `pipe_write` wakes
sleeping readers.  The sleeping process is identified by a `wait_channel`
pointer in the PCB (set to the `pipe_t` address).

**PCB addition:**

```c
/* In proc.h */
typedef struct pcb {
    /* ... existing fields ... */
    void *wait_channel;   /* sleep/wakeup synchronization (NULL = not waiting) */
    /* ... */
} pcb_t;
```

**`pipe_close()`:**

Decrements `readers` or `writers` count.  If both reach 0, free the
`pipe_t`.  If writers reaches 0 while a reader is blocked, wake the
reader (returns EOF).  If readers reaches 0 while a writer is blocked,
wake the writer (returns `-EPIPE`).

### Step 8 — `dup()` and `dup2()` System Calls

File descriptor duplication is essential for shell I/O redirection and
pipeline setup.

**`dup(fd)`** — syscall number 41:

Find the lowest-numbered free fd in `current->fd_table[]`, point it to
the same `struct file` as `fd`, increment `file->refcnt`.  Return the
new fd.

**`dup2(oldfd, newfd)`** — syscall number 63:

If `newfd` is already open, close it first.  Then duplicate `oldfd` to
`newfd`.  If `oldfd == newfd`, return `newfd` without doing anything.

```c
long sys_dup2(long oldfd, long newfd, long _unused1, long _unused2) {
    if (oldfd < 0 || oldfd >= FD_MAX || !current->fd_table[oldfd])
        return -EBADF;
    if (newfd < 0 || newfd >= FD_MAX)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;
    if (current->fd_table[newfd])
        fd_close(current, (int)newfd);
    current->fd_table[newfd] = current->fd_table[oldfd];
    current->fd_table[newfd]->refcnt++;
    return newfd;
}
```

**Shell pipeline usage pattern:**

```
parent: pipe(fds)        → fds[0]=read, fds[1]=write
parent: vfork()
child1: dup2(fds[1], 1)  → stdout = pipe write end
child1: close(fds[0])
child1: close(fds[1])
child1: execve("cmd1")
parent: vfork()
child2: dup2(fds[0], 0)  → stdin = pipe read end
child2: close(fds[0])
child2: close(fds[1])
child2: execve("cmd2")
parent: close(fds[0])
parent: close(fds[1])
parent: waitpid(child1)
parent: waitpid(child2)
```

### Step 9 — `brk`/`sbrk` Heap Management (`src/kernel/syscall/sys_mem.c`)

User processes need dynamic memory allocation.  The `brk` syscall manages
the program break — the end of the data segment.  `malloc` (in musl libc,
Phase 5) builds on top of `brk`/`mmap`.

**PCB additions:**

```c
/* In proc.h */
typedef struct pcb {
    /* ... existing fields ... */
    uint32_t brk_base;   /* initial break (end of .data+.bss, page-aligned) */
    uint32_t brk_current; /* current break (grows upward) */
    /* ... */
} pcb_t;
```

**`sys_brk(addr)` — syscall number 45:**

```c
long sys_brk(long addr, long _1, long _2, long _3) {
    if (addr == 0)
        return current->brk_current;   /* query current break */

    uint32_t new_brk = (uint32_t)addr;
    if (new_brk < current->brk_base)
        return -ENOMEM;

    /* Calculate pages needed */
    uint32_t old_pages = pages_for(current->brk_current - current->brk_base);
    uint32_t new_pages = pages_for(new_brk - current->brk_base);

    if (new_pages > old_pages) {
        /* Allocate additional pages */
        for (uint32_t i = old_pages; i < new_pages; i++) {
            void *p = page_alloc();
            if (!p) return -ENOMEM;
            current->user_pages[current->n_user_pages++] = p;
        }
    } else if (new_pages < old_pages) {
        /* Free excess pages */
        for (uint32_t i = new_pages; i < old_pages; i++) {
            page_free(current->user_pages[--current->n_user_pages]);
        }
    }

    current->brk_current = new_brk;
    return new_brk;
}
```

**Memory layout of a user process after exec:**

```
Low address:
  [.text in flash — XIP, not in SRAM]
  [.rodata in flash — XIP, not in SRAM]

SRAM pages:
  Page N:   .data (copied from flash) + .bss (zeroed) + heap (brk grows →)
  Page N+1: [allocated on demand when brk crosses page boundary]
  ...
  Page M:   Stack (grows ↓ from top of page)
High address
```

**User-space `sbrk` wrapper (`user/syscall.h`):**

```c
static inline void *sbrk(int32_t incr) {
    uint32_t old = sys_brk(0);
    uint32_t new = sys_brk((void *)(old + incr));
    if (new == (uint32_t)-ENOMEM) return (void *)-1;
    return (void *)old;
}
```

**Limitation:** `user_pages[]` in the PCB currently has 4 slots (from
Phase 1).  This limits a process to 4 data/heap pages = 16 KB max heap.
Sufficient for Phase 3 testing; expanded in Phase 5 when musl malloc
needs more.

---

## Week 4: Signals and Integration Testing

### Step 10 — Signal Infrastructure (`src/kernel/signal/signal.c`)

Signals are required for busybox ash (job control, SIGCHLD on child
exit, SIGPIPE on broken pipe).  Phase 3 implements the core delivery
mechanism; Phase 5 expands coverage for full busybox compatibility.

**Supported signals in Phase 3:**

| Signal | Number | Default Action | Note |
|---|---|---|---|
| SIGHUP | 1 | Terminate | |
| SIGINT | 2 | Terminate | Ctrl-C from tty (Phase 5) |
| SIGQUIT | 3 | Terminate | |
| SIGKILL | 9 | Terminate | Cannot be caught |
| SIGPIPE | 13 | Terminate | Broken pipe |
| SIGTERM | 15 | Terminate | |
| SIGCHLD | 17 | Ignore | Child status change |
| SIGSTOP | 19 | Stop | Cannot be caught (Phase 5) |
| SIGCONT | 18 | Continue | (Phase 5) |

**PCB additions:**

```c
#define NSIG  32

typedef void (*sighandler_t)(int);
#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)

typedef struct pcb {
    /* ... existing fields ... */
    sighandler_t sig_handlers[NSIG];  /* SIG_DFL or user function pointer */
    uint32_t     sig_pending;         /* bitmask of pending signals */
    uint32_t     sig_blocked;         /* bitmask of blocked signals (sigprocmask) */
    /* ... */
} pcb_t;
```

**`sys_kill(pid, sig)` — syscall number 37:**

1. Find the target process by PID
2. Set the bit in `target->sig_pending`
3. If the target is sleeping/blocked, wake it (set RUNNABLE)

**`sys_sigaction(sig, act, oldact)` — syscall number 67:**

Sets or queries the signal handler for a signal.  In Phase 3, only the
handler function pointer is used (no `sa_flags`, no `sa_mask` — these
are Phase 5 refinements).

**Signal delivery — return-to-userspace trampoline:**

Signals are delivered when a process returns from kernel to user mode
(at the end of any syscall or interrupt).  The check happens in
`PendSV_Handler` or `SVC_Handler` after the main work is done:

```c
/* Called just before returning to user mode */
void signal_deliver(pcb_t *p) {
    uint32_t deliverable = p->sig_pending & ~p->sig_blocked;
    if (!deliverable) return;

    int sig = __builtin_ctz(deliverable);  /* lowest pending signal */
    p->sig_pending &= ~(1u << sig);

    sighandler_t handler = p->sig_handlers[sig];

    if (handler == SIG_IGN) return;

    if (handler == SIG_DFL) {
        /* Default action: terminate (most signals) or ignore (SIGCHLD) */
        if (sig == SIGCHLD) return;
        do_exit(128 + sig);   /* killed by signal */
        return;
    }

    /* User handler — set up signal trampoline on user stack */
    signal_setup_frame(p, sig, handler);
}
```

**Signal trampoline:**

To invoke a user-space signal handler, the kernel saves the current user
context and builds a new exception frame that will call the handler:

1. Save the current user exception frame (r0-r3, r12, lr, pc, xpsr)
   onto the user stack as a `sigframe_t`
2. Push a sigreturn trampoline address as the return address:
   ```asm
   /* trampoline code placed in a known location */
   sigreturn_trampoline:
       movs r7, #SYS_sigreturn
       svc  0
   ```
3. Build a new exception frame: `r0 = sig`, `pc = handler`,
   `lr = &sigreturn_trampoline`
4. When the handler returns, it calls `sys_sigreturn`, which restores
   the saved user context

**Trampoline placement:** The trampoline is a small code snippet that
must be at a known, executable address.  Two options:
- Place in flash (romfs or kernel .text) — fixed address, always XIP
- Place on the user stack — but stack is not executable on Cortex-M
  with MPU

Phase 3 places `sigreturn_trampoline` in the kernel `.text` section
(flash XIP).  It is only 4 bytes (2 Thumb instructions).  The address
is hardcoded in the signal frame setup.

**`sys_sigreturn()` — internal syscall:**

Restores the saved user context from `sigframe_t` on the user stack.
Resumes execution from where the signal interrupted the process.

**QEMU note:** Signal delivery works identically on QEMU — it is pure
register/stack manipulation with no hardware dependency.

### Step 11 — Testing Strategy

Phase 3 introduces a three-tier testing strategy.  Each tier catches
different classes of bugs at different speeds.

#### Tier 1: Host-Native Unit Tests (`tests/`)

Extend the existing host-native test suite (Phase 1: `test_page`,
`test_kmem`, `test_fd`) with new suites for Phase 3 kernel modules.
These compile with `gcc` on the host, run instantly, and catch logic
bugs before any target testing.

**New host-native test suites:**

| Test File | Module Under Test | What It Covers |
|---|---|---|
| `tests/test_elf.c` | `src/kernel/exec/elf.c` | ELF header validation (magic, class, machine, endian), reject invalid headers (wrong magic, ELF64, big-endian, non-ARM), PT_LOAD segment extraction, entry point Thumb bit check |
| `tests/test_pipe_unit.c` | `src/kernel/fd/pipe.c` | Ring-buffer write/read logic (empty, full, wrap-around), reader/writer count tracking, EOF on last writer close, EPIPE on last reader close |
| `tests/test_signal_unit.c` | `src/kernel/signal/signal.c` | Pending bitmask set/clear, blocked mask filtering, SIG_IGN/SIG_DFL dispatch logic, SIGKILL cannot be caught/blocked |

**How to run:**

```sh
cd build_tests && cmake ../tests && make && ctest --output-on-failure
```

**Test stub additions:**

- `tests/stubs/proc_stub.c` — provides a dummy `current` PCB and
  `proc_table` for tests that reference process state
- `tests/stubs/page_stub.c` — mock `page_alloc`/`page_free` for
  pipe allocation tests without the real page pool

**Example: `tests/test_elf.c`:**

```c
#include "test_framework.h"
#include "kernel/exec/elf.h"

/* A minimal valid ELF32 ARM header for testing */
static const uint8_t valid_elf[] = {
    0x7f, 'E', 'L', 'F',       /* e_ident[EI_MAG0..3] */
    1,                           /* EI_CLASS = ELFCLASS32 */
    1,                           /* EI_DATA = ELFDATA2LSB */
    1,                           /* EI_VERSION = EV_CURRENT */
    0, 0,0,0,0,0,0,0,0,         /* EI_OSABI..EI_PAD */
    2, 0,                        /* e_type = ET_EXEC */
    40, 0,                       /* e_machine = EM_ARM */
    /* ... remaining fields ... */
};

static void test_valid_elf_accepted(void) {
    ASSERT_EQ(elf_validate((const elf32_ehdr_t *)valid_elf), 0);
}

static void test_bad_magic_rejected(void) {
    uint8_t bad[sizeof(valid_elf)];
    memcpy(bad, valid_elf, sizeof(bad));
    bad[0] = 0x00;   /* corrupt magic */
    ASSERT_EQ(elf_validate((const elf32_ehdr_t *)bad), -ENOEXEC);
}

static void test_elf64_rejected(void) {
    uint8_t bad[sizeof(valid_elf)];
    memcpy(bad, valid_elf, sizeof(bad));
    bad[4] = 2;   /* ELFCLASS64 */
    ASSERT_EQ(elf_validate((const elf32_ehdr_t *)bad), -ENOEXEC);
}

static void test_entry_has_thumb_bit(void) {
    elf32_ehdr_t hdr = { /* ... valid header with e_entry = 0x101 ... */ };
    ASSERT(elf_entry(&hdr) & 1, "entry point must have Thumb bit set");
}

int main(void) {
    printf("=== test_elf ===\n");
    TEST_GROUP("Validation");
    RUN_TEST(test_valid_elf_accepted);
    RUN_TEST(test_bad_magic_rejected);
    RUN_TEST(test_elf64_rejected);
    TEST_GROUP("Entry point");
    RUN_TEST(test_entry_has_thumb_bit);
    TEST_SUMMARY();
}
```

**`tests/CMakeLists.txt` additions:**

```cmake
add_executable(test_elf ${SRC}/kernel/exec/elf.c test_elf.c)
target_include_directories(test_elf PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME elf COMMAND test_elf)

add_executable(test_pipe_unit ${SRC}/kernel/fd/pipe.c test_pipe_unit.c)
target_include_directories(test_pipe_unit PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pipe_unit PRIVATE stubs)
add_test(NAME pipe_unit COMMAND test_pipe_unit)

add_executable(test_signal_unit ${SRC}/kernel/signal/signal.c test_signal_unit.c)
target_include_directories(test_signal_unit PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_signal_unit PRIVATE stubs)
add_test(NAME signal_unit COMMAND test_signal_unit)
```

#### Tier 2: User-Space On-Target Tests (`user/test_*.c`)

These are **real user-space ELF binaries** that run on the kernel
(via QEMU or hardware).  They exercise the complete syscall path:
user code → SVC trap → kernel handler → hardware → result.  This is
the critical new testing tier — it catches integration bugs that host
tests cannot reach (exception frame layout, MPU configuration, XIP
execution, scheduler interaction, etc.).

**User-space test framework (`user/utest.h`):**

A minimal assertion framework that reports results via `sys_write` to
fd 1 (UART).  No libc — only SVC wrappers.

```c
/* user/utest.h — On-target user-space test framework */
#ifndef UTEST_H
#define UTEST_H

#include "syscall.h"

static int ut_fail = 0;
static int ut_total = 0;

/* Write a string literal to stdout */
#define UT_PRINT(s) sys_write(1, (s), sizeof(s) - 1)

/* Convert an int to decimal and print (no snprintf in freestanding) */
static inline void ut_print_int(int v) {
    char buf[12];
    int i = 0;
    if (v < 0) { UT_PRINT("-"); v = -v; }
    if (v == 0) { UT_PRINT("0"); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (--i >= 0) sys_write(1, &buf[i], 1);
}

/* Core assertion: check condition, print PASS/FAIL with file:line */
#define UT_ASSERT(cond, msg) do {                    \
    ut_total++;                                      \
    if (!(cond)) {                                   \
        ut_fail++;                                   \
        UT_PRINT("  FAIL  " msg " (" __FILE__ ":"); \
        ut_print_int(__LINE__);                      \
        UT_PRINT(")\n");                             \
    }                                                \
} while (0)

#define UT_ASSERT_EQ(a, b) do {                      \
    ut_total++;                                      \
    long _a = (long)(a), _b = (long)(b);             \
    if (_a != _b) {                                  \
        ut_fail++;                                   \
        UT_PRINT("  FAIL  expected ");               \
        ut_print_int((int)_b);                       \
        UT_PRINT(" got ");                           \
        ut_print_int((int)_a);                       \
        UT_PRINT(" (" __FILE__ ":");                 \
        ut_print_int(__LINE__);                      \
        UT_PRINT(")\n");                             \
    }                                                \
} while (0)

/* Print summary and exit with 0 (all pass) or 1 (failures) */
#define UT_SUMMARY(name) do {                        \
    UT_PRINT(name ": ");                             \
    ut_print_int(ut_total);                          \
    UT_PRINT(" tests, ");                            \
    ut_print_int(ut_fail);                           \
    UT_PRINT(" failed\n");                           \
    return ut_fail ? 1 : 0;                          \
} while (0)

#endif /* UTEST_H */
```

**Test binaries — each is a standalone ELF placed in romfs `/bin/`:**

**`user/test_exec.c` — ELF loading and XIP execution:**

```c
#include "utest.h"

int main(void) {
    /* If we reached here, exec + XIP worked */
    UT_ASSERT(1, "exec reached main");

    /* Verify we are running from flash (XIP) */
    uint32_t pc;
    __asm__ volatile("mov %0, pc" : "=r"(pc));
    UT_ASSERT(pc >= 0x10000000 && pc < 0x20000000,
              "PC should be in flash XIP range");

    /* Verify stack is in SRAM */
    uint32_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    UT_ASSERT(sp >= 0x20000000 && sp < 0x20042000,
              "SP should be in SRAM range");

    /* Verify we are in unprivileged mode */
    /* (Reading CONTROL.nPRIV from unprivileged mode — if privileged,
     *  bit 0 would be 0; if we got here from exec, it should be 1) */

    /* Test getpid works from user mode */
    pid_t pid = sys_getpid();
    UT_ASSERT(pid > 0, "getpid should return positive PID");

    UT_SUMMARY("test_exec");
}
```

**`user/test_vfork.c` — vfork + exec + waitpid:**

```c
#include "utest.h"

int main(void) {
    /* Test 1: vfork + exec + waitpid basic flow */
    pid_t child = sys_vfork();
    if (child == 0) {
        sys_execve("/bin/hello", NULL, NULL);
        sys_exit(99);   /* should not reach */
    }
    UT_ASSERT(child > 0, "vfork should return child PID to parent");

    int status = 0;
    pid_t reaped = sys_waitpid(child, &status, 0);
    UT_ASSERT_EQ(reaped, child);
    /* hello exits with 0 via return from main */
    UT_ASSERT_EQ((status >> 8) & 0xff, 0);

    /* Test 2: vfork + _exit (no exec) */
    pid_t child2 = sys_vfork();
    if (child2 == 0) {
        sys_exit(42);
    }
    int status2 = 0;
    sys_waitpid(child2, &status2, 0);
    UT_ASSERT_EQ((status2 >> 8) & 0xff, 42);

    /* Test 3: waitpid with WNOHANG on no zombie */
    pid_t nochild = sys_waitpid(-1, NULL, 1 /* WNOHANG */);
    UT_ASSERT(nochild == 0 || nochild == -10 /* ECHILD */,
              "WNOHANG should return 0 or ECHILD");

    UT_SUMMARY("test_vfork");
}
```

**`user/test_pipe.c` — pipe + dup2 + cross-process I/O:**

```c
#include "utest.h"

int main(void) {
    /* Test 1: basic pipe write + read */
    int fds[2];
    int ret = sys_pipe(fds);
    UT_ASSERT_EQ(ret, 0);
    UT_ASSERT(fds[0] >= 0, "pipe read fd valid");
    UT_ASSERT(fds[1] >= 0, "pipe write fd valid");
    UT_ASSERT(fds[0] != fds[1], "pipe fds distinct");

    const char msg[] = "PIPEDATA";
    sys_write(fds[1], msg, 8);
    char buf[16] = {0};
    ssize_t n = sys_read(fds[0], buf, 16);
    UT_ASSERT_EQ(n, 8);
    /* Manual strcmp — no libc */
    int match = 1;
    for (int i = 0; i < 8; i++)
        if (buf[i] != msg[i]) match = 0;
    UT_ASSERT(match, "pipe data round-trip intact");

    sys_close(fds[0]);
    sys_close(fds[1]);

    /* Test 2: pipe across vfork + dup2 */
    int fds2[2];
    sys_pipe(fds2);
    pid_t pid = sys_vfork();
    if (pid == 0) {
        sys_close(fds2[0]);
        sys_dup2(fds2[1], 1);       /* stdout = pipe write end */
        sys_close(fds2[1]);
        sys_write(1, "CHILD", 5);   /* goes through pipe */
        sys_exit(0);
    }
    sys_close(fds2[1]);
    char buf2[16] = {0};
    ssize_t n2 = sys_read(fds2[0], buf2, 16);
    UT_ASSERT_EQ(n2, 5);
    match = 1;
    for (int i = 0; i < 5; i++)
        if (buf2[i] != "CHILD"[i]) match = 0;
    UT_ASSERT(match, "pipe across vfork: data correct");
    sys_close(fds2[0]);
    sys_waitpid(pid, NULL, 0);

    /* Test 3: EOF on pipe when writer closes */
    int fds3[2];
    sys_pipe(fds3);
    sys_close(fds3[1]);             /* close write end */
    char buf3[4];
    ssize_t n3 = sys_read(fds3[0], buf3, 4);
    UT_ASSERT_EQ(n3, 0);           /* should return EOF */
    sys_close(fds3[0]);

    UT_SUMMARY("test_pipe");
}
```

**`user/test_signal.c` — signal delivery and handler execution:**

```c
#include "utest.h"

static volatile int sig_received = 0;
static volatile int sig_number = 0;

static void handler(int sig) {
    sig_received = 1;
    sig_number = sig;
}

int main(void) {
    /* Test 1: install handler and send signal to self */
    sys_sigaction(10 /* SIGUSR1 */, (void *)handler, NULL);
    sys_kill(sys_getpid(), 10);
    UT_ASSERT(sig_received, "handler should have been called");
    UT_ASSERT_EQ(sig_number, 10);

    /* Test 2: SIG_IGN — signal should be silently ignored */
    sig_received = 0;
    sys_sigaction(10, (void *)1 /* SIG_IGN */, NULL);
    sys_kill(sys_getpid(), 10);
    UT_ASSERT(!sig_received, "SIG_IGN should not call handler");

    /* Test 3: handler is called, then execution resumes */
    sig_received = 0;
    sys_sigaction(10, (void *)handler, NULL);
    int before = 42;
    sys_kill(sys_getpid(), 10);
    int after = before;   /* should still be 42 after handler returns */
    UT_ASSERT_EQ(after, 42);
    UT_ASSERT(sig_received, "handler called on second delivery");

    UT_SUMMARY("test_signal");
}
```

**`user/test_brk.c` — heap growth via brk/sbrk:**

```c
#include "utest.h"

int main(void) {
    /* Test 1: query current break */
    uint32_t brk0 = (uint32_t)sys_brk(0);
    UT_ASSERT(brk0 > 0, "initial brk should be non-zero");

    /* Test 2: grow heap by 256 bytes */
    uint32_t brk1 = (uint32_t)sys_brk((void *)(brk0 + 256));
    UT_ASSERT_EQ(brk1, brk0 + 256);

    /* Test 3: write to newly allocated heap */
    volatile uint8_t *heap = (volatile uint8_t *)brk0;
    heap[0] = 0xAA;
    heap[255] = 0x55;
    UT_ASSERT_EQ(heap[0], 0xAA);
    UT_ASSERT_EQ(heap[255], 0x55);

    /* Test 4: grow across a page boundary (4096 bytes) */
    uint32_t brk2 = (uint32_t)sys_brk((void *)(brk0 + 5000));
    UT_ASSERT_EQ(brk2, brk0 + 5000);
    volatile uint8_t *heap2 = (volatile uint8_t *)brk0;
    heap2[4999] = 0xBB;
    UT_ASSERT_EQ(heap2[4999], 0xBB);

    /* Test 5: shrink heap */
    uint32_t brk3 = (uint32_t)sys_brk((void *)(brk0 + 256));
    UT_ASSERT_EQ(brk3, brk0 + 256);

    UT_SUMMARY("test_brk");
}
```

**`user/test_fd.c` — dup, dup2, close, fd limits:**

```c
#include "utest.h"

int main(void) {
    /* Test 1: dup2 redirects stdout */
    int fds[2];
    sys_pipe(fds);
    int saved_stdout = sys_dup(1);      /* save original stdout */
    sys_dup2(fds[1], 1);                /* redirect stdout to pipe */
    sys_write(1, "DUP", 3);
    sys_dup2(saved_stdout, 1);          /* restore stdout */
    sys_close(fds[1]);
    char buf[8] = {0};
    ssize_t n = sys_read(fds[0], buf, 8);
    UT_ASSERT_EQ(n, 3);
    UT_ASSERT(buf[0] == 'D' && buf[1] == 'U' && buf[2] == 'P',
              "dup2 redirect should route write through pipe");
    sys_close(fds[0]);
    sys_close(saved_stdout);

    /* Test 2: close invalid fd returns error */
    int ret = sys_close(99);
    UT_ASSERT(ret < 0, "close(99) should fail");

    /* Test 3: dup2 same fd is a no-op */
    int ret2 = sys_dup2(1, 1);
    UT_ASSERT_EQ(ret2, 1);

    UT_SUMMARY("test_fd");
}
```

#### Tier 3: Test Runner (`user/runtests.c`)

A user-space test runner binary that sequentially `vfork` + `execve`s
each `test_*` binary and collects exit statuses.  This is the top-level
entry point — the kernel `kmain()` execs `/bin/runtests` as PID 1.

```c
/* user/runtests.c — On-target test runner */
#include "syscall.h"

/* No libc — hand-written helpers */
static void print(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static const char *tests[] = {
    "/bin/test_exec",
    "/bin/test_vfork",
    "/bin/test_pipe",
    "/bin/test_signal",
    "/bin/test_brk",
    "/bin/test_fd",
    NULL
};

int main(void) {
    print("=== PPAP on-target test suite ===\n");
    int total = 0, failed = 0;

    for (int i = 0; tests[i]; i++) {
        print("RUN   ");
        print(tests[i]);
        print("\n");

        pid_t pid = sys_vfork();
        if (pid == 0) {
            sys_execve(tests[i], NULL, NULL);
            sys_exit(127);   /* exec failed */
        }

        int status = 0;
        sys_waitpid(pid, &status, 0);
        int code = (status >> 8) & 0xff;

        total++;
        if (code != 0) {
            failed++;
            print("FAIL  ");
            print(tests[i]);
            print("\n");
        } else {
            print("PASS  ");
            print(tests[i]);
            print("\n");
        }
    }

    print("\n=== Results: ");
    /* Print counts (minimal int-to-string) */
    char buf[4];
    buf[0] = '0' + (total % 10); buf[1] = '\0';
    print(buf);
    print(" tests, ");
    buf[0] = '0' + (failed % 10);
    print(buf);
    print(" failed ===\n");

    if (failed == 0)
        print("ALL TESTS PASSED\n");
    else
        print("SOME TESTS FAILED\n");

    return failed;
}
```

**Kernel launch sequence (`kmain()`):**

```c
void kmain(void) {
    /* ... existing init: mm_init, proc_init, vfs_init, mount romfs ... */

    /* Launch /bin/runtests as PID 1 (or /bin/hello for minimal boot) */
    pcb_t *init = proc_alloc();
    do_execve("/bin/runtests", NULL, NULL);
    sched_start();   /* never returns */
}
```

#### QEMU Automation (`scripts/qemu-test.sh`)

A shell script that runs the full on-target test suite under QEMU with
timeout and exit-code extraction.  Suitable for CI integration.

```sh
#!/bin/bash
# scripts/qemu-test.sh — Run on-target tests under QEMU
set -e

BUILD_DIR="${1:-build}"

# Optionally rebuild
if [[ "$2" == "--build" ]]; then
    cmake --build "$BUILD_DIR"
fi

TIMEOUT=10
OUTPUT=$(timeout $TIMEOUT qemu-system-arm \
    -M mps2-an500 -nographic \
    -kernel "$BUILD_DIR/ppap_qemu.elf" 2>&1 || true)

echo "$OUTPUT"

# Check for the summary line
if echo "$OUTPUT" | grep -q "ALL TESTS PASSED"; then
    echo ""
    echo "=== QEMU on-target tests: PASS ==="
    exit 0
else
    echo ""
    echo "=== QEMU on-target tests: FAIL ==="
    exit 1
fi
```

**Integration with the host-native test suite:**

```sh
# Full test workflow: host tests + QEMU on-target tests
cd build_tests && ctest --output-on-failure   # Tier 1: host-native
../scripts/qemu-test.sh ../build              # Tier 2+3: on-target
```

#### Testing Matrix

| What is tested | Tier 1 (Host) | Tier 2 (On-target) | Tier 3 (Runner) |
|---|---|---|---|
| ELF header validation | `test_elf.c` | — | — |
| ELF load + XIP execution | — | `test_exec.c` | via runner |
| vfork + exec + waitpid | — | `test_vfork.c` | via runner |
| Pipe ring-buffer logic | `test_pipe_unit.c` | — | — |
| Pipe end-to-end (cross-process) | — | `test_pipe.c` | via runner |
| dup / dup2 redirection | — | `test_fd.c` | via runner |
| Signal pending/mask logic | `test_signal_unit.c` | — | — |
| Signal handler delivery (trampoline) | — | `test_signal.c` | via runner |
| brk/sbrk heap management | — | `test_brk.c` | via runner |
| SVC dispatch (all new syscalls) | — | all test_*.c | via runner |
| MPU user-mode protection | — | `test_exec.c` | via runner |
| Process lifecycle (spawn → exit → reap) | — | `test_vfork.c` | via runner |
| Page allocator (existing) | `test_page.c` | — | — |
| Kernel object pool (existing) | `test_kmem.c` | — | — |
| fd table (existing) | `test_fd.c` (host) | `test_fd.c` (target) | via runner |

**Total expected assertion count (Phase 3):**
- Host-native (Tier 1): ~40 new assertions (ELF ~15, pipe ~15, signal ~10)
- On-target (Tier 2): ~35 assertions across 6 test binaries
- Runner (Tier 3): 6 test executions with pass/fail collection

#### When Each Tier Runs

| Event | Tier 1 | Tier 2+3 |
|---|---|---|
| After editing kernel C module | Run specific `ctest` | — |
| After building user binaries | — | `scripts/qemu-test.sh` |
| Before committing a step | Full `ctest` | Full QEMU test |
| On hardware | — | Flash + observe UART |

#### Hardware Testing Notes

On real RP2040 hardware, `/bin/runtests` runs exactly as on QEMU.  The
UART output is identical.  The additional value of hardware testing:

- **XIP timing:** Flash cache misses cause stalls not visible on QEMU.
  Verify test_exec completes within expected time.
- **MPU faults:** QEMU mps2-an500 may not emulate MPU; RP2040 enforces
  it.  A user-mode access to kernel memory should HardFault — add a
  manual GDB test for this (not automated, as the process dies).
- **Core 1 interaction:** If Core 1 is running the I/O worker (Phase 1),
  verify it does not interfere with user-process execution.

The test runner prints the same `ALL TESTS PASSED` / `SOME TESTS FAILED`
summary to UART, making hardware test results easy to verify via minicom.

---

## Deliverables

| File | Description |
|---|---|
| `user/Makefile` | User-space build system (cross-compile Thumb PIC ELFs) |
| `user/user.ld` | User linker script (PIC, linked at address 0) |
| `user/crt0.S` | Minimal C runtime: `_start` → `main` → `_exit` |
| `user/syscall.h` | SVC syscall wrapper declarations |
| `user/syscall.S` | SVC instruction stubs (ARM EABI convention) |
| `user/utest.h` | On-target user-space test framework (ASSERT via sys_write) |
| `user/hello.c` | Hello world — first user-space program |
| `user/runtests.c` | Test runner: vfork+exec each test binary, collect results |
| `user/test_exec.c` | On-target: ELF load, XIP execution, user-mode verification |
| `user/test_vfork.c` | On-target: vfork + exec + waitpid + exit status |
| `user/test_pipe.c` | On-target: pipe + dup2 + cross-process I/O + EOF |
| `user/test_signal.c` | On-target: signal handler delivery and SIG_IGN |
| `user/test_brk.c` | On-target: brk/sbrk heap growth + cross-page write |
| `user/test_fd.c` | On-target: dup/dup2 redirection + close errors |
| `tests/test_elf.c` | Host-native: ELF header validation tests |
| `tests/test_pipe_unit.c` | Host-native: pipe ring-buffer logic tests |
| `tests/test_signal_unit.c` | Host-native: signal pending/mask logic tests |
| `tests/stubs/proc_stub.c` | Test stub: dummy PCB for host tests |
| `scripts/qemu-test.sh` | QEMU automation: run on-target tests with timeout |
| `src/kernel/exec/elf.h` | ELF32 header definitions |
| `src/kernel/exec/elf.c` | ELF32 parser: validate, locate segments |
| `src/kernel/exec/exec.c/h` | `do_execve()`: load ELF, set up user mode, XIP |
| `src/kernel/fd/pipe.c/h` | Pipe: SRAM ring-buffer backed `file_ops` |
| `src/kernel/signal/signal.c/h` | Signal delivery, trampoline, `sigreturn` |
| `src/kernel/syscall/sys_proc.c` | Extended: `vfork`, `waitpid`, `execve`, `kill`, `sigaction` |
| `src/kernel/syscall/sys_io.c` | Extended: `pipe`, `dup`, `dup2` |
| `src/kernel/syscall/sys_mem.c` | New: `brk`/`sbrk` heap management |
| `src/kernel/syscall/syscall.c` | Updated dispatch table with all new syscalls |
| `src/kernel/proc/proc.h` | Extended PCB: `vfork_parent`, `exit_status`, `wait_channel`, `sig_*`, `brk_*` |
| `src/kernel/proc/proc.c` | Extended: `_exit` cleanup, reparenting |
| `src/kernel/mm/mpu.c` | Updated: `mpu_switch` accounts for user data pages |
| `src/kernel/main.c` | Updated: launch user process after VFS mount |
| `src/kernel/main_qemu.c` | Updated: same launch sequence for QEMU |
| `CMakeLists.txt` | Updated: user-space build + romfs integration |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| ELF Thumb bit confusion (entry point off by one) | High | Validate with `arm-none-eabi-nm` that `_start` has `T` flag; assert `entry & 1` in loader |
| vfork stack corruption (child overflows shared stack) | High | Keep child code path minimal: vfork → execve only; assert stack pointer range in debug builds |
| PIC GOT access fails without relocation | Medium | Phase 3 binaries are simple enough to avoid GOT; if needed, fall back to `-fno-pic` with fixed addresses |
| Exception frame layout differs between Cortex-M0+ and M33 | Low | Use `#define` for frame offsets; verify with GDB single-step on both QEMU and hardware |
| Signal trampoline at wrong address after kernel code changes | Medium | Place trampoline in a fixed section with a linker symbol; assert address in signal_deliver |
| Pipe deadlock (both reader and writer blocked) | Medium | Set `wait_channel` per-pipe; wake on close; add timeout in debug builds |
| User-space binary too large for flash romfs region | Low | Phase 3 test binaries are tiny (<1 KB each); monitor with `arm-none-eabi-size` |
| `brk` fragmentation when multiple pages freed/allocated | Low | Phase 3 processes are short-lived; pages returned to pool on exit |
| QEMU mps2-an500 `CONTROL.nPRIV` behavior differs from RP2040 | Medium | Test unprivileged mode on both QEMU and hardware; fall back to privileged Thread mode on QEMU if needed |
| GCC `-fpic` generates MOVW/MOVT (Thumb-2, not available on M0+) | High | Use `-fpic -march=armv6s-m` — GCC emits GOT-relative loads instead; verify disassembly |
| Child processes inherit stale fd_table entries after exec | Medium | `do_execve` should close `O_CLOEXEC` fds; Phase 3 does not set `O_CLOEXEC` so all fds are inherited |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — §2.1.3 (CONTROL register), §B3.5 (MPU)
- [ARMv6-M Architecture Reference Manual](https://developer.arm.com/documentation/ddi0419/) — §B1.4.2 (exception return), §B1.4.4 (CONTROL register)
- [ELF for ARM Architecture](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst) — ARM ELF specification, Thumb relocation types
- [PicoPiAndPortable Design Spec v0.2](PicoPiAndPortable-spec-v02.md) — §4.3 (vfork+exec model), §4.5 (ELF loader), §5.1 (process syscalls), §5.2 (pipe, dup)
- [Linux vfork(2)](https://man7.org/linux/man-pages/man2/vfork.2.html) — Reference semantics for vfork behavior

# Embedded CPU Emulation (eCPU)

Functional CPU emulators that interpret foreign instruction sets,
providing the shared foundation for all cross-architecture and
cross-OS binary execution in PPAP.

---

## 1. Overview

eCPU is the **CPU emulation layer** — it provides interpretive
emulators for various instruction set architectures (ARM Thumb, m68k,
Z80, 8086, x86, etc.). Each emulator implements a fetch-decode-execute
loop and exposes hooks for intercepting special instructions (syscalls,
traps, software interrupts).

eCPU itself is architecture-agnostic about what those intercepted
instructions *mean*. That interpretation is provided by **subsystem
personality layers** (see `docs/subsystems/overview.md`), which translate
the intercepted calls into PPAP syscalls. Every form of foreign binary
execution in PPAP — including running PPAP binaries cross-architecture
— is a subsystem built on top of eCPU:

| Use case | eCPU emulator | Subsystem personality |
|---|---|---|
| ARM ELF on m68k PPAP | ecpu-arm | PPAP (register ABI mapping) |
| m68k ELF on ARM PPAP | ecpu-m68k | PPAP (register ABI mapping) |
| CP/M .COM on any PPAP | ecpu-z80 | CP/M BDOS bridge |
| DOS .COM on any PPAP | ecpu-8086 | DOS INT 21h bridge |
| Human68k .X/.R on ARM PPAP | ecpu-m68k | Human68k DOS/F-line bridge |

The PPAP cross-architecture personality is the simplest — it only
remaps registers since PPAP uses the same syscall numbers on all
architectures. Foreign OS personalities do full API translation.

**Analogy:** eCPU is like QEMU's TCG (Tiny Code Generator) — the
pure CPU emulation engine. Subsystems are like QEMU's user-mode
or system-mode layers that give meaning to the emulated instructions.

---

## 2. Architecture

```
+----------------------------------------------------------+
|  Foreign binary                                          |
|  (PPAP ELF, CP/M .COM, DOS .EXE, Human68k .X/.R, etc.)  |
+----------------------------+-----------------------------+
                             |
+----------------------------v-----------------------------+
|  Subsystem personality layer                             |
|  (PPAP ABI remap / CP/M BDOS / DOS INT 21h / etc.)      |
|  Intercepts traps/calls → translates → PPAP syscalls     |
+----------------------------+-----------------------------+
                             |
+----------------------------v-----------------------------+
|  eCPU emulator (this document)                           |
|  - Fetch/decode/execute foreign ISA                      |
|  - Fires callbacks on trap/syscall/special instructions  |
+----------------------------+-----------------------------+
                             |
+----------------------------v-----------------------------+
|  PPAP kernel (native)                                    |
|  syscall_dispatch() — same path as native calls          |
+----------------------------------------------------------+
```

### 2.1 Execution Model

In current PPAP, eCPU-backed subsystem execution is **kernel-embedded**:
the loader (`exec_*`) allocates emulated memory/state, initializes the
CPU core, and runs it as the process image (no separate userland
emulator binary).

Execution flow:

1. `execve()` detects a foreign format and selects a subsystem loader
2. Loader initializes emulated memory, registers, and trap callback
3. Process enters the emulator run loop
4. On trap/syscall instruction: emulator fires the personality callback
5. Personality translates to PPAP syscall ABI and executes natively
6. Return value is written back to emulated registers

### 2.2 Kernel Integration

The kernel's `exec()` path dispatches to the appropriate subsystem
loader when it encounters a foreign binary:

```c
int do_execve(pcb_t *p, const char *path, const char *const *argv) {
    const uint8_t *file = romfs_lookup(path);

    /* Try native ELF */
    if (is_elf(file) && elf_validate(ehdr) == 0)
        return exec_elf_native(p, file, argv);

    /* Try registered subsystems / emulated formats */
    if (is_human68k_x(file, size)) return exec_x68k(...);
    if (is_human68k_r(path, file, size)) return exec_r68k(...);
    if (is_cpm(path, file, size)) return exec_cpm(...);
    if (is_m68k_elf(file, size)) return exec_m68k_emu(...);

    return -ENOEXEC;
}
```

This is the same approach as Linux's `binfmt_misc` — the kernel
replaces the exec with an exec of the emulator, passing the original
binary as an argument. See `docs/subsystems/overview.md` for the full
subsystem detection and dispatch design.

---

## 3. Emulator Design

### 3.1 Interpretive Core

Each eCPU emulator is a simple interpreter. No JIT — PPAP targets have
limited memory and the complexity isn't justified for the expected
workload (small CLI tools, not compute-heavy programs).

```c
typedef struct {
    uint32_t regs[16];     /* general-purpose registers */
    uint32_t pc;           /* program counter */
    uint32_t flags;        /* condition flags (N, Z, C, V) */
    uint8_t *memory;       /* flat address space */
    uint32_t mem_size;
} ecpu_state_t;

void ecpu_run(ecpu_state_t *cpu) {
    for (;;) {
        uint32_t insn = fetch(cpu);
        switch (decode(insn)) {
            case OP_ADD:  /* ... */ break;
            case OP_LOAD: /* ... */ break;
            case OP_SVC:  /* syscall — break out to host */
                ecpu_syscall(cpu);
                break;
            /* ... */
        }
        cpu->pc += insn_size;
    }
}
```

### 3.2 Trap/Syscall Hooks

When the emulator encounters a trap, syscall, or software interrupt
instruction, it does **not** interpret the call itself. Instead, it
invokes a callback provided by the subsystem personality layer:

```c
/* Called by the emulator when it hits a trap instruction */
typedef void (*ecpu_trap_handler_t)(ecpu_state_t *cpu, uint32_t trap_id);

void ecpu_set_trap_handler(ecpu_state_t *cpu, ecpu_trap_handler_t handler);
```

The personality layer registered by each subsystem decides what the
trap means and how to translate it. For example, the PPAP cross-arch
personality for ARM does simple register remapping:

```c
/* PPAP personality: ARM register ABI → native syscall */
void ppap_arm_trap(ecpu_state_t *cpu, uint32_t trap_id) {
    long ret = syscall6(
        cpu->regs[7],   /* syscall number from emulated r7 */
        cpu->regs[0],   /* arg1 from emulated r0 */
        cpu->regs[1],   /* arg2 */
        cpu->regs[2],   /* arg3 */
        cpu->regs[3],   /* arg4 */
        cpu->regs[4],   /* arg5 */
        cpu->regs[5]    /* arg6 */
    );
    cpu->regs[0] = ret; /* return value to emulated r0 */
}
```

While a CP/M personality does full OS call translation:

```c
/* CP/M personality: BDOS call → PPAP syscall */
void cpm_trap(ecpu_state_t *cpu, uint32_t trap_id) {
    uint8_t fn = cpu->regs[REG_C];  /* BDOS function number */
    switch (fn) {
        case 2:  /* Console output */
            write(1, &cpu->regs[REG_E], 1);
            break;
        /* ... */
    }
}
```

See `docs/subsystems/overview.md` for details on each personality layer.

### 3.3 Memory Model

The emulated program sees a flat virtual address space within the
emulator process's memory. The emulator allocates a contiguous region
(e.g., 64 KB for Z80, 1 MB for 8086) via `brk()` or `mmap()` and
loads the foreign binary segments into it.

Pointer arguments in syscalls (e.g., `read(fd, buf, count)` where `buf`
is a pointer) require translation: the emulated pointer is an offset
within the emulator's memory region, which must be converted to a real
host pointer before the native syscall.

```c
void *ecpu_translate_ptr(ecpu_state_t *cpu, uint32_t guest_addr) {
    if (guest_addr >= cpu->mem_size)
        return NULL;  /* EFAULT */
    return cpu->memory + guest_addr;
}
```

---

## 4. Supported Emulator Cores

Each core is a standalone interpretive emulator. The same core can be
used by multiple subsystem personalities.

### 4.1 ecpu-arm (ARMv6-M Thumb-1)

Emulates ARMv6-M Thumb-1 instructions. The simplest because:
- Thumb-1 is a small ISA (~60 instructions)
- Fixed 16-bit instruction encoding (mostly)
- No floating point
- PPAP ARM binaries are already Thumb-only

Used by: PPAP cross-arch personality (ARM ELF on m68k).
Size estimate: ~2000 lines of C, ~8 KB binary.

### 4.2 ecpu-m68k (Motorola 68000)

Emulates Motorola 68000 instructions:
- Larger ISA than Thumb-1 but well-documented
- Variable-length instructions (2-10 bytes)
- 8 data + 8 address registers
- Condition codes in SR

Used by: PPAP cross-arch (m68k ELF on ARM), Human68k personality.
Size estimate: ~4000 lines of C, ~16 KB binary.

### 4.3 ecpu-z80 (Zilog Z80 / Intel 8080)

Emulates Z80 instructions (~150 + CB/DD/ED/FD prefix groups):
- 8-bit CPU, 64 KB address space
- Superset of Intel 8080 — 8080 CP/M programs run unmodified
- Shadow register set, IX/IY index registers

Used by: CP/M personality.
Size estimate: ~3000 lines of C, ~12 KB binary.

### 4.4 ecpu-8086 (Intel 8086 real mode)

Emulates 8086/8088 real-mode instructions:
- 16-bit CPU, 1 MB address space (segment:offset)
- Variable-length instructions with ModR/M addressing
- Segment registers (CS, DS, ES, SS)

Used by: DOS personality.
Size estimate: ~5000 lines of C, ~20 KB binary.

### 4.5 ecpu-armv6 (ARMv6 full ARM + Thumb)

Emulates full ARM + Thumb for Pi Zero binaries:
- Significantly more complex (ARM mode + Thumb interwork)
- Useful for running Pi Zero binaries on RP2040 or X68000
- VFP instructions can be trapped and emulated in software

Used by: PPAP cross-arch personality (ARMv6 ELF on other hosts).
Size estimate: ~6000 lines of C, ~24 KB binary.

### 4.6 ecpu-x86 (32-bit x86 protected mode) — stretch goal

Emulates i386+ protected-mode instructions:
- 32-bit operands, complex encoding
- Significantly larger ISA than any other target

Used by: Windows PE personality (stretch goal).
Size estimate: ~8000+ lines of C.

---

## 5. Performance Considerations

Interpretive emulation is slow — typically 10-100x slower than native.
This is acceptable for:
- Small CLI utilities (grep, cat, wc, etc.)
- Interactive programs (shells, text editors)
- Programs that spend most time in I/O (syscalls are native speed)

Not suitable for:
- CPU-intensive computation
- Real-time programs
- Programs requiring low latency

On the RP2040 (133 MHz), emulating a simple m68k program would run at
roughly 1-10 MHz equivalent — adequate for text processing and shell
scripts.

---

## 6. Implementation Plan

### Phase 1 — Emulator framework + kernel dispatch

1. Define `ecpu_state_t` common interface and trap hook API
2. Subsystem detection chain in `exec()` (see `docs/subsystems/overview.md`)
3. Compile-time table mapping binary formats to emulator paths

### Phase 2 — First emulator core (ecpu-arm)

1. Write Thumb-1 interpreter (~60 instructions)
2. Trap hook for `svc` instruction
3. Pair with PPAP cross-arch personality (register ABI remap)
4. Test: run ARM `hello` binary on m68k PPAP

### Phase 3 — Second emulator core (ecpu-m68k)

1. Write m68k interpreter
2. Trap hooks for `trap #0` (PPAP) and F-line (Human68k)
3. Pair with PPAP cross-arch personality + Human68k personality
4. Test: run m68k binaries on ARM PPAP

### Phase 4 — Retro CPU emulator cores

1. ecpu-z80 — pair with CP/M personality ✅ (see `docs/ecpu/z80.md`, `docs/subsystems/cpm.md`)
2. ecpu-8086 — pair with DOS personality
3. See `docs/subsystems/overview.md` for per-personality implementation plans

### Phase 5 — Cross-architecture romfs

1. romfs can contain binaries for multiple architectures
2. Symlink `/bin/hello` → `/bin/hello.arm` (native) or
   `/bin/hello.m68k` (emulated) based on target
3. Or: fat binary support (single ELF with multiple arch sections)

---

## 7. Relationship to Subsystems

eCPU provides the **CPU emulation engine**. It does not interpret
syscalls or OS calls — that is the subsystem personality layer's job.

Every use of eCPU goes through a subsystem:

```
eCPU core (this document)     Subsystem personality (docs/subsystems/overview.md)
─────────────────────────     ─────────────────────────────────────────────
ecpu-arm                  ──→ PPAP personality (register ABI remap only)
ecpu-m68k                 ──→ PPAP personality / Human68k personality
ecpu-z80                  ──→ CP/M BDOS personality
ecpu-8086                 ──→ DOS INT 21h personality
ecpu-x86                  ──→ Win32 API personality (stretch)
```

The PPAP cross-architecture personality is the thinnest — same syscall
numbers, only register positions differ. Foreign OS personalities do
full API translation (BDOS→PPAP, INT 21h→PPAP, etc.).

---

## 8. Open Questions

1. ~~**Kernel-space vs user-space emulator**~~ **Resolved:** all eCPU
   emulators are kernel-embedded. The personality bridge calls
   `sys_open()`/`sys_read()`/etc. directly — no trap per translated
   call. See `docs/subsystems/overview.md` §2.3 for rationale.

2. **Signal delivery**: when the host kernel delivers a signal to the
   emulator process, it must translate the signal context to the
   emulated architecture's signal frame. This is complex — defer to
   later phases.

3. **fork() under emulation**: when an emulated process calls fork(),
   the emulator process itself forks. The child emulator continues
   executing the foreign binary. This should work naturally.

4. **Shared libraries**: emulated programs can only use statically
   linked libraries (no foreign-arch ld.so). This matches PPAP's
   existing static-linking approach.

5. **Self-hosting**: could PPAP compile itself under emulation? e.g.,
   run `m68k-elf-gcc` (an x86 binary) on PPAP-ARM via ecpu-x86?
   Theoretically possible but impractically slow.

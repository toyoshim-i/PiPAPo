# `exec` and Subsystem Loader Refactoring

This document describes the refactoring of the `exec` pipeline and subsystem loader mechanism. The work is complete.

---

## 1. Overview

The `exec` pipeline was refactored to cleanly separate three concerns:
1.  **Process Management:** Creating and setting up a new process (`pcb_t`) — handled by `do_execve`.
2.  **Binary Format Loading:** Parsing a specific binary format and loading it into memory — handled by `loader_t` implementations.
3.  **CPU Abstraction:** The interface to a CPU, whether native or emulated — handled by `cpu_ops_t` implementations.

---

## 2. Architecture

### 2.1 CPU Abstraction: `cpu_ops_t`

A generic CPU operations structure abstracts the underlying processor (`src/kernel/cpu/cpu.h`). Each implementation provides state management, memory access, trap handling, and an execution entry point.

**Design note:** An earlier draft included a `destroy_state()` method; it was dropped for optimization — CPU state lifetime is tied to process lifetime and freed via `page_free()` in `sys_exit`/`sys_waitpid`.

Implementations:
-   `ecpu_z80_ops` — Z80 emulator
-   `ecpu_m68k_ops` — m68k emulator
-   `native_cpu_ops` — host architecture (ARM or m68k), selected at compile time via `HOST_ARCH_ID`

### 2.2 Binary Format Loader: `loader_t`

Each supported binary format is represented by a `loader_t` structure (`src/kernel/exec/loader.h`):

```c
typedef struct loader {
    const char* name;
    int (*detect)(const uint8_t* file_buf, uint32_t file_size, const char* path);
    int (*load)(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
                const cpu_ops_t* cpu_ops, void* cpu_state,
                const char* const* argv);
    int required_arch_id;
    int xip;
} loader_t;
```

Registered loaders (in `src/kernel/exec/loader.c`):
-   `com_loader` — CP/M .COM files (`CPU_ARCH_Z80`, non-XIP)
-   `x_loader` — Human68k X-format (`CPU_ARCH_M68K`, non-XIP)
-   `r_loader` — Human68k R-format (`CPU_ARCH_M68K`, non-XIP)
-   `sos_loader` — S-OS "SWORD" .obj (`CPU_ARCH_Z80`, non-XIP)
-   `m68k_emu_loader` — m68k ELF cross-arch emulation (`CPU_ARCH_M68K`, non-XIP)
-   `elf_loader` — ELF binaries (any arch, XIP)

**Loader ordering:** Format-specific loaders are registered before `elf_loader` so they match first. The `elf_loader` is the final fallback.

**State management:** Each loader manages its own CPU state allocation internally. The coordinator passes `NULL` for `cpu_state`; loaders that need emulated CPU state allocate it themselves (e.g., `com_loader` bundles `z80_state_t` + `cpm_state_t`).

### 2.3 The `do_execve` Coordinator

The `do_execve` function (`src/kernel/exec/exec.c`) orchestrates loading:

1.  **File Loading:** Look up and read the executable file from the VFS (with XIP support for romfs).
2.  **Loader Detection:** Iterate through `loader_registry[]`, calling `loader->detect()`.
3.  **On match:**
    a.  Select CPU ops via `cpu_ops_for_arch()` based on `required_arch_id`.
    b.  Call `loader->load()` with the file buffer, PCB, and CPU ops.
4.  **Post-load:** Free the file buffer if `!loader->xip`. Set `p->comm`, `p->cwd`, reset signal handlers.
5.  **On failure:** Release all allocated resources and return error.

---

## 3. Key Files

-   `src/kernel/cpu/cpu.h` — `cpu_ops_t` definition and architecture IDs
-   `src/kernel/cpu/cpu.c` — CPU ops registry (`cpu_ops_for_arch`)
-   `src/kernel/cpu/cpu_native.c` — `native_cpu_ops` implementation
-   `src/kernel/exec/loader.h` — `loader_t` definition
-   `src/kernel/exec/loader.c` — Loader registry (`loader_registry[]`)
-   `src/kernel/exec/exec.c` — `do_execve` coordinator
-   `src/kernel/exec/elf_loader.c` — ELF format loader
-   `src/kernel/exec/com_loader.c` — CP/M .COM format loader
-   `src/kernel/exec/x_loader.c` — Human68k X-format loader
-   `src/kernel/exec/r_loader.c` — Human68k R-format loader
-   `src/kernel/exec/h68k_emu.c` — Shared Human68k m68k emulator code (ARM host)
-   `src/kernel/exec/sos_loader.c` — S-OS "SWORD" .obj format loader
-   `src/kernel/exec/m68k_emu_loader.c` — m68k ELF cross-arch emulator loader

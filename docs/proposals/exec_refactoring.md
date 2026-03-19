# `exec` and Subsystem Loader Refactoring Proposal

The current implementation of program execution (`exec`) and subsystem loading is a mixture of concerns that makes it difficult to extend and maintain. The core `do_execve` function has a hardcoded chain of format detectors, and each subsystem's `exec_<subsys>` function repeats boilerplate code for process setup. Loaders are tied to subsystems rather than binary formats. This proposal outlines a plan to refactor this mechanism to create clear boundaries and improve modularity.

---

## 1. Goals and Scope

### 1.1 Primary Goal

The primary goal is to refactor the `exec` pipeline to cleanly separate three distinct concerns:
1.  **Process Management:** The core logic of creating and setting up a new process (`pcb_t`).
2.  **Binary Format Loading:** The logic for parsing a specific binary format (ELF, CP/M .COM, Human68k .X, etc.) and loading it into memory.
3.  **CPU Abstraction:** The interface to a CPU, whether it is a physical native CPU or an emulated CPU (eCPU).

### 1.2 In Scope

-   Define a generic CPU abstraction (`cpu_ops_t`) that can represent both native and emulated CPUs.
-   Define a loader abstraction (`loader_t`) for handling specific binary formats.
-   Refactor `do_execve` to be a coordinator that uses registered loaders and CPU abstractions.
-   Migrate existing ELF, CP/M, and Human68k loading logic into the new framework.
-   Make loaders CPU-agnostic; they should operate via the `cpu_ops_t` interface.

### 1.3 Out of Scope

-   Changing the functionality of any specific subsystem (CP/M, Human68k, etc.).
-   Adding new binary formats or CPU emulators in this refactoring, though the goal is to make it easier.
-   Changing the VFS or memory management (`page_alloc`) systems.

---

## 2. Proposed Architecture

The new architecture will be composed of three main components: `do_execve` as the central coordinator, a set of `loader_t` implementations, and a set of `cpu_ops_t` implementations.

### 2.1 CPU Abstraction: `cpu_ops_t`

We will introduce a generic CPU operations structure, `cpu_ops_t`, which abstracts the underlying processor. This is an evolution of the existing `ecpu_core_ops_t`.

```c
/* src/kernel/cpu/cpu.h */

typedef struct cpu_ops {
    const char* name;
    int arch_id;

    // Allocate and initialize a CPU state structure.
    // For native CPUs, this might be minimal. For eCPUs, this allocates the emulator state.
    void* (*create_state)(void);

    // Initialize the CPU for a new process. `memory` is a pointer to the process's address space.
    int (*init)(void* state, uint8_t* memory, uint32_t mem_size);

    // The entry point for running the process's code.
    // For eCPUs, this is the emulator loop.
    // For native CPUs, this function sets up the hardware context and jumps to user code.
    int (*run)(void* state);

    // Set a trap/exception handler.
    void (*set_trap_handler)(void* state, cpu_trap_handler_t handler, void* ctx);

    // Register access
    uint32_t (*get_reg)(void* state, int reg_id);
    void (*set_reg)(void* state, int reg_id, uint32_t val);

    // Memory access
    void* (*translate_ptr)(void* state, uint32_t guest_addr, uint32_t size);
    uint8_t (*read8)(void* state, uint32_t addr);
    void (*write8)(void* state, uint32_t addr, uint8_t val);
    uint16_t (*read16)(void* state, uint32_t addr);
    void (*write16)(void* state, uint32_t addr, uint16_t val);
    uint32_t (*read32)(void* state, uint32_t addr);
    void (*write32)(void* state, uint32_t addr, uint32_t val);

} cpu_ops_t;
```

**Design note:** An earlier draft included a `destroy_state()` method; it was dropped for optimization — CPU state lifetime is tied to process lifetime and freed via `page_free()` in `sys_exit`/`sys_waitpid`.

We provide implementations for all supported CPUs:
-   `ecpu_z80_ops`: Wraps the existing Z80 emulator.
-   `ecpu_m68k_ops`: Wraps the existing m68k emulator.
-   `native_cpu_ops`: A single implementation for the host architecture (ARM or m68k), selected at compile time via `HOST_ARCH_ID`. The `run` function performs a context switch to user mode.

### 2.2 Binary Format Loader: `loader_t`

A `loader_t` structure represents each supported binary format. These loaders are format-specific, not subsystem-specific.

```c
/* src/kernel/exec/loader.h */

typedef struct loader {
    const char* name;

    // Detect if the loader can handle this file.
    int (*detect)(const uint8_t* file_buf, uint32_t file_size, const char* path);

    // Load the binary.
    // This function is responsible for populating memory and setting the initial CPU state
    // (PC, SP, etc.) via the provided cpu_ops interface.
    // It should not modify the pcb_t directly, other than what's needed for loading.
    int (*load)(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
                const cpu_ops_t* cpu_ops, void* cpu_state,
                const char* const* argv);

    // The required CPU architecture for this loader.
    int required_arch_id;

    // If true, the loader executes code directly from the file buffer (XIP).
    // The coordinator keeps the buffer alive after a successful load.
    // If false, the coordinator frees the file buffer after loading.
    int xip;

} loader_t;
```

Loader implementations (registered in `src/kernel/exec/loader.c`):
-   `elf_loader` (in `src/kernel/exec/elf_loader.c`): Handles ELF binaries. CPU-agnostic — all memory writes use `cpu_ops` interface.
-   (Phase 3) `com_loader`: Handles CP/M .COM files. `required_arch_id` = `CPU_ARCH_Z80`.
-   (Phase 3) `x_loader`: For Human68k `.X` files. `required_arch_id` = `CPU_ARCH_M68K`.
-   (Phase 3) `r_loader`: For Human68k `.R` files. `required_arch_id` = `CPU_ARCH_M68K`.

### 2.3 The `do_execve` Coordinator

The `do_execve` function in `src/kernel/exec/exec.c` will be refactored to orchestrate the loading process.

**Current `do_execve` logic** (after Phase 3.1):
1.  **File Loading:** Look up and read the executable file from the VFS (with XIP support).
2.  **Legacy detection chain:** For subsystem formats not yet migrated (Human68k, SOS, m68k-emu), the old hardcoded `exec_*()` calls are tried first. These will be removed as Phase 3 continues.
3.  **Loader registry:** Iterate through `loader_registry[]` (defined in `src/kernel/exec/loader.c`). Call `loader->detect()` on each one.
4.  **On successful detection:**
    a. Select CPU ops via `cpu_ops_for_arch()` based on `required_arch_id` (native or emulated).
    b. Call `loader->load()`, passing the file buffer, PCB, and CPU ops. Each loader manages its own CPU state allocation internally.
5.  **Post-load cleanup:** Free the file buffer if the loader's `xip` flag is false (non-XIP loaders copy data, so the buffer is no longer needed). Set `p->comm`, `p->cwd`, reset signal handlers.
6.  **Final Cleanup:** If no loader was found, or if any step failed, release all allocated resources and return an error.

**Target `do_execve` logic** (after Phase 3 — all loaders migrated):
1.  **File Loading:** Read the executable file from the VFS.
2.  **Loader Detection:** Iterate through `loader_registry[]`. Call `loader->detect()`.
3.  **On successful detection:**
    a. Get the `required_arch_id` from the loader.
    b. **Select CPU ops:** Use `cpu_ops_for_arch()` to choose the appropriate `cpu_ops_t`.
    c. **Call loader:** `loader->load()` populates memory and sets initial CPU state.
    d. **Finalize PCB:** Set `p->cpu_ops`, `p->cpu_state`.
4.  **Final Cleanup:** Release resources on failure.

---

## 3. Implementation Phases

### Phase 1: Establish CPU and Loader Abstractions

**Status: Completed.**

1.  Created `src/kernel/cpu/cpu.h` and `src/kernel/exec/loader.h` with the `cpu_ops_t` and `loader_t` definitions.
2.  Created `src/kernel/cpu/cpu.c` to hold a registry of available `cpu_ops_t*`.
3.  Created `src/kernel/exec/loader.c` to hold a registry of available `loader_t*`.
4.  Implemented `native_cpu_ops` in `src/kernel/cpu/cpu_native.c` — a single native CPU ops for the host architecture (ARM or m68k), selected at compile time. Memory access functions are direct operations.
5.  Adapted the existing `ecpu_z80` and `ecpu_m68k` to the new `cpu_ops_t` interface.

### Phase 2: Refactor `do_execve` and the ELF loader

**Status: Completed.**

1.  Refactored `do_execve` in `exec.c` to use the loader registry for ELF binaries. Legacy subsystem detection chains remain for Phase 3 migration.
2.  Extracted the ELF loading logic into `src/kernel/exec/elf_loader.c` implementing the `loader_t` interface (with `elf_detect` and `elf_load`).
3.  The `elf_loader` is CPU-agnostic: all GOT patching and relocations use `cpu_ops->read32()` / `cpu_ops->write32()`.

### Phase 3: Migrate Subsystem Loaders

1.  **CP/M:** **Status: Completed.**
    -   Created `src/kernel/exec/com_loader.c` implementing `loader_t`.
    -   Moved detection + loading logic from `exec_cpm.c` into `com_loader.c`.
    -   Registered `com_loader` in `src/kernel/exec/loader.c`.
    -   Updated `do_execve` coordinator to select CPU ops via `cpu_ops_for_arch()` based on `required_arch_id`.
    -   Added `xip` flag to `loader_t` so the coordinator knows whether to free the file buffer after loading.
    -   Removed `exec_cpm.c` and `exec_cpm.h`.
2.  **Human68k:**
    -   Create `src/kernel/exec/x_loader.c` and `src/kernel/exec/r_loader.c`.
    -   Move the logic from `human68k_loader.c` and `exec_x68k.c` into these new loaders.
    -   `exec_x68k.c` will be removed.
3.  **Other subsystems (SOS, etc.)** will be migrated in a similar fashion.

---

## 4. Benefits

-   **Separation of Concerns:** `exec` logic, format parsing, and CPU handling are cleanly separated.
-   **Extensibility:** Adding a new executable format requires only a new `loader_t` implementation. Adding a new CPU target (native or emulated) requires a new `cpu_ops_t` implementation.
-   **Reduced Code Duplication:** Process creation boilerplate (argv, signals, cwd) is centralized in `do_execve`.
-   **Transparency:** Loaders can be written without knowledge of whether the target CPU is real or emulated.
-   **Improved Maintainability:** The code will be easier to understand and debug.

---

## 5. Related Files

-   `src/kernel/cpu/cpu.h` — `cpu_ops_t` definition and architecture IDs
-   `src/kernel/cpu/cpu.c` — CPU ops registry (`cpu_ops_for_arch`)
-   `src/kernel/cpu/cpu_native.c` — `native_cpu_ops` implementation
-   `src/kernel/exec/loader.h` — `loader_t` definition
-   `src/kernel/exec/loader.c` — Loader registry (`loader_registry[]`)
-   `src/kernel/exec/elf_loader.c` — ELF format loader
-   `src/kernel/exec/exec.c` — `do_execve` coordinator
-   `src/kernel/exec/com_loader.c` — CP/M .COM format loader
-   `src/kernel/exec/exec_x68k.c` — Human68k exec (legacy, Phase 3 target)
-   `src/kernel/exec/exec_sos.c` — S-OS exec (legacy, Phase 3 target)
-   `src/kernel/exec/exec_m68k_emu.c` — m68k emulator exec (legacy, Phase 3 target)

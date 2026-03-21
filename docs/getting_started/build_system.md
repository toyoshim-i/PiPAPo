# PPAP Build System

This document describes the PPAP build-system configuration surface for
subsystems and eCPU cores. Build flags are one key part of that surface.

The kernel supports selective enabling/disabling of subsystems and emulator
cores via CMake options. All features are enabled by default for backward
compatibility.

## Build Flags

### Subsystems
- **`PPAP_ENABLE_HUMAN68K`** (default: `ON`)  
  Enables the Human68k DOS call compatibility subsystem. When disabled, Human68k X-format (.x) and R-format (.r) binary support is removed.

- **`PPAP_ENABLE_CPM`** (default: `ON`)  
  Enables the CP/M subsystem and Z80 emulator. When disabled, CP/M binary execution and Z80 I/O emulation are removed (note: Z80 eCPU is separately controlled below).

### I/O Options
- **`PPAP_SEMIHOST`** (default: `OFF`)
  Replaces the hardware UART driver with an ARM semihosting backend (`bkpt 0xAB`). Serial I/O goes through the debugger (QEMU with `-semihosting`, or OpenOCD with `arm semihosting enable`) instead of a physical/emulated UART. Useful for one-cable debug setups and new target bringup. ARM targets only.

### Emulator Cores (eCPU)
- **`PPAP_ENABLE_ECPU_M68K`** (default: `ON`)  
  Enables the m68k processor emulator. Used on ARM targets to execute cross-compiled m68k ELF binaries. When disabled, m68k binary emulation is unavailable on non-m68k platforms.

- **`PPAP_ENABLE_ECPU_Z80`** (default: `ON`)  
  Enables the Z80 processor emulator. Used for CP/M emulation and Z80-based programs. Can be disabled independently of CP/M subsystem support.

## Usage Examples

### Build with all subsystems and eCPUs (default)
```bash
cmake -S src/target/qemu_arm -B build/qemu_arm
cmake --build build/qemu_arm
```

### Build without Human68k support
```bash
cmake -S src/target/qemu_arm -B build/qemu_arm -DPPAP_ENABLE_HUMAN68K=OFF
cmake --build build/qemu_arm
```

### Build with minimal subsystems (ELF and core kernel only)
```bash
cmake -S src/target/qemu_arm -B build/qemu_arm \
  -DPPAP_ENABLE_HUMAN68K=OFF \
  -DPPAP_ENABLE_CPM=OFF \
  -DPPAP_ENABLE_ECPU_M68K=OFF \
  -DPPAP_ENABLE_ECPU_Z80=OFF
cmake --build build/qemu_arm
```

### Build with ARM semihosting (QEMU or OpenOCD debug output)
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
  -S src/target/qemu_arm -B build/qemu_arm \
  -DPPAP_SEMIHOST=ON
cmake --build build/qemu_arm
```

### Build with Human68k but without eCPU emulators
```bash
cmake -S src/target/qemu_arm -B build/qemu_arm \
  -DPPAP_ENABLE_ECPU_M68K=OFF \
  -DPPAP_ENABLE_ECPU_Z80=OFF
cmake --build build/qemu_arm
```

## Binary Size Impact

With the default all-features-enabled build, all subsystems and eCPU cores are included in the kernel. Selectively disabling features can reduce kernel binary size:

- **Human68k subsystem**: Removes human68k_bridge.c, h68k_util.c, exec_x68k.c (~100+ KB)
- **CP/M subsystem**: Removes cpm_bridge.c, cpm_loader.c, exec_cpm.c (~80+ KB)
- **m68k eCPU**: Removes ecpu_m68k.c, ecpu_m68k_alu.c, exec_m68k_emu.c, ppap_m68k_bridge.c (~150+ KB)
- **Z80 eCPU**: Removes ecpu_z80.c, ecpu_z80_alu.c (~80+ KB)

## Technical Details

### CMake Implementation
- Options are defined in `cmake/user.cmake`
- Conditional source inclusion is in `cmake/kernel.cmake`
- `target_compile_definitions()` in `cmake/arm_m.cmake` and `cmake/m68k.cmake` apply preprocessor flags to all targets

### Preprocessor Symbols
When building with a flag enabled, the corresponding `PPAP_ENABLE_*` symbol is defined as `1` for C preprocessor:

```c
#ifdef PPAP_ENABLE_HUMAN68K
  // Human68k-specific code
#endif
```

This allows the source files to be compiled conditionally while still being included in the build when enabled.

### Per-Target Control
Each target inherits these flags from its parent architecture CMake file (arm_m.cmake or m68k.cmake). Individual targets can override defaults:

```cmake
# In target CMakeLists.txt, after project() and before target creation:
set(PPAP_ENABLE_HUMAN68K OFF)

# Or via command line:
cmake -S src/target/mytarget -B build/mytarget -DPPAP_ENABLE_HUMAN68K=OFF
```

## Kernel Startup

The `subsys_init()` function in `src/kernel/subsys/subsys.c` registers only the enabled subsystems at boot time. Attempting to execute binaries for disabled subsystems will result in an "unsupported executable format" error.

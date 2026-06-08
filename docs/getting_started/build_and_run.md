# Build And Run

## Build Targets

```sh
./scripts/build.sh all
./scripts/build.sh pico1calc
./scripts/build.sh --test qemu_arm
./scripts/build.sh qemu_m68k
./scripts/build.sh x68k
./scripts/build.sh pico2rv
./scripts/build.sh xtensa_cc
./scripts/build.sh pcxt
```

## Run / Flash Targets

```sh
./scripts/run.sh                        # qemu_arm
./scripts/run.sh --build                # build + run qemu_arm
./scripts/run.sh --build qemu_m68k      # build + run qemu_m68k
./scripts/run.sh --test                 # build with tests + run
./scripts/run.sh --test qemu_m68k
./scripts/run.sh pico1calc              # flash pre-built pico1calc
./scripts/run.sh --build pico1calc      # build + flash pico1calc
./scripts/run.sh x68k                   # build floppy image + launch XEiJ
./scripts/run.sh --build x68k           # build kernel + floppy image + launch XEiJ
```

For XEiJ-specific debugging, pass extra emulator arguments through
`XEIJ_EXTRA_ARGS`:

```sh
XEIJ_EXTRA_ARGS='-pastepipe=on' ./scripts/run.sh --build x68k
```

## Direct CMake Usage

All target builds normally run inside Docker containers.  To run CMake
manually:

```sh
# ARM QEMU target
docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/arm bash -c "
  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
        -S src/target/qemu_arm -B build/qemu_arm && \
  cmake --build build/qemu_arm -- -j\$(nproc)"

# Pico target
docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/arm bash -c "
  cmake -S src/target/pico1calc -B build/pico1calc && \
  cmake --build build/pico1calc -- -j\$(nproc)"

# m68k target
docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/m68k bash -c "
  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake \
        -S src/target/qemu_m68k -B build/qemu_m68k && \
  cmake --build build/qemu_m68k -- -j\$(nproc)"
```

## Build Flags

The kernel supports selective enabling/disabling of subsystems and emulator
cores via CMake options.  Most features are enabled by default for backward
compatibility.

### Subsystems

- `PPAP_ENABLE_HUMAN68K` (default: `ON`): enables Human68k `.x` and `.r`
  binary support.
- `PPAP_ENABLE_CPM` (default: `ON`): enables the CP/M subsystem and Z80 I/O
  emulation.

### Emulator Cores

- `PPAP_ENABLE_ECPU_M68K` (default: `ON`): enables the m68k eCPU used on
  non-m68k platforms for cross-compiled m68k ELF binaries.
- `PPAP_ENABLE_ECPU_Z80` (default: `ON`): enables the Z80 eCPU used by CP/M
  support.

### I/O Options

- `PPAP_SEMIHOST` (default: `OFF`): replaces the ARM hardware UART backend
  with semihosting (`bkpt 0xAB`) for QEMU/OpenOCD debug I/O.

## Flag Examples

```sh
# Default all-feature build
cmake -S src/target/qemu_arm -B build/qemu_arm
cmake --build build/qemu_arm

# Minimal subsystem build
cmake -S src/target/qemu_arm -B build/qemu_arm \
  -DPPAP_ENABLE_HUMAN68K=OFF \
  -DPPAP_ENABLE_CPM=OFF \
  -DPPAP_ENABLE_ECPU_M68K=OFF \
  -DPPAP_ENABLE_ECPU_Z80=OFF
cmake --build build/qemu_arm

# ARM semihosting
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
  -S src/target/qemu_arm -B build/qemu_arm \
  -DPPAP_SEMIHOST=ON
cmake --build build/qemu_arm
```

## Size Impact

Disabling features reduces kernel size by removing their source groups:

- Human68k subsystem: `subsys/human68k/`
- CP/M subsystem: `subsys/cpm/`
- m68k eCPU: `ecpu_m68k*.c` and `subsys/ppap/`
- MS-DOS subsystem: `subsys/msdos/` on pcxt
- Z80 eCPU: `ecpu_z80*.c`

## Implementation Notes

- Build options are defined in `cmake/user.cmake`.
- Conditional source inclusion is handled in `cmake/kernel.cmake`.
- Architecture CMake files such as `cmake/arm_m.cmake` and
  `cmake/m68k.cmake` apply the matching preprocessor symbols.
- Target `CMakeLists.txt` files may override option defaults before target
  creation.

When a flag is enabled, the matching `PPAP_ENABLE_*` symbol is defined as `1`
for C preprocessor use:

```c
#ifdef PPAP_ENABLE_HUMAN68K
  /* Human68k-specific code */
#endif
```

At boot, `subsys_init()` registers only the enabled subsystems.  Executing a
binary for a disabled subsystem fails with an unsupported executable format
error.

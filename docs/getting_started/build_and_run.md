# Build And Run

## Build targets

```sh
./scripts/build.sh pico1calc
./scripts/build.sh --test qemu_arm
./scripts/build.sh qemu_m68k
./scripts/build.sh x68k
```

## Run / flash targets

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

## Direct CMake usage

```sh
# ARM QEMU target
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake \
      -S src/target/qemu_arm -B build/qemu_arm
cmake --build build/qemu_arm

# Pico target
cmake -S src/target/pico1calc -B build/pico1calc
cmake --build build/pico1calc

# m68k target
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake \
      -S src/target/qemu_m68k -B build/qemu_m68k
cmake --build build/qemu_m68k
```

See also [`build_system.md`](build_system.md).

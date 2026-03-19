# Quick Start

## 1. Install toolchains and dependencies

```sh
./scripts/setup_toolchain.sh
```

## 2. Build

```sh
./scripts/build.sh pico1calc
./scripts/build.sh --test qemu_arm
./scripts/build.sh qemu_m68k
./scripts/build.sh x68k
```

## 3. Run / flash

```sh
./scripts/run.sh                        # run qemu_arm
./scripts/run.sh --build qemu_m68k      # build + run qemu_m68k
./scripts/run.sh --test                 # build with tests + run
./scripts/run.sh pico1calc              # flash pre-built pico1calc
./scripts/run.sh x68k                   # build floppy image + launch XEiJ
```

See [`build_and_run.md`](build_and_run.md) for full command variants.

## 4. Test

```sh
./scripts/test.sh
./scripts/test.sh --all
```

See [`testing.md`](testing.md) for test architecture and coverage notes.

## 5. Debug

For hardware debug (OpenOCD + GDB), see [`debugging.md`](debugging.md).

## 6. Shell

PiPAPo uses **push** (PiPAPo μShell) as the default `/bin/sh`.
See [`push.md`](push.md) for usage, builtins, and scripting.

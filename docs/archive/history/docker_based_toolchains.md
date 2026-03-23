# Proposal: Docker-Based Per-Target Toolchains

## Problem

`setup_toolchain.sh` installs all toolchains for all 9 targets in one pass:
ARM (apt), m68k (built from source), RISC-V bare-metal (downloaded),
RISC-V Linux (built from source), Xtensa/ESP-IDF, ia16, plus emulators
(QEMU, XEiJ, 86Box) and debug tools (OpenOCD).

This causes several issues:

- **Slow onboarding**: A developer targeting only qemu_arm still waits for
  m68k GCC (30-60 min) and RISC-V Linux toolchain (30-60 min) to build.
- **Fragile installs**: Source builds (m68k, RISC-V Linux) depend on
  upstream repos (sourceware.org, GitHub) that are intermittently down.
- **Host pollution**: apt packages, symlinks, and Java installs modify the
  host system in ways that are hard to undo.
- **Reproducibility**: Different host distros / package versions lead to
  subtle build differences.
- **Submodule bloat**: `third_party/` submodules (pico-sdk, esp-idf, qemu,
  openocd) are checked out on the host even when the developer only
  targets one arch. Some (esp-idf) are large and pull their own nested
  submodules.

## Proposed Design

One Docker image per target architecture family. Each image contains only
the toolchain(s), SDKs, and emulators needed for that family. Third-party
dependencies currently checked out as git submodules on the host are instead
cloned and built inside the container image at build time.

### Image Layout

| Image | Targets | Contents |
|-------|---------|----------|
| `ppap/arm` | qemu_arm, pico1, pico1calc, pico2 | arm-none-eabi-gcc, Pico SDK, QEMU ARM, OpenOCD (system + RPi fork), gdb-multiarch |
| `ppap/m68k` | qemu_m68k, x68k | m68k-elf-gcc 14.2.0, QEMU m68k, XEiJ, Java 25 |
| `ppap/riscv` | qemu_rv32, pico2rv | riscv32-unknown-elf-gcc, riscv32-unknown-linux-gnu-gcc, Pico SDK, QEMU RISC-V, OpenOCD (RPi fork) |
| `ppap/xtensa` | xtensa_cc | ESP-IDF + Xtensa toolchain, Python venv |
| `ppap/ia16` | ibmpc | ia16-elf-gcc, NASM, 86Box |

User-space sources (musl, busybox, rogue, zexall) remain as host-side
submodules in `third_party/` because they are shared across all arch
families and built per-arch as part of the normal CMake build. They are
small, stable, and accessed via the `/ppap` mount.

### Third-Party Dependencies: From Submodules to In-Container

Currently these live under `third_party/` as git submodules. In the
Docker model, each is cloned and (where needed) built during `docker
build`, producing a self-contained image.

#### Checkout and Install Paths Inside Containers

All third-party sources are checked out under `/opt/ppap/src/`, built
under `/opt/ppap/build/`, and installed under `/opt/ppap/`.

| Dependency | Image(s) | Checkout (in container) | Build dir | Install / SDK path |
|------------|----------|------------------------|-----------|-------------------|
| **pico-sdk** | arm, riscv | `/opt/ppap/src/pico-sdk` | (header-only, no build) | `PICO_SDK_PATH=/opt/ppap/src/pico-sdk` |
| **esp-idf** | xtensa | `/opt/ppap/src/esp-idf` | (managed by idf.py) | `IDF_PATH=/opt/ppap/src/esp-idf` |
| **openocd** (RPi fork) | arm, riscv | `/opt/ppap/src/openocd` | `/opt/ppap/build/openocd` | `/opt/ppap/bin/openocd` |
| **qemu** (m68k build) | m68k | `/opt/ppap/src/qemu` | `/opt/ppap/build/qemu` | `/opt/ppap/bin/qemu-system-m68k` |
| **XEiJ** | m68k | (downloaded jar) | -- | `/opt/ppap/xeij/XEiJ.jar` |
| **86Box** | ia16 | (downloaded AppImage) | -- | `/opt/ppap/86box/86Box.AppImage` |

Toolchains built from source follow the same pattern:

| Toolchain | Image | Build dir | Install path |
|-----------|-------|-----------|--------------|
| m68k-elf-gcc | m68k | `/opt/ppap/build/gcc-m68k` | `/opt/ppap/m68k-elf/` |
| riscv-linux-gnu-gcc | riscv | `/opt/ppap/build/riscv-gnu-toolchain` | `/opt/ppap/riscv-linux/` |
| Xtensa toolchain | xtensa | (via ESP-IDF install.sh) | `IDF_TOOLS_PATH=/opt/ppap/xtensa-tools` |

Toolchains installed from prebuilt binaries:

| Toolchain | Image | Install path |
|-----------|-------|--------------|
| arm-none-eabi-gcc | arm | `/usr/bin/` (apt) |
| riscv32-unknown-elf-gcc | riscv | `/opt/ppap/riscv-elf/` |
| ia16-elf-gcc | ia16 | `/opt/ppap/ia16-elf/` |

#### What Stays as Host Submodules

These remain in `third_party/` on the host and are accessed through the
`/ppap` bind mount:

| Submodule | Reason |
|-----------|--------|
| **musl** | Built per-arch during CMake build; sources shared across all images |
| **busybox** | Same as musl |
| **rogue** | Same as musl |
| **zexall** | Small data files (Z80 test ROMs), included in romfs |

#### What Gets Removed from `.gitmodules`

| Submodule | Current path | Moved to |
|-----------|-------------|----------|
| **pico-sdk** | `third_party/pico-sdk` | Cloned inside ppap/arm and ppap/riscv images |
| **esp-idf** | `third_party/esp-idf` | Cloned inside ppap/xtensa image |
| **openocd** | `third_party/openocd` | Cloned+built inside ppap/arm and ppap/riscv images |
| **qemu** | `third_party/qemu` | Cloned+built inside ppap/m68k image (system QEMU via apt for arm/riscv) |

### Dockerfile Example (m68k)

```dockerfile
FROM ubuntu:24.04

# Build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git \
    openjdk-25-jre-headless curl ca-certificates \
    libglib2.0-dev libpixman-1-dev python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*

# m68k-elf-gcc (from source)
RUN git clone --depth 1 -b binutils-2_43 \
      git://sourceware.org/git/binutils-gdb.git /opt/ppap/src/binutils && \
    mkdir /opt/ppap/build/binutils && cd /opt/ppap/build/binutils && \
    /opt/ppap/src/binutils/configure --target=m68k-elf \
      --prefix=/opt/ppap/m68k-elf --disable-nls --disable-werror && \
    make -j$(nproc) && make install && \
    rm -rf /opt/ppap/build/binutils /opt/ppap/src/binutils

RUN git clone --depth 1 -b releases/gcc-14.2.0 \
      git://gcc.gnu.org/git/gcc.git /opt/ppap/src/gcc && \
    mkdir /opt/ppap/build/gcc && cd /opt/ppap/build/gcc && \
    /opt/ppap/src/gcc/configure --target=m68k-elf \
      --prefix=/opt/ppap/m68k-elf --with-cpu=m68000 \
      --enable-languages=c --without-headers \
      --disable-nls --disable-shared --disable-libssp && \
    make -j$(nproc) all-gcc all-target-libgcc && \
    make install-gcc install-target-libgcc && \
    rm -rf /opt/ppap/build/gcc /opt/ppap/src/gcc

ENV PATH="/opt/ppap/m68k-elf/bin:${PATH}"

# QEMU m68k
RUN git clone --depth 1 -b v9.1.3 \
      https://gitlab.com/qemu-project/qemu.git /opt/ppap/src/qemu && \
    mkdir /opt/ppap/build/qemu && cd /opt/ppap/build/qemu && \
    /opt/ppap/src/qemu/configure --target-list=m68k-softmmu \
      --prefix=/opt/ppap && \
    make -j$(nproc) && make install && \
    rm -rf /opt/ppap/build/qemu /opt/ppap/src/qemu

# XEiJ
RUN mkdir -p /opt/ppap/xeij && \
    curl -L -o /tmp/xeij.zip \
      "https://stdkmd.net/xeij/XEiJ0260308.zip" && \
    unzip /tmp/xeij.zip -d /opt/ppap/xeij && rm /tmp/xeij.zip

WORKDIR /ppap
```

### Directory Structure

```
docker/
  arm/Dockerfile
  m68k/Dockerfile
  riscv/Dockerfile
  xtensa/Dockerfile
  ia16/Dockerfile
```

### How Containers Are Used

Each container:
- Mounts the project root as `/ppap` (read-write); build output goes to
  `/ppap/build/<target>/` which is already under the mount
- Runs as the host user's UID/GID (no root-owned build artifacts)
- Is ephemeral (`--rm`) -- no persistent container state

```bash
# Example: build qemu_arm
docker run --rm -u $(id -u):$(id -g) \
  -v "$PPAP_ROOT:/ppap" \
  -w /ppap \
  ppap/arm \
  cmake -S src/target/qemu_arm -B build/qemu_arm && \
  cmake --build build/qemu_arm
```

### setup_docker.sh (New Script)

A new `scripts/setup_docker.sh` handles Docker image setup, separate from
the existing `setup_toolchain.sh` which continues to work as-is for
host-native installs. This allows step-by-step migration -- developers
can switch one arch at a time to Docker while keeping others host-native.

```bash
#!/bin/bash
# setup_docker.sh -- build/pull Docker images for specified targets
#
# Usage:
#   ./scripts/setup_docker.sh           # build all images
#   ./scripts/setup_docker.sh arm       # build only ARM image
#   ./scripts/setup_docker.sh m68k riscv # build multiple images

TARGETS="${@:-all}"

if [[ "$TARGETS" == "all" ]]; then
  IMAGES="arm m68k riscv xtensa ia16"
else
  IMAGES="$TARGETS"
fi

for img in $IMAGES; do
  case "$img" in
    arm|m68k|riscv|xtensa|ia16) ;;
    *) echo "Unknown image: $img"; exit 1 ;;
  esac
  echo "Building ppap/${img}..."
  docker build -t "ppap/${img}" "docker/${img}/"
done
```

A developer working only on ARM runs `./scripts/setup_docker.sh arm` and
gets a working environment in minutes (image pull or cached build).

Both scripts can coexist: `setup_toolchain.sh` for host-native,
`setup_docker.sh` for containerized. `run.sh` picks the right path
based on which is available (see CMake Integration below).

### CMake Integration

`run.sh` wraps the entire CMake invocation inside the container. It detects
the target, picks the right image, and runs cmake inside. When no Docker
image is found for the target, it falls back to the host-native toolchain
(i.e. the current behavior).

```bash
# scripts/run.sh
target_to_image() {
  case "$1" in
    qemu_arm|pico1|pico1calc|pico2) echo "arm" ;;
    qemu_m68k|x68k)                 echo "m68k" ;;
    qemu_rv32|pico2rv)              echo "riscv" ;;
    xtensa_cc)                      echo "xtensa" ;;
    ibmpc)                          echo "ia16" ;;
  esac
}

IMAGE="ppap/$(target_to_image "$TARGET")"

# Use Docker if image exists, otherwise fall back to host-native
if docker image inspect "$IMAGE" &>/dev/null; then
  docker run --rm -u $(id -u):$(id -g) \
    -v "$PPAP_ROOT:/ppap" -w /ppap \
    "$IMAGE" \
    cmake -S "src/target/${TARGET}" -B "build/${TARGET}" && \
    cmake --build "build/${TARGET}"
else
  # Existing host-native build path (unchanged)
  cmake -S "src/target/${TARGET}" -B "build/${TARGET}" && \
  cmake --build "build/${TARGET}"
fi
```

This means a developer can migrate one arch at a time: run
`./scripts/setup_docker.sh m68k` to switch m68k to Docker while keeping
ARM on the host-native toolchain via `setup_toolchain.sh`.

CMake toolchain files (e.g. `cmake/toolchain_m68k.cmake`) remain
unchanged -- they reference compiler binaries by name, which are on
`$PATH` inside the container. The only change needed is that CMake
variables pointing at third-party SDK paths (e.g. `PICO_SDK_PATH`) are
set via environment variables baked into each image, replacing the
current host-relative paths.

### Emulator and Flash Access

All emulators and flashing tools run inside the container with host
device/display forwarding:

- **QEMU** (ARM, m68k, RISC-V): Runs inside the container directly.
  Terminal I/O works over the inherited TTY (`-it` flag). No special
  host access needed.

- **XEiJ** (X68000 emulator, Java): Runs inside the container with X11
  forwarding:
  ```bash
  docker run --rm -u $(id -u):$(id -g) \
    -v "$PPAP_ROOT:/ppap" -w /ppap \
    -e DISPLAY="$DISPLAY" \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    ppap/m68k \
    java -jar /opt/ppap/xeij/XEiJ.jar ...
  ```

- **86Box** (IBM PC emulator): Same X11 forwarding as XEiJ:
  ```bash
  docker run --rm -u $(id -u):$(id -g) \
    -v "$PPAP_ROOT:/ppap" -w /ppap \
    -e DISPLAY="$DISPLAY" \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    ppap/ia16 \
    /opt/ppap/86box/86Box.AppImage ...
  ```

- **OpenOCD / USB flashing** (pico1, pico2, pico2rv, xtensa_cc): Runs
  inside the container with USB device passthrough:
  ```bash
  docker run --rm -u $(id -u):$(id -g) \
    -v "$PPAP_ROOT:/ppap" -w /ppap \
    --device /dev/bus/usb \
    ppap/arm \
    openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg ...
  ```
  For xtensa_cc (ESP32-S3 USB-JTAG or DFU), same `--device` approach.

`run.sh` adds these flags automatically based on the operation
(`--build` vs `--test` vs `--flash`).

### Pre-Built Images (Optional)

To avoid even the Docker build step, images can be pushed to GitHub
Container Registry:

```bash
# CI builds and pushes
docker build -t ghcr.io/user/ppap-arm docker/arm/
docker push ghcr.io/user/ppap-arm

# Developer pulls
docker pull ghcr.io/user/ppap-arm
docker tag ghcr.io/user/ppap-arm ppap/arm
```

This eliminates the sourceware.org / GitHub flakiness entirely -- the
toolchain build happens once in CI, and developers just pull the result.

## Migration Path

1. **Phase 1**: Add `scripts/setup_docker.sh` and Dockerfiles for one arch
   (e.g. m68k, the most painful host-native build). `setup_toolchain.sh`
   stays unchanged. Both paths coexist.
2. **Phase 2**: Add `docker image inspect` fallback logic to `run.sh`.
   Developers can mix Docker and host-native per arch. Add Dockerfiles
   for remaining arches one at a time.
3. **Phase 3**: All arches have Docker images. Publish pre-built images
   to GHCR. Remove pico-sdk, esp-idf, openocd, qemu from `.gitmodules`.
   `setup_toolchain.sh` remains for legacy / bare-metal-only use.

## Trade-offs

| | Docker-based | Current (host-install) |
|---|---|---|
| Setup time (one target) | ~1 min (pull) or ~5 min (build) | 30-90 min |
| Setup time (all targets) | ~5 min (pull) | 60-120 min |
| Reproducibility | Exact same image everywhere | Varies by host |
| Disk usage | ~2-4 GB per image | ~1-2 GB per toolchain |
| Host requirements | Docker only | apt, build-essential, Java, ... |
| Debug (GDB, OpenOCD) | USB passthrough via --device | Native access |
| CI integration | Trivial (use image) | Must replicate setup |

## Open Questions

- Should pico2 and pico2rv share the `ppap/arm` and `ppap/riscv` images
  respectively, or have a combined `ppap/pico2` image? (They share Pico SDK
  and OpenOCD-RP but differ in compiler.)
- ccache: should images include ccache with a host-mounted cache dir
  (`-v ~/.ccache:/ccache`) for faster rebuilds?
- Wayland: for XEiJ / 86Box on Wayland-only hosts, may need XWayland or
  native Wayland support in the container.

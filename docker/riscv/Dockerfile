# =============================================================================
# ppap/riscv — Docker image for RISC-V targets (qemu_rv32, pico2rv)
# =============================================================================
#
# Contains:
#   - riscv32-unknown-elf-gcc (prebuilt from pico-sdk-tools v2.2.0-3)
#   - riscv32-unknown-linux-gnu-gcc (prebuilt from riscv-collab 2026.03.13)
#   - Pico SDK 2.2.0
#   - OpenOCD RPi fork sdk-2.2.0 (RP2350 support)
#   - QEMU RISC-V (apt, qemu-system-misc)
#   - gdb-multiarch (apt)
#   - cmake + ninja
#
# Build:
#   docker build -t ppap/riscv docker/riscv/
#
# Usage:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/riscv \
#     cmake -S src/target/qemu_rv32 -B build/qemu_rv32 && \
#     cmake --build build/qemu_rv32
# =============================================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# -- Apt packages ------------------------------------------------------------

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gdb-multiarch \
    qemu-system-misc \
    cmake ninja-build \
    git python3 ca-certificates wget \
    automake autoconf libtool texinfo \
    libftdi-dev libusb-1.0-0-dev libjim-dev libhidapi-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# -- RISC-V bare-metal toolchain (kernel) ------------------------------------
# Prebuilt from raspberrypi/pico-sdk-tools v2.2.0-3

RUN mkdir -p /opt/ppap/riscv-elf && \
    wget -q -O /tmp/riscv-tc.tar.gz \
      "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.2.0-3/riscv-toolchain-15-x86_64-lin.tar.gz" && \
    tar xf /tmp/riscv-tc.tar.gz -C /opt/ppap/riscv-elf && \
    rm /tmp/riscv-tc.tar.gz

ENV PATH="/opt/ppap/riscv-elf/bin:${PATH}"
ENV PICO_TOOLCHAIN_PATH=/opt/ppap/riscv-elf

# -- RISC-V Linux toolchain (user-space PIE binaries) -----------------------
# Prebuilt from riscv-collab/riscv-gnu-toolchain 2026.03.13.
# Default arch is rv32gc but we pass -march=rv32imac -mabi=ilp32 at build time.

RUN mkdir -p /opt/ppap/riscv-linux && \
    wget -q -O /tmp/riscv-linux-tc.tar.xz \
      "https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2026.03.13/riscv32-glibc-ubuntu-24.04-gcc.tar.xz" && \
    tar xf /tmp/riscv-linux-tc.tar.xz -C /opt/ppap/riscv-linux --strip-components=1 && \
    rm /tmp/riscv-linux-tc.tar.xz

ENV PATH="/opt/ppap/riscv-linux/bin:${PATH}"
ENV PPAP_RISCV_LINUX_TOOLCHAIN=/opt/ppap/riscv-linux

# -- Pico SDK 2.2.0 ---------------------------------------------------------

RUN git clone --depth 1 -b 2.2.0 --recurse-submodules \
      https://github.com/raspberrypi/pico-sdk.git /opt/ppap/src/pico-sdk

ENV PICO_SDK_PATH=/opt/ppap/src/pico-sdk

# -- OpenOCD RPi fork (RP2350 support) --------------------------------------

RUN git clone --depth 1 -b sdk-2.2.0 \
      https://github.com/raspberrypi/openocd.git /opt/ppap/src/openocd && \
    cd /opt/ppap/src/openocd && \
    git submodule update --init --depth 1 && \
    ./bootstrap && \
    ./configure --prefix=/opt/ppap --enable-cmsis-dap --enable-ftdi \
      --disable-werror && \
    make -j$(nproc) && make install && \
    rm -rf /opt/ppap/src/openocd

WORKDIR /ppap

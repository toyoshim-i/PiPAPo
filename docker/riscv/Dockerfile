# =============================================================================
# ppap/riscv — Docker image for RISC-V targets (qemu_rv32, pico2rv)
# =============================================================================
#
# Contains:
#   - riscv32-unknown-elf-gcc (prebuilt from pico-sdk-tools v2.2.0-3)
#   - riscv32-unknown-linux-gnu-gcc (built from source, rv32imac ilp32)
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
    gawk bison flex texinfo \
    automake autoconf libtool \
    libftdi-dev libusb-1.0-0-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# -- RISC-V bare-metal toolchain (kernel) ------------------------------------
# Prebuilt from raspberrypi/pico-sdk-tools v2.2.0-3

RUN mkdir -p /opt/ppap/riscv-elf && \
    wget -q -O /tmp/riscv-tc.tar.gz \
      "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.2.0-3/riscv-toolchain-15-x86_64-lin.tar.gz" && \
    tar xf /tmp/riscv-tc.tar.gz -C /opt/ppap/riscv-elf && \
    rm /tmp/riscv-tc.tar.gz

ENV PATH="/opt/ppap/riscv-elf/bin:${PATH}"

# -- RISC-V Linux toolchain (user-space PIE binaries) -----------------------
# Built from source with --with-abi=ilp32 for PIC-safe soft-float libgcc.

RUN git clone --depth 1 \
      https://github.com/riscv-collab/riscv-gnu-toolchain.git \
      /opt/ppap/src/riscv-gnu-toolchain && \
    mkdir -p /opt/ppap/riscv-linux && \
    cd /opt/ppap/src/riscv-gnu-toolchain && \
    ./configure --prefix=/opt/ppap/riscv-linux \
      --with-arch=rv32imac --with-abi=ilp32 && \
    make linux -j$(nproc) && \
    rm -rf /opt/ppap/src/riscv-gnu-toolchain

ENV PATH="/opt/ppap/riscv-linux/bin:${PATH}"

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

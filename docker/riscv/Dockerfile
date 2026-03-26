# =============================================================================
# ppap/riscv — Docker image for RISC-V targets (qemu_rv32, pico2rv)
# =============================================================================
#
# Contains:
#   - riscv32-unknown-elf-gcc (prebuilt from pico-sdk-tools v2.2.0-3)
#   - riscv32-unknown-linux-gnu-gcc (built from source, soft-float ilp32)
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
# Built from source with --with-abi=ilp32 (soft-float).  Prebuilt riscv-collab
# packages default to rv32gc (double-float) libgcc which is ABI-incompatible
# with PPAP's ilp32 musl.  Building from source gives us:
#   - soft-float PIC libgcc (links into -pie binaries)
#   - ld with -pie support (R_RISCV_RELATIVE relocations)
#
# We only need binutils + gcc (stage 1 + libgcc).  No glibc — PPAP uses musl.

# Step 1: binutils
RUN wget -q -O /tmp/binutils.tar.xz https://ftp.gnu.org/gnu/binutils/binutils-2.44.tar.xz && \
    cd /tmp && tar xf binutils.tar.xz && rm binutils.tar.xz && \
    mkdir /tmp/build-binutils && cd /tmp/build-binutils && \
    /tmp/binutils-2.44/configure \
      --prefix=/opt/ppap/riscv-linux \
      --target=riscv32-unknown-linux-gnu \
      --with-arch=rv32imac --with-abi=ilp32 \
      --disable-werror --disable-gdb --disable-sim \
      --disable-libdecnumber --disable-readline && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/binutils-2.44 /tmp/build-binutils

# Step 2: GCC + libgcc (C compiler, no libc needed — PPAP uses musl)
RUN wget -q -O /tmp/gcc.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-15.1.0/gcc-15.1.0.tar.xz && \
    cd /tmp && tar xf gcc.tar.xz && rm gcc.tar.xz && \
    cd /tmp/gcc-15.1.0 && ./contrib/download_prerequisites && \
    mkdir /tmp/build-gcc && cd /tmp/build-gcc && \
    /tmp/gcc-15.1.0/configure \
      --prefix=/opt/ppap/riscv-linux \
      --target=riscv32-unknown-linux-gnu \
      --with-arch=rv32imac --with-abi=ilp32 \
      --enable-languages=c \
      --without-headers --with-newlib \
      --disable-shared --disable-threads --disable-libssp \
      --disable-libquadmath --disable-libgomp --disable-libatomic \
      --disable-multilib --disable-decimal-float && \
    make -j$(nproc) all-gcc && make install-gcc && \
    make -j$(nproc) all-target-libgcc && make install-target-libgcc && \
    rm -rf /tmp/gcc-15.1.0 /tmp/build-gcc

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

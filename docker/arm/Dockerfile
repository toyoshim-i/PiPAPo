# =============================================================================
# ppap/arm — Docker image for ARM Cortex-M targets
#   (qemu_arm, pico1, pico1calc, pico2)
# =============================================================================
#
# Contains:
#   - arm-none-eabi-gcc (apt)
#   - gdb-multiarch (apt)
#   - Pico SDK 2.2.0
#   - OpenOCD (system apt for RP2040 + RPi fork sdk-2.2.0 for RP2350)
#   - QEMU ARM (apt, mps2-an500)
#   - cmake + ninja
#
# Build:
#   docker build -t ppap/arm docker/arm/
#
# Usage:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/arm \
#     cmake -S src/target/qemu_arm -B build/qemu_arm \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_arm_m.cmake && \
#     cmake --build build/qemu_arm
# =============================================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# -- Apt packages ------------------------------------------------------------

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-arm-none-eabi binutils-arm-none-eabi \
    gdb-multiarch \
    openocd \
    qemu-system-arm \
    cmake ninja-build \
    git python3 ca-certificates \
    automake autoconf texinfo libtool \
    libftdi-dev libusb-1.0-0-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# -- arm-none-eabi-gdb symlink -----------------------------------------------

RUN if ! command -v arm-none-eabi-gdb >/dev/null 2>&1; then \
      ln -sf "$(command -v gdb-multiarch)" /usr/local/bin/arm-none-eabi-gdb; \
    fi

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

ENV PATH="/opt/ppap/bin:${PATH}"

WORKDIR /ppap

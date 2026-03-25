# =============================================================================
# ppap/m68k — Docker image for Motorola 68000 targets (qemu_m68k, x68k)
# =============================================================================
#
# Contains:
#   - m68k-elf-gcc 14.2.0 (C only, --with-cpu=m68000, built from source)
#   - binutils 2.43 for m68k-elf
#   - QEMU m68k 9.1.3 (built from source, m68k-softmmu only)
#   - XEiJ (X68000 emulator, Java)
#   - cmake + ninja (build system)
#
# Build:
#   docker build -t ppap/m68k docker/m68k/
#
# Usage:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/m68k \
#     cmake -S src/target/qemu_m68k -B build/qemu_m68k \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake && \
#     cmake --build build/qemu_m68k
#
# Docker-only toolchain; no local build script.
# =============================================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# -- Build tools + QEMU build deps + Java for XEiJ --------------------------

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc g++ make \
    texinfo flex bison \
    wget git tar xz-utils ca-certificates \
    cmake ninja-build \
    python3 python3-venv \
    libglib2.0-dev libpixman-1-dev \
    openjdk-21-jre-headless \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# -- binutils 2.43 for m68k-elf ---------------------------------------------

RUN mkdir -p /opt/ppap/src /opt/ppap/build /opt/ppap/m68k-elf && \
    wget -q -O /tmp/binutils.tar.xz \
      "https://ftp.gnu.org/gnu/binutils/binutils-2.43.tar.xz" && \
    tar xf /tmp/binutils.tar.xz -C /opt/ppap/src && \
    rm /tmp/binutils.tar.xz && \
    mkdir /opt/ppap/build/binutils && cd /opt/ppap/build/binutils && \
    /opt/ppap/src/binutils-2.43/configure \
      --target=m68k-elf \
      --prefix=/opt/ppap/m68k-elf \
      --disable-nls --disable-werror \
      --disable-gdb --disable-sim \
      --disable-libdecnumber --disable-readline && \
    make -j$(nproc) && make install && \
    rm -rf /opt/ppap/build/binutils /opt/ppap/src/binutils-2.43

ENV PATH="/opt/ppap/m68k-elf/bin:${PATH}"
ENV PPAP_M68K_TOOLCHAIN=/opt/ppap/m68k-elf

# -- GCC 14.2.0 (C only, m68000) --------------------------------------------

COPY patches/gcc/ /tmp/gcc-patches/

RUN wget -q -O /tmp/gcc.tar.xz \
      "https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz" && \
    tar xf /tmp/gcc.tar.xz -C /opt/ppap/src && \
    rm /tmp/gcc.tar.xz && \
    cd /opt/ppap/src/gcc-14.2.0 && \
    for p in /tmp/gcc-patches/*.patch; do \
      [ -f "$p" ] && patch -p1 < "$p"; \
    done && \
    ./contrib/download_prerequisites && \
    mkdir /opt/ppap/build/gcc && cd /opt/ppap/build/gcc && \
    /opt/ppap/src/gcc-14.2.0/configure \
      --target=m68k-elf \
      --prefix=/opt/ppap/m68k-elf \
      --with-cpu=m68000 \
      --enable-languages=c \
      --without-headers \
      --disable-shared --disable-threads \
      --disable-libssp --disable-libquadmath \
      --disable-libgomp --disable-libatomic \
      --disable-libstdcxx --disable-nls \
      --disable-multilib \
      --with-newlib \
      CFLAGS_FOR_TARGET="-msep-data -fPIC" && \
    make -j$(nproc) all-gcc all-target-libgcc && \
    make install-gcc install-target-libgcc && \
    rm -rf /opt/ppap/build/gcc /opt/ppap/src/gcc-14.2.0

# -- QEMU 9.1.3 (m68k-softmmu only) ----------------------------------------

RUN git clone --depth 1 -b v9.1.3 \
      https://gitlab.com/qemu-project/qemu.git /opt/ppap/src/qemu && \
    mkdir /opt/ppap/build/qemu && cd /opt/ppap/build/qemu && \
    /opt/ppap/src/qemu/configure \
      --target-list=m68k-softmmu \
      --prefix=/opt/ppap && \
    make -j$(nproc) && make install && \
    rm -rf /opt/ppap/build/qemu /opt/ppap/src/qemu

ENV PATH="/opt/ppap/bin:${PATH}"

# -- XEiJ (X68000 emulator) -------------------------------------------------

RUN mkdir -p /opt/ppap/xeij && \
    wget -q -O /tmp/xeij.zip \
      "https://stdkmd.net/xeij/XEiJ_0260308.zip" && \
    unzip -qo /tmp/xeij.zip -d /opt/ppap/xeij && \
    rm /tmp/xeij.zip

# -- System QEMU for ARM (for running qemu-system-m68k tests) ---------------
# Already built from source above as /opt/ppap/bin/qemu-system-m68k

WORKDIR /ppap

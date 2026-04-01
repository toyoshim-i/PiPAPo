# =============================================================================
# ppap/ia16 — Docker image for PC/XT (i8086) target
# =============================================================================
#
# Contains:
#   - ia16-elf-gcc (C compiler for 16-bit x86 real mode, built from source)
#   - NASM (assembler)
#   - QEMU i386 (basic smoke testing)
#   - cmake + ninja (build system)
#
# Build:
#   docker build -t ppap/ia16 docker/ia16/
#
# Usage:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/ia16 \
#     cmake -S src/target/pcxt -B build/pcxt && \
#     cmake --build build/pcxt
#
# The ia16-elf-gcc toolchain is built from tkchia/build-ia16 (GitLab).
# Stages: prereqs → binutils → gcc1 (C only) → newlib
# Full gcc2 (C++) is skipped — PPAP kernel is C only.
# =============================================================================

FROM ubuntu:24.04

# Avoid interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Build tools + ia16 build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc g++ make \
    flex bison texinfo m4 autoconf automake libtool \
    wget git tar bzip2 ca-certificates \
    nasm \
    qemu-system-x86 \
    cmake ninja-build \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Build ia16-elf-gcc from source
# Using tkchia/build-ia16 build scripts (fetch.sh + build.sh)
WORKDIR /opt/ppap/build-ia16

RUN git clone --depth 1 https://gitlab.com/tkchia/build-ia16.git . \
    && chmod +x fetch.sh build.sh

# fetch.sh downloads gcc-ia16, binutils-ia16, newlib-ia16 sources + GMP/MPFR/MPC/ISL
RUN ./fetch.sh

# Build stages: prereqs (math libs) → binutils → gcc1 (C bootstrap) → newlib
# Skip gcc2 (C++) — PPAP is C only. This saves ~15 min build time.
RUN ./build.sh clean prereqs binutils gcc1 newlib

# Move toolchain to /opt/ppap/ia16-elf and clean up build artifacts
RUN mv prefix /opt/ppap/ia16-elf \
    && rm -rf /opt/ppap/build-ia16

ENV PATH="/opt/ppap/ia16-elf/bin:${PATH}"

WORKDIR /ppap

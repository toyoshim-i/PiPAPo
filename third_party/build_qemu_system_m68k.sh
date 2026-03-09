#!/usr/bin/env bash
# build_qemu_system_m68k.sh — Build QEMU m68k emulator from source
#
# Builds QEMU from the third_party/qemu submodule, targeting only
# m68k-softmmu to keep the build fast.
#
# This is optional — the system qemu-system-m68k (apt: qemu-system-misc)
# works in most cases.  Use this script if your system QEMU has bugs with
# the m68k virt machine.
#
# Usage:
#   ./third_party/build_qemu_system_m68k.sh
#
# The built binary lands at:
#   third_party/qemu/build/qemu-system-m68k

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

QEMU_SRC="$SCRIPT_DIR/qemu"
QEMU_BUILD="$QEMU_SRC/build"
QEMU_M68K="$QEMU_BUILD/qemu-system-m68k"

# Already built?
if [[ -x "$QEMU_M68K" ]]; then
    echo "[build-qemu] Already built: $QEMU_M68K"
    "$QEMU_M68K" --version | head -1
    exit 0
fi

# Initialise submodule if needed
if [[ ! -f "$QEMU_SRC/meson.build" ]]; then
    echo "[build-qemu] Initialising QEMU submodule..."
    git -C "$PROJECT_DIR" submodule update --init third_party/qemu
fi

# Install build dependencies if missing
BUILD_DEPS=(meson ninja-build libpixman-1-dev libglib2.0-dev pkg-config python3-venv)
MISSING=()
for pkg in "${BUILD_DEPS[@]}"; do
    dpkg -s "$pkg" &>/dev/null || MISSING+=("$pkg")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "[build-qemu] Installing build dependencies: ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y "${MISSING[@]}"
fi

echo "[build-qemu] Configuring QEMU (m68k-softmmu only)..."
cd "$QEMU_SRC"
./configure --target-list=m68k-softmmu \
            --disable-docs --disable-tools --disable-guest-agent \
            --prefix="$QEMU_BUILD/install" 2>&1 | tail -3

echo "[build-qemu] Building QEMU (this may take a few minutes)..."
make -j"$(nproc)" 2>&1 | tail -5

if [[ -x "$QEMU_M68K" ]]; then
    echo "[build-qemu] Built: $QEMU_M68K"
    "$QEMU_M68K" --version | head -1
else
    echo "[build-qemu] Error: build failed"
    exit 1
fi
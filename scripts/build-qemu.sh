#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Build QEMU m68k from source
# =============================================================================
# Builds QEMU 9.1.x from the third_party/qemu submodule, targeting only
# m68k-softmmu to keep the build fast.
#
# This is optional — the system qemu-system-m68k (apt: qemu-system-misc)
# works in most cases.  Use this script if your system QEMU has bugs with
# the m68k virt machine.
#
# Prerequisites (installed by setup-toolchain.sh):
#   meson, ninja-build, libpixman-1-dev, libglib2.0-dev, pkg-config,
#   python3-venv
#
# Usage:
#   ./scripts/build-qemu.sh
#
# The built binary lands at:
#   third_party/qemu/build/qemu-system-m68k
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
QEMU_SRC="${PPAP_ROOT}/third_party/qemu"
QEMU_BUILD="${PPAP_ROOT}/third_party/qemu/build"
QEMU_M68K="${QEMU_BUILD}/qemu-system-m68k"

info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
error()   { echo "[ERROR] $*" >&2; exit 1; }

# Already built?
if [[ -x "${QEMU_M68K}" ]]; then
  success "QEMU m68k already built: ${QEMU_M68K}"
  "${QEMU_M68K}" --version | head -1
  exit 0
fi

# Initialise submodule if needed
if [[ ! -f "${QEMU_SRC}/meson.build" ]]; then
  info "Initialising QEMU submodule..."
  git -C "${PPAP_ROOT}" submodule update --init third_party/qemu
fi

# Install build dependencies if missing
BUILD_DEPS=(meson ninja-build libpixman-1-dev libglib2.0-dev pkg-config python3-venv)
MISSING=()
for pkg in "${BUILD_DEPS[@]}"; do
  dpkg -s "$pkg" &>/dev/null || MISSING+=("$pkg")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
  info "Installing build dependencies: ${MISSING[*]}"
  sudo apt-get update -qq
  sudo apt-get install -y "${MISSING[@]}"
fi

info "Configuring QEMU (m68k-softmmu only)..."
cd "${QEMU_SRC}"
./configure --target-list=m68k-softmmu \
            --disable-docs --disable-tools --disable-guest-agent \
            --prefix="${QEMU_BUILD}/install" 2>&1 | tail -3

info "Building QEMU (this may take a few minutes)..."
make -j"$(nproc)" 2>&1 | tail -5

if [[ -x "${QEMU_M68K}" ]]; then
  success "QEMU m68k built successfully: ${QEMU_M68K}"
  "${QEMU_M68K}" --version | head -1
else
  error "QEMU m68k build failed"
fi

#!/usr/bin/env bash
# build_openocd_rp.sh — Build the Raspberry Pi fork of OpenOCD (RP2350 support)
#
# Builds from the third_party/openocd submodule (raspberrypi/openocd, sdk-2.2.0)
# and installs to tools/openocd-rp/ (local, no sudo required).
#
# Prerequisites (apt):
#   automake autoconf build-essential texinfo libtool libftdi-dev
#   libusb-1.0-0-dev pkg-config
#
# Usage:
#   ./third_party/build_openocd_rp.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PPAP_ROOT}/third_party/openocd"
INSTALL_DIR="${PPAP_ROOT}/tools/openocd-rp"

if [[ ! -f "${SRC_DIR}/bootstrap" ]]; then
    echo "[openocd] Error: ${SRC_DIR} not found."
    echo "          Run: git submodule update --init third_party/openocd"
    exit 1
fi

echo "[openocd] Building Raspberry Pi OpenOCD (sdk-2.2.0) ..."

cd "${SRC_DIR}"

# Initialize OpenOCD's own submodules (jimtcl, libjaylink)
if [[ ! -f "${SRC_DIR}/jimtcl/configure.ac" ]]; then
    echo "[openocd] Initializing submodules (jimtcl, libjaylink) ..."
    git submodule update --init --depth 1
fi

# Bootstrap autotools
echo "[openocd] Running bootstrap ..."
./bootstrap 2>&1 | tail -3

# Configure — enable CMSIS-DAP (picoprobe) support, install locally
echo "[openocd] Configuring (prefix=${INSTALL_DIR}) ..."
./configure \
    --prefix="${INSTALL_DIR}" \
    --disable-cmsis-dap \
    --enable-cmsis-dap-v2 \
    --enable-picoprobe \
    --enable-internal-jimtcl \
    --enable-internal-libjaylink \
    --disable-werror \
    2>&1 | tail -5

# Build
echo "[openocd] Building ..."
make -j"$(nproc)" 2>&1 | tail -5

# Install locally (no sudo)
echo "[openocd] Installing to ${INSTALL_DIR} ..."
make install 2>&1 | tail -3

# Verify
OPENOCD_BIN="${INSTALL_DIR}/bin/openocd"
if [[ -x "${OPENOCD_BIN}" ]]; then
    echo "[openocd] Success: ${OPENOCD_BIN}"
    "${OPENOCD_BIN}" --version 2>&1 | head -1
    # Verify RP2350 target exists
    if [[ -f "${INSTALL_DIR}/share/openocd/scripts/target/rp2350.cfg" ]]; then
        echo "[openocd] RP2350 target config: OK"
    else
        echo "[openocd] Warning: rp2350.cfg not found in install"
    fi
else
    echo "[openocd] Error: build failed — ${OPENOCD_BIN} not found"
    exit 1
fi

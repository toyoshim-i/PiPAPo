#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Toolchain Setup Script
# =============================================================================
# Sets up the full development environment for building PiPAPo on
# a Debian/Ubuntu-based Linux host.
#
# What this script does:
#   1. Installs required apt packages (ARM cross-toolchain, OpenOCD, etc.)
#   2. Initializes git submodules (Pico SDK, musl, busybox, etc.)
#   3. Verifies the installation
#
# Usage:
#   chmod +x scripts/setup-toolchain.sh
#   ./scripts/setup-toolchain.sh
#
# Requirements:
#   - Debian/Ubuntu-based Linux (uses apt)
#   - sudo privileges
#   - Internet access (for apt and git)
#
# Idempotent: safe to run multiple times; already-installed items are skipped.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Helpers -----------------------------------------------------------------

info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
warn()    { echo "[WARN]  $*"; }
error()   { echo "[ERROR] $*" >&2; exit 1; }

check_cmd() {
  if command -v "$1" &>/dev/null; then
    success "$1 is available: $(command -v "$1")"
    return 0
  else
    return 1
  fi
}

# --- Step 1: apt packages ----------------------------------------------------

info "=== Step 1: Installing apt packages ==="

APT_PACKAGES=(
  gcc-arm-none-eabi       # ARM cross-compiler (armv6m/Thumb)
  binutils-arm-none-eabi  # Assembler, linker, objcopy, objdump
  # m68k-elf toolchain is built from source: ./third_party/build-gcc-m68k.sh
  gdb-multiarch           # GDB with ARM and m68k support
  openocd                 # SWD/JTAG on-chip debugger (v0.12+)
  minicom                 # Serial console
  cmake                   # Build system (>= 3.13 required by Pico SDK)
  ninja-build             # Fast build backend for CMake
  git                     # Version control
  python3                 # Required by Pico SDK scripts
  qemu-system-arm         # QEMU mps2-an500 smoke tests (scripts/qemu.sh)
  qemu-system-misc        # QEMU m68k (virt) for 68000 target
)

# Check which packages are already installed
MISSING=()
for pkg in "${APT_PACKAGES[@]}"; do
  if dpkg -s "$pkg" &>/dev/null; then
    success "apt: $pkg already installed"
  else
    MISSING+=("$pkg")
  fi
done

if [[ ${#MISSING[@]} -gt 0 ]]; then
  info "Installing missing packages: ${MISSING[*]}"
  sudo apt-get update -qq
  sudo apt-get install -y "${MISSING[@]}"
else
  info "All apt packages already installed, skipping apt-get."
fi

# --- Step 1b: arm-none-eabi-gdb symlink --------------------------------------
#
# Ubuntu/Debian dropped the gdb-arm-none-eabi package in favour of
# gdb-multiarch.  Many tools (OpenOCD docs, ppap.gdb, Pico SDK examples)
# invoke `arm-none-eabi-gdb` by name, so create a symlink if absent.

info "=== Step 1b: arm-none-eabi-gdb symlink ==="

if command -v arm-none-eabi-gdb &>/dev/null; then
  success "arm-none-eabi-gdb already available: $(command -v arm-none-eabi-gdb)"
elif command -v gdb-multiarch &>/dev/null; then
  GDB_TARGET="/usr/local/bin/arm-none-eabi-gdb"
  sudo ln -sf "$(command -v gdb-multiarch)" "${GDB_TARGET}"
  success "Created symlink: ${GDB_TARGET} -> $(command -v gdb-multiarch)"
else
  warn "Neither arm-none-eabi-gdb nor gdb-multiarch found — GDB unavailable"
  FAIL=1
fi

# --- Step 2: Git submodules --------------------------------------------------

info "=== Step 2: Initializing git submodules ==="

# Initialize all submodules (pico-sdk, musl, busybox, rogue, qemu)
git -C "${PPAP_ROOT}" submodule update --init --recursive --quiet
success "All git submodules initialized."

# Verify Pico SDK submodule
PICO_SDK_DIR="${PPAP_ROOT}/third_party/pico-sdk"
if [[ -f "${PICO_SDK_DIR}/pico_sdk_init.cmake" ]]; then
  success "Pico SDK present at ${PICO_SDK_DIR}"
else
  warn "Pico SDK submodule not found — run: git submodule update --init --recursive"
fi

# --- Step 3: QEMU m68k check ------------------------------------------------
#
# The system qemu-system-m68k (apt: qemu-system-misc) usually works.
# If you need a newer version, run: ./scripts/build.sh host_qemu
# which builds QEMU 9.1.x from the third_party/qemu submodule.

QEMU_M68K="${PPAP_ROOT}/third_party/qemu/build/qemu-system-m68k"

# --- Step 4: Verification ----------------------------------------------------

info "=== Step 4: Verification ==="

FAIL=0

verify_version() {
  local label="$1"
  local cmd="$2"
  local expected_pattern="$3"
  local output
  output=$(eval "$cmd" 2>&1 | head -1)
  if echo "$output" | grep -qE "$expected_pattern"; then
    success "${label}: ${output}"
  else
    warn "${label}: unexpected output — ${output}"
    FAIL=1
  fi
}

# arm-none-eabi-gcc
verify_version "arm-none-eabi-gcc" \
  "arm-none-eabi-gcc --version" \
  "arm-none-eabi-gcc"

# Check it produces valid armv6m output
echo 'int main(void){return 0;}' > /tmp/ppap_check.c
if arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -nostdlib \
     -o /tmp/ppap_check.elf /tmp/ppap_check.c 2>/dev/null; then
  ARCH=$(arm-none-eabi-readelf -h /tmp/ppap_check.elf | grep Machine)
  success "arm-none-eabi-gcc produces valid ELF: ${ARCH}"
else
  warn "arm-none-eabi-gcc failed to compile a minimal test program"
  FAIL=1
fi
rm -f /tmp/ppap_check.c /tmp/ppap_check.elf

# m68k-elf-gcc (custom toolchain)
M68K_GCC="${PPAP_ROOT}/tools/m68k-toolchain/bin/m68k-elf-gcc"
if [[ -x "$M68K_GCC" ]]; then
  verify_version "m68k-elf-gcc" \
    "$M68K_GCC --version" \
    "m68k-elf-gcc"
else
  warn "m68k-elf-gcc not found. Run: ./third_party/build-gcc-m68k.sh"
  FAIL=1
fi

# OpenOCD
verify_version "openocd" \
  "openocd --version" \
  "Open On-Chip Debugger"

# cmake
verify_version "cmake" \
  "cmake --version" \
  "cmake version 3\."

# Pico SDK
if [[ -f "${PICO_SDK_DIR}/pico_sdk_init.cmake" ]]; then
  success "Pico SDK: present at ${PICO_SDK_DIR}"
else
  warn "Pico SDK: NOT found at ${PICO_SDK_DIR}"
  FAIL=1
fi

# arm-none-eabi-gdb (symlink to gdb-multiarch created in Step 1b)
if check_cmd arm-none-eabi-gdb; then
  :
else
  warn "arm-none-eabi-gdb not found in PATH"
  FAIL=1
fi

# qemu-system-arm
verify_version "qemu-system-arm" \
  "qemu-system-arm --version" \
  "QEMU emulator"

# qemu-system-m68k (prefer local build, fall back to system)
if [[ -x "${QEMU_M68K}" ]]; then
  verify_version "qemu-system-m68k (local)" \
    "${QEMU_M68K} --version" \
    "QEMU emulator"
elif command -v qemu-system-m68k &>/dev/null; then
  verify_version "qemu-system-m68k (system)" \
    "qemu-system-m68k --version" \
    "QEMU emulator"
else
  warn "qemu-system-m68k: not found (install qemu-system-misc or run ./scripts/build.sh host_qemu)"
  FAIL=1
fi

# --- Summary -----------------------------------------------------------------

echo ""
echo "============================================================"
if [[ $FAIL -eq 0 ]]; then
  echo " Toolchain setup complete. All checks passed."
  echo ""
  echo " Next step: build a target."
  echo "   ./scripts/build.sh pico1calc"
  echo "   ./scripts/build.sh qemu_arm"
else
  echo " Setup completed with warnings. Review [WARN] lines above."
fi
echo "============================================================"

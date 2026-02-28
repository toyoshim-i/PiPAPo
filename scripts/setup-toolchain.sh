#!/usr/bin/env bash
# =============================================================================
# PicoPiAndPortable — Toolchain Setup Script
# =============================================================================
# Sets up the full development environment for building PicoPiAndPortable on
# a Debian/Ubuntu-based Linux host.
#
# What this script does:
#   1. Installs required apt packages (ARM cross-toolchain, OpenOCD, etc.)
#   2. Clones the Raspberry Pi Pico SDK (with submodules)
#   3. Writes PICO_SDK_PATH to ~/.bashrc (and ~/.zshrc if zsh is present)
#   4. Verifies the installation
#
# Usage:
#   chmod +x scripts/setup-toolchain.sh
#   ./scripts/setup-toolchain.sh [--sdk-dir <path>]
#
# Options:
#   --sdk-dir <path>   Where to clone the Pico SDK (default: ~/pico-sdk)
#
# Requirements:
#   - Debian/Ubuntu-based Linux (uses apt)
#   - sudo privileges
#   - Internet access (for apt and git)
#
# Idempotent: safe to run multiple times; already-installed items are skipped.
# =============================================================================

set -euo pipefail

# --- Configuration -----------------------------------------------------------

PICO_SDK_REPO="https://github.com/raspberrypi/pico-sdk.git"
PICO_SDK_DIR="${HOME}/pico-sdk"   # overridable via --sdk-dir

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sdk-dir)
      PICO_SDK_DIR="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--sdk-dir <path>]" >&2
      exit 1
      ;;
  esac
done

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
  gdb-multiarch           # GDB with ARM support
  openocd                 # SWD/JTAG on-chip debugger (v0.12+)
  minicom                 # Serial console
  cmake                   # Build system (>= 3.13 required by Pico SDK)
  ninja-build             # Fast build backend for CMake
  git                     # Version control
  python3                 # Required by Pico SDK scripts
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

# --- Step 2: Pico SDK --------------------------------------------------------

info "=== Step 2: Cloning Pico SDK ==="

if [[ -f "${PICO_SDK_DIR}/pico_sdk_init.cmake" ]]; then
  success "Pico SDK already present at ${PICO_SDK_DIR}"
  info "Updating submodules..."
  git -C "${PICO_SDK_DIR}" submodule update --init --recursive --quiet
else
  info "Cloning Pico SDK into ${PICO_SDK_DIR} ..."
  git clone --recurse-submodules "${PICO_SDK_REPO}" "${PICO_SDK_DIR}"
  success "Pico SDK cloned."
fi

# --- Step 3: Set PICO_SDK_PATH environment variable -------------------------

info "=== Step 3: Configuring PICO_SDK_PATH ==="

ENV_LINE="export PICO_SDK_PATH=\"${PICO_SDK_DIR}\""

add_to_shell_rc() {
  local rc_file="$1"
  if [[ -f "$rc_file" ]]; then
    if grep -qF "PICO_SDK_PATH" "$rc_file"; then
      success "PICO_SDK_PATH already set in ${rc_file}"
    else
      echo "" >> "$rc_file"
      echo "# PicoPiAndPortable — Pico SDK path" >> "$rc_file"
      echo "${ENV_LINE}" >> "$rc_file"
      success "Added PICO_SDK_PATH to ${rc_file}"
      warn "Run 'source ${rc_file}' or open a new terminal for it to take effect."
    fi
  fi
}

add_to_shell_rc "${HOME}/.bashrc"
add_to_shell_rc "${HOME}/.zshrc"

# Make it available in the current session too
export PICO_SDK_PATH="${PICO_SDK_DIR}"

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

# gdb-multiarch
if check_cmd gdb-multiarch; then
  :
else
  warn "gdb-multiarch not found in PATH"
  FAIL=1
fi

# --- Summary -----------------------------------------------------------------

echo ""
echo "============================================================"
if [[ $FAIL -eq 0 ]]; then
  echo " Toolchain setup complete. All checks passed."
  echo ""
  echo " PICO_SDK_PATH=${PICO_SDK_PATH}"
  echo ""
  echo " Next step: run CMake to configure the project build."
  echo "   mkdir -p build && cd build"
  echo "   cmake .. && cmake --build ."
else
  echo " Setup completed with warnings. Review [WARN] lines above."
fi
echo "============================================================"

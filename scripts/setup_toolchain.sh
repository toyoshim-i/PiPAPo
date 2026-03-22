#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Toolchain Setup Script
# =============================================================================
# Sets up the full development environment for building PiPAPo on
# a Debian/Ubuntu-based Linux host.
#
# What this script does:
#   1. Installs required apt packages (ARM cross-toolchain, OpenOCD, etc.)
#   1b. Creates arm-none-eabi-gdb symlink (Ubuntu dropped the package)
#   1c. Installs RISC-V toolchain (prebuilt from pico-sdk-tools)
#   1d. Installs ESP-IDF and Xtensa toolchain (for CardComputer / ESP32-S3)
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
DL_DIR="${PPAP_ROOT}/build/downloads"

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
  # m68k-elf toolchain is built from source: ./third_party/build_gcc_m68k.sh
  # RISC-V toolchain: prebuilt from raspberrypi/pico-sdk-tools (Step 1c)
  # Xtensa toolchain: installed via ESP-IDF install.sh (Step 1d)
  gdb-multiarch           # GDB with ARM and m68k support
  openocd                 # SWD/JTAG on-chip debugger (v0.12+, RP2040 only)
  minicom                 # Serial console
  cmake                   # Build system (>= 3.13 required by Pico SDK)
  ninja-build             # Fast build backend for CMake
  git                     # Version control
  python3                 # Required by Pico SDK scripts
  qemu-system-arm         # QEMU mps2-an500 smoke tests (scripts/run.sh)
  qemu-system-misc        # QEMU m68k (virt) for 68000 target
  openjdk-25-jre          # Java 25 runtime for XEiJ (X68000 emulator, needs GUI)
  unzip                   # Needed to extract XEiJ archive
  # Build deps for Raspberry Pi OpenOCD fork (RP2350 support)
  automake                # autotools for OpenOCD build
  autoconf                # autotools for OpenOCD build
  texinfo                 # makeinfo for OpenOCD docs
  libtool                 # libtoolize for OpenOCD build
  libftdi-dev             # FTDI support for OpenOCD
  libusb-1.0-0-dev        # USB support for OpenOCD
  pkg-config              # Build dependency resolution
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

# --- Step 1c: RISC-V toolchain (prebuilt from raspberrypi/pico-sdk-tools) ----
#
# The Pico SDK expects riscv32-unknown-elf-gcc with newlib (nosys.specs).
# The official prebuilt toolchain from raspberrypi/pico-sdk-tools provides
# everything needed.  Installed to tools/riscv-toolchain/ within the project.

info "=== Step 1c: RISC-V toolchain ==="

RISCV_TC_DIR="${PPAP_ROOT}/tools/riscv-toolchain"
RISCV_TC_VER="v2.2.0-3"
RISCV_TC_TAR="riscv-toolchain-15-x86_64-lin.tar.gz"
RISCV_TC_URL="https://github.com/raspberrypi/pico-sdk-tools/releases/download/${RISCV_TC_VER}/${RISCV_TC_TAR}"

if [[ -x "${RISCV_TC_DIR}/bin/riscv32-unknown-elf-gcc" ]]; then
  success "RISC-V toolchain already installed at ${RISCV_TC_DIR}"
else
  mkdir -p "${DL_DIR}"
  if [[ ! -f "${DL_DIR}/${RISCV_TC_TAR}" ]]; then
    info "Downloading RISC-V toolchain (${RISCV_TC_VER})..."
    wget -q --show-progress -O "${DL_DIR}/${RISCV_TC_TAR}.tmp" "${RISCV_TC_URL}"
    mv "${DL_DIR}/${RISCV_TC_TAR}.tmp" "${DL_DIR}/${RISCV_TC_TAR}"
  fi
  info "Extracting RISC-V toolchain to ${RISCV_TC_DIR}..."
  mkdir -p "${RISCV_TC_DIR}"
  tar -xf "${DL_DIR}/${RISCV_TC_TAR}" -C "${RISCV_TC_DIR}"
  if [[ -x "${RISCV_TC_DIR}/bin/riscv32-unknown-elf-gcc" ]]; then
    success "RISC-V toolchain installed to ${RISCV_TC_DIR}"
  else
    warn "RISC-V toolchain extraction failed — check archive contents"
  fi
fi

# --- Step 1d: Xtensa toolchain (CardComputer / ESP32-S3) --------------------
#
# The CardComputer target uses the ESP32-S3 (Xtensa LX7).  ESP-IDF (source)
# lives in third_party/esp-idf as a git submodule (initialized in Step 2).
# The Xtensa cross-toolchain and ESP-IDF Python venv are installed into
# tools/xtensa-toolchain/ (matching tools/m68k-toolchain/, tools/riscv-toolchain/).
#
# Convention:
#   third_party/  = source repos (git submodules)
#   tools/        = built/downloaded artifacts (toolchains, host utilities)

info "=== Step 1d: Xtensa toolchain ==="

ESP_IDF_DIR="${PPAP_ROOT}/third_party/esp-idf"
XTENSA_TC_DIR="${PPAP_ROOT}/tools/xtensa-toolchain"

# apt dependencies for ESP-IDF install.sh
ESP_IDF_APT_DEPS=(
  python3-venv            # ESP-IDF install.sh creates a virtualenv
  python3-pip             # pip for ESP-IDF Python dependencies
  libffi-dev              # Required by some ESP-IDF Python packages
  dfu-util                # USB DFU flashing (ESP32-S3 native USB)
)

ESP_IDF_MISSING=()
for pkg in "${ESP_IDF_APT_DEPS[@]}"; do
  if dpkg -s "$pkg" &>/dev/null; then
    success "apt: $pkg already installed"
  else
    ESP_IDF_MISSING+=("$pkg")
  fi
done

if [[ ${#ESP_IDF_MISSING[@]} -gt 0 ]]; then
  info "Installing ESP-IDF apt dependencies: ${ESP_IDF_MISSING[*]}"
  sudo apt-get install -y "${ESP_IDF_MISSING[@]}"
fi

# ESP-IDF source is a git submodule — checked in Step 2.
# Here we only install the toolchain, which requires the submodule to exist.
# If Step 2 hasn't run yet (first-time setup), we defer silently.
XTENSA_GCC_PATTERN="${XTENSA_TC_DIR}/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-gcc"
XTENSA_GCC=$(compgen -G "${XTENSA_GCC_PATTERN}" | head -1 || true)

if [[ -n "${XTENSA_GCC}" && -x "${XTENSA_GCC}" ]]; then
  success "Xtensa toolchain already installed: ${XTENSA_GCC}"
elif [[ ! -f "${ESP_IDF_DIR}/install.sh" ]]; then
  info "ESP-IDF submodule not yet initialized — Xtensa toolchain will be installed after Step 2"
else
  # IDF_TOOLS_PATH controls where ESP-IDF downloads toolchains and creates
  # its Python venv.  Default is ~/.espressif; we redirect to tools/ to
  # keep everything project-local and match other toolchains.
  info "Running ESP-IDF install.sh for esp32s3 (into ${XTENSA_TC_DIR})..."
  mkdir -p "${XTENSA_TC_DIR}"
  IDF_TOOLS_PATH="${XTENSA_TC_DIR}" "${ESP_IDF_DIR}/install.sh" esp32s3
  # Re-detect after install
  XTENSA_GCC=$(compgen -G "${XTENSA_GCC_PATTERN}" | head -1 || true)
  if [[ -n "${XTENSA_GCC}" && -x "${XTENSA_GCC}" ]]; then
    success "Xtensa toolchain installed: ${XTENSA_GCC}"
  else
    warn "Xtensa toolchain not found after install — check ${XTENSA_TC_DIR}/"
  fi
fi

# --- Step 2: Git submodules --------------------------------------------------

info "=== Step 2: Initializing git submodules ==="

# Initialize top-level submodules only (non-recursive).
# QEMU has broken transitive submodule deps (Zeex/subhook repo is gone)
# and its build script handles its own submodules anyway.
git -C "${PPAP_ROOT}" submodule update --init --quiet
# Pico SDK and ESP-IDF need recursive init for their internal submodules
git -C "${PPAP_ROOT}/third_party/pico-sdk" submodule update --init --recursive --quiet
if [[ -d "${PPAP_ROOT}/third_party/esp-idf" ]]; then
  info "Initializing ESP-IDF submodules (this may take a moment)..."
  git -C "${PPAP_ROOT}/third_party/esp-idf" submodule update --init --recursive --depth 1 --quiet
fi
# OpenOCD RPi fork needs its own submodules (jimtcl, libjaylink)
if [[ -d "${PPAP_ROOT}/third_party/openocd" ]]; then
  git -C "${PPAP_ROOT}/third_party/openocd" submodule update --init --depth 1 --quiet
fi
success "All git submodules initialized."

# Verify Pico SDK submodule
PICO_SDK_DIR="${PPAP_ROOT}/third_party/pico-sdk"
if [[ -f "${PICO_SDK_DIR}/pico_sdk_init.cmake" ]]; then
  success "Pico SDK present at ${PICO_SDK_DIR}"
else
  warn "Pico SDK submodule not found — run: git submodule update --init --recursive"
fi

# Verify ESP-IDF submodule
if [[ -f "${ESP_IDF_DIR}/export.sh" ]]; then
  success "ESP-IDF present at ${ESP_IDF_DIR}"
else
  warn "ESP-IDF submodule not found — run: git submodule update --init --recursive"
fi

# --- Step 2a: Deferred Xtensa toolchain install ------------------------------
#
# If Step 1d was deferred because the ESP-IDF submodule wasn't yet initialized,
# run the toolchain install now that the submodule is available.

XTENSA_GCC=$(compgen -G "${XTENSA_GCC_PATTERN}" | head -1 || true)
if [[ -z "${XTENSA_GCC}" || ! -x "${XTENSA_GCC}" ]] && [[ -f "${ESP_IDF_DIR}/install.sh" ]]; then
  info "Running deferred ESP-IDF install.sh for esp32s3 (into ${XTENSA_TC_DIR})..."
  mkdir -p "${XTENSA_TC_DIR}"
  IDF_TOOLS_PATH="${XTENSA_TC_DIR}" "${ESP_IDF_DIR}/install.sh" esp32s3
  XTENSA_GCC=$(compgen -G "${XTENSA_GCC_PATTERN}" | head -1 || true)
  if [[ -n "${XTENSA_GCC}" && -x "${XTENSA_GCC}" ]]; then
    success "Xtensa toolchain installed: ${XTENSA_GCC}"
  else
    warn "Xtensa toolchain not found after install — check ${XTENSA_TC_DIR}/"
  fi
fi

# --- Step 2b: Raspberry Pi OpenOCD (RP2350 support) -------------------------
#
# The system openocd (apt) is 0.12.0 which only supports RP2040.
# The Raspberry Pi fork (third_party/openocd, sdk-2.2.0) adds RP2350 support.
# Build it locally to tools/openocd-rp/.

info "=== Step 2b: Building Raspberry Pi OpenOCD ==="

OPENOCD_RP_BIN="${PPAP_ROOT}/tools/openocd-rp/bin/openocd"
if [[ -x "${OPENOCD_RP_BIN}" ]]; then
  success "Raspberry Pi OpenOCD already built: ${OPENOCD_RP_BIN}"
else
  info "Building Raspberry Pi OpenOCD (this takes a few minutes)..."
  "${PPAP_ROOT}/third_party/build_openocd_rp.sh"
fi

# --- Step 3: QEMU m68k check ------------------------------------------------
#
# The system qemu-system-m68k (apt: qemu-system-misc) usually works.
# If you need a newer version, run: ./third_party/build_qemu_system_m68k.sh
# which builds QEMU 9.1.x from the third_party/qemu submodule.

QEMU_M68K="${PPAP_ROOT}/third_party/qemu/build/qemu-system-m68k"

# --- Step 3b: XEiJ (X68000 Emulator in Java) --------------------------------
#
# Downloads XEiJ from the official site into build/downloads and extracts
# it to tools/xeij.  Requires a Java runtime (default-jre).

info "=== Step 3b: Installing XEiJ ==="

XEIJ_VER="0260308"
XEIJ_ZIP="XEiJ_${XEIJ_VER}.zip"
XEIJ_URL="https://stdkmd.net/xeij/${XEIJ_ZIP}"
XEIJ_DIR="${PPAP_ROOT}/tools/xeij"

if [[ -f "${XEIJ_DIR}/XEiJ.jar" ]]; then
  success "XEiJ already installed at ${XEIJ_DIR}"
else
  mkdir -p "${DL_DIR}" "${XEIJ_DIR}"
  if [[ -f "${DL_DIR}/${XEIJ_ZIP}" ]]; then
    info "XEiJ archive already downloaded."
  else
    info "Downloading XEiJ ${XEIJ_VER}..."
    wget -q --show-progress -O "${DL_DIR}/${XEIJ_ZIP}.tmp" "${XEIJ_URL}"
    mv "${DL_DIR}/${XEIJ_ZIP}.tmp" "${DL_DIR}/${XEIJ_ZIP}"
  fi
  info "Extracting XEiJ to ${XEIJ_DIR}..."
  XEIJ_TMP=$(mktemp -d)
  unzip -qo "${DL_DIR}/${XEIJ_ZIP}" -d "${XEIJ_TMP}"
  # The zip may contain a top-level directory (e.g. XEiJ/); flatten into XEIJ_DIR
  if [[ -f "${XEIJ_TMP}/XEiJ.jar" ]]; then
    cp -a "${XEIJ_TMP}/." "${XEIJ_DIR}/"
  elif [[ -d "${XEIJ_TMP}/XEiJ" ]]; then
    cp -a "${XEIJ_TMP}/XEiJ/." "${XEIJ_DIR}/"
  else
    # Unknown layout — move whatever was extracted
    cp -a "${XEIJ_TMP}"/*/ "${XEIJ_DIR}/" 2>/dev/null || \
    cp -a "${XEIJ_TMP}/." "${XEIJ_DIR}/"
  fi
  rm -rf "${XEIJ_TMP}"
  if [[ ! -f "${XEIJ_DIR}/XEiJ.jar" ]]; then
    warn "XEiJ.jar not found after extraction — check zip contents"
    info "Zip contents:"
    unzip -l "${DL_DIR}/${XEIJ_ZIP}" | head -20
  else
    success "XEiJ installed to ${XEIJ_DIR}"
  fi
fi

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
  warn "m68k-elf-gcc not found. Run: ./third_party/build_gcc_m68k.sh"
  FAIL=1
fi

# riscv32-unknown-elf-gcc (prebuilt from pico-sdk-tools)
RISCV_GCC="${PPAP_ROOT}/tools/riscv-toolchain/bin/riscv32-unknown-elf-gcc"
if [[ -x "$RISCV_GCC" ]]; then
  verify_version "riscv32-unknown-elf-gcc" \
    "$RISCV_GCC --version" \
    "riscv32-unknown-elf-gcc"
  # Check it produces valid rv32 output
  echo 'int main(void){return 0;}' > /tmp/ppap_rv_check.c
  if "$RISCV_GCC" -march=rv32imac -mabi=ilp32 -nostdlib \
       -o /tmp/ppap_rv_check.elf /tmp/ppap_rv_check.c 2>/dev/null; then
    ARCH="${PPAP_ROOT}/tools/riscv-toolchain/bin/riscv32-unknown-elf-readelf"
    ARCH_OUT=$("$ARCH" -h /tmp/ppap_rv_check.elf | grep Machine)
    success "riscv32-unknown-elf-gcc produces valid ELF: ${ARCH_OUT}"
  else
    warn "riscv32-unknown-elf-gcc failed to compile a minimal rv32 test"
    FAIL=1
  fi
  rm -f /tmp/ppap_rv_check.c /tmp/ppap_rv_check.elf
else
  warn "riscv32-unknown-elf-gcc not found. Run setup_toolchain.sh to install."
  FAIL=1
fi

# OpenOCD (system — RP2040 only)
verify_version "openocd (system)" \
  "openocd --version" \
  "Open On-Chip Debugger"

# OpenOCD (Raspberry Pi fork — RP2350 support)
if [[ -x "${PPAP_ROOT}/tools/openocd-rp/bin/openocd" ]]; then
  verify_version "openocd (rp2350)" \
    "${PPAP_ROOT}/tools/openocd-rp/bin/openocd --version" \
    "Open On-Chip Debugger"
else
  warn "openocd-rp: not found (run ./third_party/build_openocd_rp.sh for RP2350 support)"
  FAIL=1
fi

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

# XEiJ
if [[ -f "${XEIJ_DIR}/XEiJ.jar" ]]; then
  success "XEiJ: installed at ${XEIJ_DIR}/XEiJ.jar"
else
  warn "XEiJ: not found at ${XEIJ_DIR}/XEiJ.jar"
  FAIL=1
fi

# Java
if command -v java &>/dev/null; then
  verify_version "java" \
    "java -version" \
    "version"
else
  warn "java: not found (required for XEiJ)"
  FAIL=1
fi

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
  warn "qemu-system-m68k: not found (install qemu-system-misc or run ./third_party/build_qemu_system_m68k.sh)"
  FAIL=1
fi

# Xtensa toolchain (xtensa-esp-elf-gcc in tools/xtensa-toolchain/)
XTENSA_GCC_PATTERN="${PPAP_ROOT}/tools/xtensa-toolchain/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-gcc"
XTENSA_GCC=$(compgen -G "${XTENSA_GCC_PATTERN}" | head -1 || true)
if [[ -n "${XTENSA_GCC}" && -x "${XTENSA_GCC}" ]]; then
  verify_version "xtensa-esp-elf-gcc" \
    "${XTENSA_GCC} --version" \
    "xtensa-esp.*-elf-gcc"
  # Check it produces valid Xtensa output with Call0 ABI
  echo 'int main(void){return 0;}' > /tmp/ppap_xtensa_check.c
  if "${XTENSA_GCC}" -mcpu=esp32s3 -mabi=call0 -nostdlib \
       -o /tmp/ppap_xtensa_check.elf /tmp/ppap_xtensa_check.c 2>/dev/null; then
    XTENSA_READELF="${XTENSA_GCC%gcc}readelf"
    ARCH_OUT=$("${XTENSA_READELF}" -h /tmp/ppap_xtensa_check.elf | grep Machine)
    success "xtensa-esp-elf-gcc produces valid ELF (Call0): ${ARCH_OUT}"
  else
    warn "xtensa-esp-elf-gcc failed to compile a minimal Call0 ABI test"
    FAIL=1
  fi
  rm -f /tmp/ppap_xtensa_check.c /tmp/ppap_xtensa_check.elf
else
  warn "xtensa-esp-elf-gcc: not found at tools/xtensa-toolchain/"
  FAIL=1
fi

# ESP-IDF (source, git submodule)
if [[ -f "${PPAP_ROOT}/third_party/esp-idf/export.sh" ]]; then
  success "ESP-IDF: present at ${PPAP_ROOT}/third_party/esp-idf"
else
  warn "ESP-IDF: not found at ${PPAP_ROOT}/third_party/esp-idf"
  FAIL=1
fi

# esptool.py (installed by ESP-IDF into tools/xtensa-toolchain/ venv)
XTENSA_TC_DIR="${PPAP_ROOT}/tools/xtensa-toolchain"
if [[ -f "${PPAP_ROOT}/third_party/esp-idf/export.sh" ]]; then
  # Source export.sh with IDF_TOOLS_PATH pointing to our local dir
  ESPTOOL_CHECK=$(bash -c "export IDF_TOOLS_PATH='${XTENSA_TC_DIR}'; source '${PPAP_ROOT}/third_party/esp-idf/export.sh' >/dev/null 2>&1 && command -v esptool.py" 2>/dev/null || true)
  if [[ -n "${ESPTOOL_CHECK}" ]]; then
    success "esptool.py: available (via ESP-IDF venv in tools/xtensa-toolchain/)"
  else
    warn "esptool.py: not found in ESP-IDF environment"
    FAIL=1
  fi
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

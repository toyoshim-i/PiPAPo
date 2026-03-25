#!/bin/bash
# Build musl libc for PiPAPo
#
# This script:
#   1. Resets the musl submodule to clean upstream state
#   2. Copies PPAP overlay files (syscall number remapping)
#   3. Deletes arch assembly files that need generic C fallbacks
#   4. Configures and builds musl as a static library
#   5. Installs headers + libc.a into the sysroot
#   6. Restores the submodule to its clean upstream state
#
# Normally invoked from cmake via user.cmake (PPAP_CONFIG set).
# Standalone: ./third_party/build_musl.sh [--m68k] [--clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MUSL_SRC="$SCRIPT_DIR/musl"
OVERLAY_DIR="$SCRIPT_DIR/patches/musl/overlay"

# --- Parse flags ---
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --m68k) PPAP_ARCH=m68k ;;
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Source cmake-generated config if available ---
if [[ -n "${PPAP_CONFIG:-}" && -f "$PPAP_CONFIG" ]]; then
    source "$PPAP_CONFIG"
elif [[ -z "${PPAP_ARCH:-}" ]]; then
    PPAP_ARCH=arm
fi

# --- Derive variables from PPAP_ARCH (fallback for standalone use) ---
if [[ -z "${PPAP_CC:-}" ]]; then
    if [[ "$PPAP_ARCH" == "m68k" ]]; then
        PPAP_CC="m68k-elf-gcc"
        PPAP_CROSS_PREFIX="m68k-elf-"
        PPAP_MUSL_TARGET=m68k-elf
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/m68k/musl-sysroot"
        PPAP_MUSL_CFLAGS="-m68000 -Os -g -msep-data -ffunction-sections -fdata-sections"
        PPAP_ARCH_LABEL="m68k (68000)"
    else
        PPAP_CC=arm-none-eabi-gcc
        PPAP_CROSS_PREFIX=arm-none-eabi-
        PPAP_MUSL_TARGET=arm-none-eabi
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/arm_m/musl-sysroot"
        PPAP_MUSL_CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -g -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections"
        PPAP_ARCH_LABEL="armv6m-thumb (Cortex-M0+)"
    fi
fi

# --- Handle --clean ---
if $CLEAN; then
    echo "musl [$PPAP_ARCH]: cleaning build artifacts..."
    cd "$MUSL_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$PPAP_MUSL_SYSROOT"
    echo "musl [$PPAP_ARCH]: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$PPAP_MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "musl [$PPAP_ARCH]: libc.a already exists at $PPAP_MUSL_SYSROOT/lib/libc.a — skipping."
    echo "musl [$PPAP_ARCH]: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v "$PPAP_CC" &>/dev/null; then
    echo "ERROR: $PPAP_CC not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$MUSL_SRC/configure" ]]; then
    echo "ERROR: musl submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/musl" >&2
    exit 1
fi

# --- Arch-specific assembly files to delete (need C fallbacks) ---
if [[ "$PPAP_ARCH" == "m68k" ]]; then
    DELETE_FILES=(
        src/setjmp/m68k/setjmp.s
        src/setjmp/m68k/longjmp.s
        src/signal/m68k/sigsetjmp.s
        src/thread/m68k/clone.s
        src/thread/m68k/syscall_cp.s
        src/thread/m68k/__m68k_read_tp.s
        src/ldso/m68k/dlsym.s
        src/ldso/m68k/dlsym_time64.S
    )
else
    DELETE_FILES=(
        src/setjmp/arm/setjmp.S
        src/setjmp/arm/longjmp.S
        src/signal/arm/sigsetjmp.s
        src/signal/arm/restore.s
        src/process/arm/vfork.s
        src/thread/arm/clone.s
        src/thread/arm/syscall_cp.s
        src/thread/arm/__unmapself.s
        src/string/arm/memcpy.S
        src/ldso/arm/tlsdesc.S
        src/ldso/arm/dlsym.s
        src/ldso/arm/dlsym_time64.S
    )
fi

# --- Restore submodule to clean state ---
echo "musl [$PPAP_ARCH]: resetting submodule to clean state..."
cd "$MUSL_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Copy overlay files (PPAP syscall number remapping) ---
echo "musl [$PPAP_ARCH]: applying PPAP overlay files..."
if [[ -d "$OVERLAY_DIR" ]]; then
    cp -rv "$OVERLAY_DIR"/. "$MUSL_SRC"/ 2>&1 | sed 's/^/  /'
else
    echo "  WARNING: overlay directory not found at $OVERLAY_DIR"
fi

# --- Delete arch assembly files that need C fallbacks ---
echo "musl [$PPAP_ARCH]: removing arch assembly files (C fallbacks available)..."
for f in "${DELETE_FILES[@]}"; do
    if [[ -f "$MUSL_SRC/$f" ]]; then
        rm -v "$MUSL_SRC/$f" | sed 's/^/  /'
    fi
done

# --- Configure ---
echo "musl [$PPAP_ARCH]: configuring for $PPAP_ARCH_LABEL..."
mkdir -p "$PPAP_MUSL_SYSROOT"

./configure \
    --target="$PPAP_MUSL_TARGET" \
    --prefix="$PPAP_MUSL_SYSROOT" \
    --disable-shared \
    --enable-static \
    CROSS_COMPILE="$PPAP_CROSS_PREFIX" \
    CC="$PPAP_CC" \
    CFLAGS="$PPAP_MUSL_CFLAGS"

# --- Build ---
echo "musl [$PPAP_ARCH]: building libc.a..."
make -j"$(nproc)"

# --- Install ---
echo "musl [$PPAP_ARCH]: installing to $PPAP_MUSL_SYSROOT..."
make install

# --- Restore submodule ---
echo "musl [$PPAP_ARCH]: restoring submodule to clean state..."
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify ---
if [[ -f "$PPAP_MUSL_SYSROOT/lib/libc.a" ]]; then
    SIZE=$(stat -c%s "$PPAP_MUSL_SYSROOT/lib/libc.a" 2>/dev/null || stat -f%z "$PPAP_MUSL_SYSROOT/lib/libc.a")
    echo "musl [$PPAP_ARCH]: SUCCESS — libc.a built ($SIZE bytes)"
    echo "  sysroot: $PPAP_MUSL_SYSROOT"
    echo "  headers: $PPAP_MUSL_SYSROOT/include/"
    echo "  library: $PPAP_MUSL_SYSROOT/lib/libc.a"
else
    echo "ERROR: libc.a not found after build" >&2
    exit 1
fi

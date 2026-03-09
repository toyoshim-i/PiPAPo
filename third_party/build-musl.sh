#!/bin/bash
# Build musl libc for PicoPiAndPortable
#
# This script:
#   1. Resets the musl submodule to clean upstream state
#   2. Copies PPAP overlay files (syscall number remapping)
#   3. Deletes arch assembly files that need generic C fallbacks
#   4. Configures and builds musl as a static library
#   5. Installs headers + libc.a into the sysroot
#   6. Restores the submodule to its clean upstream state
#
# Usage: ./third_party/build-musl.sh [--m68k] [--clean]
#   --m68k    Build for m68k (default: ARM Cortex-M0+)
#   --clean   Remove build artifacts and sysroot, then exit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MUSL_SRC="$SCRIPT_DIR/musl"
OVERLAY_DIR="$SCRIPT_DIR/patches/musl/overlay"

# --- Parse flags ---
ARCH=arm
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --m68k) ARCH=m68k ;;
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Arch-specific configuration ---
if [[ "$ARCH" == "m68k" ]]; then
    M68K_TC="$PROJECT_ROOT/tools/m68k-toolchain"
    MUSL_SYSROOT="$PROJECT_ROOT/build/m68k/musl-sysroot"
    CROSS_PREFIX="${M68K_TC}/bin/m68k-elf-"
    MUSL_TARGET=m68k-elf
    MUSL_CC="${M68K_TC}/bin/m68k-elf-gcc"
    MUSL_CFLAGS="-m68000 -Os -g -msep-data -ffunction-sections -fdata-sections"
    ARCH_LABEL="m68k (68000)"
    # m68k asm files to delete: setjmp uses FPU (fmovem), ldso not needed
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
    MUSL_SYSROOT="$PROJECT_ROOT/build/arm_m/musl-sysroot"
    CROSS_PREFIX=arm-none-eabi-
    MUSL_TARGET=arm-none-eabi
    MUSL_CC=arm-none-eabi-gcc
    MUSL_CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -g -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections"
    ARCH_LABEL="armv6m-thumb (Cortex-M0+)"
    # ARM asm files incompatible with Thumb-1
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

# --- Handle --clean ---
if $CLEAN; then
    echo "musl [$ARCH]: cleaning build artifacts..."
    cd "$MUSL_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$MUSL_SYSROOT"
    echo "musl [$ARCH]: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "musl [$ARCH]: libc.a already exists at $MUSL_SYSROOT/lib/libc.a — skipping."
    echo "musl [$ARCH]: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v "$MUSL_CC" &>/dev/null; then
    echo "ERROR: $MUSL_CC not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$MUSL_SRC/configure" ]]; then
    echo "ERROR: musl submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/musl" >&2
    exit 1
fi

# --- Restore submodule to clean state ---
echo "musl [$ARCH]: resetting submodule to clean state..."
cd "$MUSL_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Copy overlay files (PPAP syscall number remapping) ---
echo "musl [$ARCH]: applying PPAP overlay files..."
if [[ -d "$OVERLAY_DIR" ]]; then
    cp -rv "$OVERLAY_DIR"/. "$MUSL_SRC"/ 2>&1 | sed 's/^/  /'
else
    echo "  WARNING: overlay directory not found at $OVERLAY_DIR"
fi

# --- Delete arch assembly files that need C fallbacks ---
echo "musl [$ARCH]: removing arch assembly files (C fallbacks available)..."
for f in "${DELETE_FILES[@]}"; do
    if [[ -f "$MUSL_SRC/$f" ]]; then
        rm -v "$MUSL_SRC/$f" | sed 's/^/  /'
    fi
done

# --- Configure ---
echo "musl [$ARCH]: configuring for $ARCH_LABEL..."
mkdir -p "$MUSL_SYSROOT"

./configure \
    --target="$MUSL_TARGET" \
    --prefix="$MUSL_SYSROOT" \
    --disable-shared \
    --enable-static \
    CROSS_COMPILE="$CROSS_PREFIX" \
    CC="$MUSL_CC" \
    CFLAGS="$MUSL_CFLAGS"

# --- Build ---
echo "musl [$ARCH]: building libc.a..."
make -j"$(nproc)"

# --- Install ---
echo "musl [$ARCH]: installing to $MUSL_SYSROOT..."
make install

# --- Restore submodule ---
echo "musl [$ARCH]: restoring submodule to clean state..."
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify ---
if [[ -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    SIZE=$(stat -c%s "$MUSL_SYSROOT/lib/libc.a" 2>/dev/null || stat -f%z "$MUSL_SYSROOT/lib/libc.a")
    echo "musl [$ARCH]: SUCCESS — libc.a built ($SIZE bytes)"
    echo "  sysroot: $MUSL_SYSROOT"
    echo "  headers: $MUSL_SYSROOT/include/"
    echo "  library: $MUSL_SYSROOT/lib/libc.a"
else
    echo "ERROR: libc.a not found after build" >&2
    exit 1
fi

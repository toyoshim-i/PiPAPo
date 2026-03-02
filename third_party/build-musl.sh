#!/bin/bash
# Build musl libc for PicoPiAndPortable (ARMv6-M Thumb / Cortex-M0+)
#
# This script:
#   1. Resets the musl submodule to clean upstream state
#   2. Copies PPAP overlay files (Thumb-1 compatible replacements)
#   3. Deletes ARM assembly files that have generic C fallbacks
#   4. Configures and builds musl as a static library
#   5. Installs headers + libc.a into build/musl-sysroot/
#   6. Restores the submodule to its clean upstream state
#
# Usage: ./third_party/build-musl.sh [--clean]
#   --clean   Remove build artifacts and sysroot, then exit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MUSL_SRC="$SCRIPT_DIR/musl"
MUSL_SYSROOT="$PROJECT_ROOT/build/musl-sysroot"
OVERLAY_DIR="$SCRIPT_DIR/patches/musl/overlay"

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "musl: cleaning build artifacts..."
    cd "$MUSL_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$MUSL_SYSROOT"
    echo "musl: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "musl: libc.a already exists at $MUSL_SYSROOT/lib/libc.a — skipping."
    echo "musl: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v arm-none-eabi-gcc &>/dev/null; then
    echo "ERROR: arm-none-eabi-gcc not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$MUSL_SRC/configure" ]]; then
    echo "ERROR: musl submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/musl" >&2
    exit 1
fi

# --- Restore submodule to clean state ---
echo "musl: resetting submodule to clean state..."
cd "$MUSL_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Copy overlay files (Thumb-1 compatible replacements) ---
echo "musl: applying PPAP overlay files..."
if [[ -d "$OVERLAY_DIR" ]]; then
    # Copy overlay tree on top of musl source, preserving directory structure
    cp -rv "$OVERLAY_DIR"/. "$MUSL_SRC"/ 2>&1 | sed 's/^/  /'
else
    echo "  WARNING: overlay directory not found at $OVERLAY_DIR"
fi

# --- Delete ARM assembly files that have generic C fallbacks ---
# These use ARM-mode instructions (LDREX/STREX, conditional execution,
# STMFD/LDMFD, etc.) incompatible with Thumb-1.  Removing them causes
# musl's build system to use the generic C implementations instead.
echo "musl: removing ARM assembly files (C fallbacks available)..."
DELETE_FILES=(
    # setjmp/longjmp — generic C versions exist
    src/setjmp/arm/setjmp.S
    src/setjmp/arm/longjmp.S
    # signal — generic C versions exist
    src/signal/arm/sigsetjmp.s
    src/signal/arm/restore.s
    # process — generic C version exists
    src/process/arm/vfork.s
    # thread — generic C versions exist
    src/thread/arm/clone.s
    src/thread/arm/syscall_cp.s
    src/thread/arm/__unmapself.s
    # string — generic C version exists
    src/string/arm/memcpy.S
    # ldso — not needed (static linking only, no shared libraries)
    src/ldso/arm/tlsdesc.S
    src/ldso/arm/dlsym.s
    src/ldso/arm/dlsym_time64.S
)

for f in "${DELETE_FILES[@]}"; do
    if [[ -f "$MUSL_SRC/$f" ]]; then
        rm -v "$MUSL_SRC/$f" | sed 's/^/  /'
    fi
done

# --- Configure ---
echo "musl: configuring for armv6m-thumb (Cortex-M0+)..."
mkdir -p "$MUSL_SYSROOT"

./configure \
    --target=arm-none-eabi \
    --prefix="$MUSL_SYSROOT" \
    --disable-shared \
    --enable-static \
    CROSS_COMPILE=arm-none-eabi- \
    CC=arm-none-eabi-gcc \
    CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -g -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"

# --- Build ---
echo "musl: building libc.a..."
make -j"$(nproc)"

# --- Install ---
echo "musl: installing to $MUSL_SYSROOT..."
make install

# --- Restore submodule ---
echo "musl: restoring submodule to clean state..."
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify ---
if [[ -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    SIZE=$(stat -c%s "$MUSL_SYSROOT/lib/libc.a" 2>/dev/null || stat -f%z "$MUSL_SYSROOT/lib/libc.a")
    echo "musl: SUCCESS — libc.a built ($SIZE bytes)"
    echo "  sysroot: $MUSL_SYSROOT"
    echo "  headers: $MUSL_SYSROOT/include/"
    echo "  library: $MUSL_SYSROOT/lib/libc.a"
else
    echo "ERROR: libc.a not found after build" >&2
    exit 1
fi

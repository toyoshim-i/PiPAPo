#!/bin/bash
# Build m68k-elf GCC cross-compiler for PicoPiAndPortable
#
# This script downloads and builds:
#   1. binutils 2.43 for m68k-elf
#   2. GCC 14.2.0 (C only) for m68k-elf with --with-cpu=m68000
#
# The resulting toolchain produces 68000-safe code and libgcc,
# eliminating the need for manual divmod overrides.
#
# Usage: ./third_party/build-gcc-m68k.sh [--clean]
#   --clean   Remove toolchain and build artifacts, then exit
#
# Prerequisites:
#   - Standard build tools (make, gcc, g++, texinfo)
#   - GMP, MPFR, MPC development libraries (or they will be
#     downloaded automatically via GCC's download_prerequisites)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Versions
BINUTILS_VER=2.43
GCC_VER=14.2.0

# Directories
DL_DIR="$SCRIPT_DIR/dl"
BUILD_DIR="$PROJECT_ROOT/build/m68k-gcc-build"
PREFIX="$PROJECT_ROOT/tools/m68k-toolchain"
TARGET=m68k-elf

# Source URLs (GNU mirrors)
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

# --- Parse flags ---
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Handle --clean ---
if $CLEAN; then
    echo "gcc-m68k: this will remove the installed toolchain and build tree."
    echo "  toolchain: $PREFIX"
    echo "  build tree: $BUILD_DIR"
    echo "  (downloaded tarballs in $DL_DIR are always preserved)"
    read -r -p "Remove toolchain? [y/N] " answer
    case "$answer" in
        [yY]|[yY][eE][sS])
            echo "gcc-m68k: cleaning..."
            rm -rf "$BUILD_DIR" "$PREFIX"
            echo "gcc-m68k: clean done."
            ;;
        *)
            echo "gcc-m68k: clean skipped."
            ;;
    esac
    exit 0
fi

# --- Skip if already built ---
if [[ -x "$PREFIX/bin/${TARGET}-gcc" ]]; then
    echo "gcc-m68k: toolchain already exists at $PREFIX/bin/${TARGET}-gcc — skipping."
    echo "gcc-m68k: run '$0 --clean' to force rebuild."
    "$PREFIX/bin/${TARGET}-gcc" --version | head -1
    exit 0
fi

# --- Check host prerequisites ---
echo "gcc-m68k: checking host prerequisites..."
MISSING=()
for cmd in gcc g++ make makeinfo; do
    if ! command -v "$cmd" &>/dev/null; then
        MISSING+=("$cmd")
    fi
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "ERROR: missing host tools: ${MISSING[*]}" >&2
    echo "  Install with: sudo apt install build-essential texinfo" >&2
    exit 1
fi

# --- Download sources ---
mkdir -p "$DL_DIR"

download() {
    local url="$1"
    local dest="$DL_DIR/$(basename "$url")"
    if [[ -f "$dest" ]]; then
        echo "  cached: $(basename "$dest")"
    else
        echo "  downloading: $(basename "$url")..."
        wget -q --show-progress -O "$dest.tmp" "$url"
        mv "$dest.tmp" "$dest"
    fi
}

echo "gcc-m68k: downloading sources..."
download "$BINUTILS_URL"
download "$GCC_URL"

# --- Extract sources ---
mkdir -p "$BUILD_DIR"

extract() {
    local archive="$DL_DIR/$1"
    local dir="$BUILD_DIR/$2"
    if [[ -d "$dir" ]]; then
        echo "  already extracted: $2"
    else
        echo "  extracting: $1..."
        tar xf "$archive" -C "$BUILD_DIR"
    fi
}

echo "gcc-m68k: extracting sources..."
extract "binutils-${BINUTILS_VER}.tar.xz" "binutils-${BINUTILS_VER}"
extract "gcc-${GCC_VER}.tar.xz" "gcc-${GCC_VER}"

# --- Download GCC prerequisites (GMP, MPFR, MPC) ---
GCC_SRC="$BUILD_DIR/gcc-${GCC_VER}"
if [[ ! -d "$GCC_SRC/gmp" ]]; then
    echo "gcc-m68k: downloading GCC prerequisites (GMP, MPFR, MPC)..."
    cd "$GCC_SRC"
    ./contrib/download_prerequisites
    cd "$PROJECT_ROOT"
else
    echo "gcc-m68k: GCC prerequisites already present."
fi

# --- Build binutils ---
BINUTILS_BUILD="$BUILD_DIR/build-binutils"
if [[ -x "$PREFIX/bin/${TARGET}-as" ]]; then
    echo "gcc-m68k: binutils already installed — skipping."
else
    echo ""
    echo "========================================"
    echo "gcc-m68k: building binutils ${BINUTILS_VER}..."
    echo "========================================"
    mkdir -p "$BINUTILS_BUILD"
    cd "$BINUTILS_BUILD"
    "$BUILD_DIR/binutils-${BINUTILS_VER}/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --disable-werror \
        --disable-gdb \
        --disable-sim \
        --disable-libdecnumber \
        --disable-readline
    make -j"$(nproc)"
    make install
    echo "gcc-m68k: binutils installed."
fi

# --- Build GCC + libgcc ---
export PATH="$PREFIX/bin:$PATH"

GCC_BUILD="$BUILD_DIR/build-gcc"
echo ""
echo "========================================"
echo "gcc-m68k: building GCC ${GCC_VER} (C only, cpu=68000)..."
echo "========================================"
mkdir -p "$GCC_BUILD"
cd "$GCC_BUILD"
"$GCC_SRC/configure" \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --with-cpu=m68000 \
    --enable-languages=c \
    --without-headers \
    --disable-shared \
    --disable-threads \
    --disable-libssp \
    --disable-libquadmath \
    --disable-libgomp \
    --disable-libatomic \
    --disable-libstdcxx \
    --disable-nls \
    --disable-multilib \
    --with-newlib
make -j"$(nproc)" all-gcc all-target-libgcc
make install-gcc install-target-libgcc
echo "gcc-m68k: GCC + libgcc installed."

# --- Verify ---
echo ""
echo "========================================"
echo "gcc-m68k: verification"
echo "========================================"

# Check compiler works
echo "  compiler version:"
"$PREFIX/bin/${TARGET}-gcc" --version | head -1 | sed 's/^/    /'

# Check libgcc location
LIBGCC=$("$PREFIX/bin/${TARGET}-gcc" -m68000 -print-libgcc-file-name)
echo "  libgcc: $LIBGCC"

# Verify no 68020+ instructions in libgcc
echo "  checking libgcc for 68020+ instructions..."
BAD_INSNS=$("$PREFIX/bin/${TARGET}-objdump" -d "$LIBGCC" | \
    grep -ciE '\bbsr\.l\b|\bdivu\.l\b|\bdivs\.l\b|\bmulu\.l\b|\bmuls\.l\b|\bextb\.l\b' || true)
if [[ "$BAD_INSNS" -eq 0 ]]; then
    echo "    OK: no 68020+ instructions found"
else
    echo "    WARNING: found $BAD_INSNS lines with 68020+ instructions" >&2
    echo "    (this may indicate a build configuration issue)" >&2
fi

# List installed tools
echo "  installed tools:"
for tool in gcc as ld ar objcopy objdump size strip ranlib; do
    if [[ -x "$PREFIX/bin/${TARGET}-${tool}" ]]; then
        echo "    ${TARGET}-${tool}"
    fi
done

echo ""
echo "gcc-m68k: SUCCESS — toolchain installed to $PREFIX"
echo "  Add to PATH: export PATH=\"$PREFIX/bin:\$PATH\""

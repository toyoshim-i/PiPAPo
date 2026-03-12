#!/usr/bin/env bash
# build.sh — Build a PPAP target
#
# Usage:
#   ./scripts/build.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc, qemu_arm, qemu_m68k
#
# Options:
#   --test              Enable PPAP_TESTS (kernel + userland test suite)
#   --test-extended     Enable PPAP_TESTS + PPAP_TESTS_EXTENDED
#   --clean             Remove build directory before building (full rebuild)
#   --overlay=<dir>     Extra overlay directory copied into romfs (highest priority)
#   --h68k-debug        Enable kernel Human68k debug diagnostics
#
# Examples:
#   ./scripts/build.sh pico1              # build pico1
#   ./scripts/build.sh pico1calc          # build pico1calc
#   ./scripts/build.sh --test qemu_arm    # build qemu_arm with tests
#   ./scripts/build.sh --test-extended qemu_arm  # build qemu_arm with extended tests
#   ./scripts/build.sh --clean qemu_m68k  # clean rebuild m68k
#   ./scripts/build.sh qemu_m68k          # build m68k QEMU target
#   ./scripts/build.sh --overlay=~/my_x68k qemu_m68k  # add custom files to romfs

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse arguments ──────────────────────────────────────────────────────────
TESTS=OFF
TESTS_EXTENDED=OFF
CLEAN=0
OVERLAY=""
H68K_DEBUG=OFF
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --test)       TESTS=ON ;;
        --test-extended) TESTS=ON; TESTS_EXTENDED=ON ;;
        --clean)      CLEAN=1 ;;
        --overlay=*)  OVERLAY="${arg#--overlay=}" ;;
        --h68k-debug) H68K_DEBUG=ON ;;
        -*)           echo "Unknown option: $arg" >&2; exit 1 ;;
        *)            TARGET="$arg" ;;
    esac
done

# Show usage if no target specified
if [[ -z "$TARGET" ]]; then
    sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
    exit 0
fi

# ── Determine source and build directories ──────────────────────────────────
case "$TARGET" in
    qemu_arm|pico1|pico1calc)
        SOURCE_DIR="$PROJECT_DIR/src/target/$TARGET"
        BUILD_DIR="$PROJECT_DIR/build/$TARGET"
        ;;
    qemu_m68k)
        SOURCE_DIR="$PROJECT_DIR/src/target/qemu_m68k"
        BUILD_DIR="$PROJECT_DIR/build/qemu_m68k"
        ;;
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, qemu_arm, qemu_m68k"
        exit 1
        ;;
esac

CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Clean build directory if requested ───────────────────────────────────────
if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "[build] Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# ── Build ────────────────────────────────────────────────────────────────────
EXTRA_ARGS=(-DPPAP_EXTRA_OVERLAY=)
case "$TARGET" in
    qemu_arm)
        # Bare-metal ARM (no Pico SDK) — needs explicit toolchain file
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_arm_m.cmake")
        ;;
    qemu_m68k)
        # Ensure custom m68k-elf toolchain is available
        M68K_TC="$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-gcc"
        if [[ ! -x "$M68K_TC" ]]; then
            echo "[build] m68k-elf-gcc not found. Building toolchain..."
            "$PROJECT_DIR/third_party/build_gcc_m68k.sh"
        fi
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_m68k.cmake")
        ;;
esac

if [[ -n "$OVERLAY" ]]; then
    # Resolve to absolute path
    OVERLAY="$(cd "$OVERLAY" 2>/dev/null && pwd)" || {
        echo "[build] Error: overlay directory '$OVERLAY' not found" >&2
        exit 1
    }
    EXTRA_ARGS[0]="-DPPAP_EXTRA_OVERLAY=$OVERLAY"
fi

echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS, PPAP_TESTS_EXTENDED=$TESTS_EXTENDED)..."
cmake "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
      -DPPAP_TESTS="$TESTS" \
      -DPPAP_TESTS_EXTENDED="$TESTS_EXTENDED" \
      -DH68K_DEBUG="$H68K_DEBUG" \
      -S "$SOURCE_DIR" -B "$BUILD_DIR" >/dev/null 2>&1
cmake --build "$BUILD_DIR" -- -j"$(nproc)"

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

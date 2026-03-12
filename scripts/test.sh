#!/usr/bin/env bash
# test.sh — Run PiPAPo tests
#
# Usage:
#   ./scripts/test.sh [OPTIONS]
#
# Options:
#   --verbose   Show all test output, not just failures (host unit tests)
#   --all       Run everything: host unit tests, build all targets, QEMU tests
#   --extended  With --all, run extended QEMU test lanes
#   --all-extended  Same as --all --extended
#
# Examples:
#   ./scripts/test.sh            # host unit tests only
#   ./scripts/test.sh --verbose  # host unit tests with verbose output
#   ./scripts/test.sh --all      # host + build all targets + QEMU tests
#   ./scripts/test.sh --all --extended  # same, but runs extended QEMU lanes
#   ./scripts/test.sh --all-extended    # shorthand for --all --extended

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse arguments ────────────────────────────────────────────────────────
DO_ALL=0
VERBOSE=0
USE_EXTENDED=0

for arg in "$@"; do
    case "$arg" in
        --all)          DO_ALL=1 ;;
        --extended)     USE_EXTENDED=1 ;;
        --all-extended) DO_ALL=1; USE_EXTENDED=1 ;;
        --verbose)      VERBOSE=1 ;;
        -*)        echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [[ $USE_EXTENDED -eq 1 && $DO_ALL -eq 0 ]]; then
    echo "--extended requires --all (or use --all-extended)" >&2
    exit 1
fi

# ── Host unit tests ────────────────────────────────────────────────────────
BUILD_DIR="$PROJECT_DIR/build/host"

echo "=== Host unit tests ==="
cmake -S "$PROJECT_DIR/tests/host" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    > /dev/null

cmake --build "$BUILD_DIR" -- -j"$(nproc)"
echo ""

if [[ $VERBOSE -eq 1 ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure -V
else
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

if [[ $DO_ALL -eq 0 ]]; then
    exit 0
fi

# ── Build all production targets ───────────────────────────────────────────
echo ""
echo "=== Building all ARM targets (production) ==="
"$SCRIPT_DIR/build.sh" qemu_arm
"$SCRIPT_DIR/build.sh" pico1
"$SCRIPT_DIR/build.sh" pico1calc

echo ""
echo "=== ARM binary sizes (production) ==="
arm-none-eabi-size build/qemu_arm/ppap_qemu_arm.elf \
                   build/pico1/ppap_pico1.elf \
                   build/pico1calc/ppap_pico1calc.elf 2>/dev/null || true

echo ""
echo "=== Building m68k target (production) ==="
"$SCRIPT_DIR/build.sh" qemu_m68k

echo ""
echo "=== m68k binary size (production) ==="
"$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-size" \
    build/qemu_m68k/ppap_qemu_m68k.elf 2>/dev/null || true

# ── QEMU on-target tests ──────────────────────────────────────────────────
echo ""
if [[ $USE_EXTENDED -eq 1 ]]; then
    echo "=== QEMU automated test (ARM, extended) ==="
    "$SCRIPT_DIR/run.sh" --test-extended qemu_arm
else
    echo "=== QEMU automated test (ARM) ==="
    "$SCRIPT_DIR/run.sh" --test qemu_arm
fi

echo ""
if [[ $USE_EXTENDED -eq 1 ]]; then
    echo "=== QEMU automated test (m68k, extended) ==="
    "$SCRIPT_DIR/run.sh" --test-extended qemu_m68k
else
    echo "=== QEMU automated test (m68k) ==="
    "$SCRIPT_DIR/run.sh" --test qemu_m68k
fi

echo ""
echo "=== All tests passed ==="

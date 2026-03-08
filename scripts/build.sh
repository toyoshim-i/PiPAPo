#!/usr/bin/env bash
# build.sh — Build a PPAP target
#
# Usage:
#   ./scripts/build.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc, qemu_arm, qemu_m68k
#
# Options:
#   --test    Enable PPAP_TESTS (kernel integration tests + userland test suite)
#
# Examples:
#   ./scripts/build.sh pico1              # build pico1
#   ./scripts/build.sh pico1calc          # build pico1calc
#   ./scripts/build.sh --test qemu_arm    # build qemu_arm with tests
#   ./scripts/build.sh qemu_m68k          # build m68k QEMU target

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse arguments ──────────────────────────────────────────────────────────
TESTS=OFF
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --test)   TESTS=ON ;;
        -*)       echo "Unknown option: $arg" >&2; exit 1 ;;
        *)        TARGET="$arg" ;;
    esac
done

# Show usage if no target specified
if [[ -z "$TARGET" ]]; then
    sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
    exit 0
fi

# ── Target-specific build ────────────────────────────────────────────────────
case "$TARGET" in
    pico1|pico1calc|qemu_arm)
        BUILD_DIR="$PROJECT_DIR/build/arm_m"
        CMAKE_TARGET="ppap_${TARGET}"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

        echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS)..."
        cmake -B "$BUILD_DIR" -DPPAP_TESTS="$TESTS" "$PROJECT_DIR" >/dev/null 2>&1
        cmake --build "$BUILD_DIR" --target "$CMAKE_TARGET" -- -j"$(nproc)"
        ;;
    qemu_m68k)
        BUILD_DIR="$PROJECT_DIR/build/m68k"
        CMAKE_TARGET="ppap_qemu_m68k"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

        echo "[build] Building $CMAKE_TARGET..."
        cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain-m68k.cmake" \
              -S "$PROJECT_DIR/src/target/qemu_m68k" -B "$BUILD_DIR" >/dev/null 2>&1
        cmake --build "$BUILD_DIR" -- -j"$(nproc)"
        ;;
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, qemu_arm, qemu_m68k"
        exit 1
        ;;
esac

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

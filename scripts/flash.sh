#!/usr/bin/env bash
# flash.sh — Build and flash a PPAP target to the RP2040 via OpenOCD
#
# Usage:
#   ./scripts/flash.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc, qemu_arm
#
# Options:
#   --build   Build only (skip flash)
#   --test    Enable PPAP_TESTS (kernel integration tests + userland test suite)
#
# Examples:
#   ./scripts/flash.sh pico1              # build & flash pico1
#   ./scripts/flash.sh pico1calc          # build & flash pico1calc
#   ./scripts/flash.sh --test pico1       # build & flash pico1 with tests
#   ./scripts/flash.sh --build pico1      # build only, no flash
#   ./scripts/flash.sh --build qemu_arm   # build qemu_arm ELF
#
# Alternatively, without a debug adapter, hold BOOTSEL, plug in USB, then:
#   cp build/src/target/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
#
# Requirements:
#   - openocd in PATH (v0.12 or later)
#   - Picoprobe (or any CMSIS-DAP adapter) wired to the target Pico
#   - openocd.cfg present in the project root

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
CFG="$PROJECT_DIR/openocd.cfg"

# ── Parse arguments ──────────────────────────────────────────────────────────
TESTS=OFF
BUILD_ONLY=false
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --test)   TESTS=ON ;;
        --build)  BUILD_ONLY=true ;;
        -*)       echo "Unknown option: $arg" >&2; exit 1 ;;
        *)        TARGET="$arg" ;;
    esac
done

# Show usage if no target specified
if [[ -z "$TARGET" ]]; then
    sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
    exit 0
fi

# Validate target name
case "$TARGET" in
    pico1|pico1calc|qemu_arm) ;;
    *)
        echo "[flash] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, qemu_arm"
        exit 1
        ;;
esac

CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Build ────────────────────────────────────────────────────────────────────
echo "[flash] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS)..."
cmake -B "$BUILD_DIR" -DPPAP_TESTS="$TESTS" "$PROJECT_DIR" >/dev/null 2>&1
cmake --build "$BUILD_DIR" --target "$CMAKE_TARGET" -- -j"$(nproc)"

if [[ ! -f "$ELF" ]]; then
    echo "[flash] Error: $ELF not found after build."
    exit 1
fi

# ── Skip flash if --build or qemu_arm ──────────────────────────────────────────
if $BUILD_ONLY; then
    echo "[flash] Built $ELF"
    exit 0
fi

if [[ "$TARGET" == "qemu_arm" ]]; then
    echo "[flash] Built $ELF (qemu_arm — use qemu-system-arm to run)"
    exit 0
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if ! command -v openocd &>/dev/null; then
    echo "[flash] Error: openocd not found in PATH."
    echo "        Install with: sudo apt install openocd"
    exit 1
fi

# ── Stop any running OpenOCD (holds the adapter exclusively) ──────────────────
if pgrep -x openocd &>/dev/null; then
    echo "[flash] Stopping existing OpenOCD instance..."
    pkill -x openocd
    sleep 0.5
fi

# ── Flash ─────────────────────────────────────────────────────────────────────
echo "[flash] Flashing $ELF ..."
openocd \
    -f "$CFG" \
    -c "program \"$ELF\" verify reset exit"

echo "[flash] Done."

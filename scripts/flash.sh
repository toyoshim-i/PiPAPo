#!/usr/bin/env bash
# flash.sh — Flash a PPAP target to the RP2040 via OpenOCD
#
# Usage:
#   ./scripts/flash.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc
#
# Options:
#   --build   Build before flashing (calls scripts/build.sh)
#   --test    Enable PPAP_TESTS (passed to build.sh, implies --build)
#
# Examples:
#   ./scripts/flash.sh pico1              # flash pico1 (must be pre-built)
#   ./scripts/flash.sh --build pico1calc  # build & flash pico1calc
#   ./scripts/flash.sh --test pico1       # build with tests & flash pico1
#
# Alternatively, without a debug adapter, hold BOOTSEL, plug in USB, then:
#   cp build/arm_m/src/target/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
#
# Requirements:
#   - openocd in PATH (v0.12 or later)
#   - Picoprobe (or any CMSIS-DAP adapter) wired to the target Pico
#   - scripts/debug/openocd.cfg present in the project

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build/arm_m"
CFG="$SCRIPT_DIR/debug/openocd.cfg"

# ── Parse arguments ──────────────────────────────────────────────────────────
DO_BUILD=false
BUILD_ARGS=()
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --build)  DO_BUILD=true ;;
        --test)   DO_BUILD=true; BUILD_ARGS+=(--test) ;;
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
    pico1|pico1calc) ;;
    *)
        echo "[flash] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc"
        exit 1
        ;;
esac

CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Optional build ────────────────────────────────────────────────────────────
if $DO_BUILD; then
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}" "$TARGET"
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo "[flash] Error: $ELF not found."
    echo "        Run: ./scripts/build.sh $TARGET"
    exit 1
fi

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

#!/usr/bin/env bash
# flash.sh — Flash ppap_pico1calc.elf to the RP2040 via OpenOCD
#
# Usage (from project root):
#   ./scripts/flash.sh                     # flash build/ppap_pico1calc.elf
#   ./scripts/flash.sh --build             # rebuild first, then flash
#   ./scripts/flash.sh build/ppap_pico1.elf  # flash a specific ELF
#
# Alternatively, without a debug adapter, hold BOOTSEL, plug in USB, then:
#   cp build/src/target/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
#
# Requirements:
#   - openocd in PATH (v0.12 or later)
#   - Picoprobe (or any CMSIS-DAP adapter) wired to the target Pico
#   - openocd.cfg present in the project root
#   - ELF already built (or use --build)
#
# For daily development: use this script (OpenOCD keeps the adapter alive;
# you can re-flash without unplugging anything).
# For release: use the .uf2 file (no adapter required).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ELF="$PROJECT_DIR/build/ppap_pico1calc.elf"
CFG="$PROJECT_DIR/openocd.cfg"

# ── Optional rebuild ──────────────────────────────────────────────────────────
if [[ "${1:-}" == "--build" ]]; then
    echo "[flash] Building ppap_pico1calc..."
    cmake --build "$PROJECT_DIR/build" --target ppap_pico1calc
    shift
fi

# ── Optional ELF path override ───────────────────────────────────────────────
if [[ -n "${1:-}" && -f "${1:-}" ]]; then
    ELF="$1"
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo "[flash] Error: $ELF not found."
    echo "        Run 'cmake --build build' or use --build flag."
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
    sleep 0.5   # give the adapter a moment to release
fi

# ── Flash ─────────────────────────────────────────────────────────────────────
# 'program <elf> verify reset exit':
#   - Loads all ELF segments into flash (addresses embedded in the ELF)
#   - Verifies the written data by re-reading and comparing
#   - Resets the target so it starts running immediately
#   - Exits OpenOCD (no server left running)
echo "[flash] Flashing $ELF ..."
openocd \
    -f "$CFG" \
    -c "program \"$ELF\" verify reset exit"

echo "[flash] Done."

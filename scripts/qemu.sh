#!/usr/bin/env bash
# qemu.sh — Run ppap_qemu_arm under QEMU mps2-an500 (Cortex-M0+)
#
# Usage (from project root):
#   ./scripts/qemu.sh            # run build/ppap_qemu_arm.elf
#   ./scripts/qemu.sh --build    # rebuild first, then run
#   ./scripts/qemu.sh --gdb      # pause at reset, wait for GDB on :1234
#
# Install QEMU if not present:
#   sudo apt install qemu-system-arm
#
# GDB session (in a second terminal):
#   gdb-multiarch -ex "target remote :1234" build/ppap_qemu_arm.elf
#
# Expected output (boot header followed by interleaved "0" / "1" indefinitely):
#   PicoPiAndPortable booting (QEMU mps2-an500)...
#   UART: CMSDK UART0 @ 0x40004000
#   Clock: emulated (no PLL — skipping clock_init_pll)
#   MM: ...
#   PROC: ...
#   XIP: xip_add @ 0x000xxxxx (ROM, 0x000xxxxx in QEMU)
#   XIP: xip_add(3,4) = 7
#   SCHED: starting cooperative context-switch test (QEMU)
#   0
#   1
#   0
#   1
#   ...  (interleaved output proves PendSV context switch works)
#
# Press Ctrl-A X to quit QEMU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ELF="$PROJECT_DIR/build/ppap_qemu_arm.elf"

# ── Optional rebuild ──────────────────────────────────────────────────────────
if [[ "${1:-}" == "--build" ]]; then
    echo "[qemu] Building ppap_qemu_arm..."
    cmake --build "$PROJECT_DIR/build" --target ppap_qemu_arm
    shift
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if ! command -v qemu-system-arm &>/dev/null; then
    echo "[qemu] Error: qemu-system-arm not found."
    echo "       Install with: sudo apt install qemu-system-arm"
    exit 1
fi

if [[ ! -f "$ELF" ]]; then
    echo "[qemu] Error: $ELF not found."
    echo "       Run: cmake --build build --target ppap_qemu_arm"
    exit 1
fi

# ── GDB stub option ───────────────────────────────────────────────────────────
GDB_ARGS=()
if [[ "${1:-}" == "--gdb" ]]; then
    GDB_ARGS=(-s -S)   # -s = GDB server on :1234, -S = pause at reset
    echo "[qemu] Waiting for GDB on :1234 ..."
    echo "       Connect with: gdb-multiarch -ex 'target remote :1234' $ELF"
fi

# ── Run ───────────────────────────────────────────────────────────────────────
# -M mps2-an500     ARM MPS2 board with Cortex-M0+ (same ISA as RP2040)
# -nographic        disable GUI window; redirects QEMU monitor to stdio
# -serial mon:stdio multiplex UART + QEMU monitor on stdio (Ctrl-A X to quit)
#                   (-serial stdio alone conflicts with -nographic on QEMU 8+)
echo "[qemu] Running $ELF ..."
qemu-system-arm \
    -M mps2-an500 \
    -nographic \
    -serial mon:stdio \
    -kernel "$ELF" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

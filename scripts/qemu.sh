#!/usr/bin/env bash
# qemu.sh — Run PPAP under QEMU
#
# Usage (from project root):
#   ./scripts/qemu.sh                # build (if needed) & run ARM target
#   ./scripts/qemu.sh --m68k         # build (if needed) & run M68K target
#   ./scripts/qemu.sh --no-build     # skip build, run existing binary
#   ./scripts/qemu.sh --gdb          # pause at reset, wait for GDB on :1234
#   ./scripts/qemu.sh --m68k --gdb   # M68K with GDB stub
#
# Flags can be combined in any order.
#
# Press Ctrl-A X to quit QEMU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse flags ───────────────────────────────────────────────────────────────
TARGET="arm"
DO_BUILD=1
DO_GDB=0
for arg in "$@"; do
    case "$arg" in
        --m68k)     TARGET="m68k" ;;
        --no-build) DO_BUILD=0 ;;
        --build)    DO_BUILD=1 ;;  # kept for back-compat (now default)
        --gdb)      DO_GDB=1 ;;
        *)          echo "Unknown option: $arg"; echo "Usage: $0 [--m68k] [--no-build] [--gdb]"; exit 1 ;;
    esac
done

# ── Target-specific configuration ────────────────────────────────────────────
if [[ "$TARGET" == "m68k" ]]; then
    ELF="$PROJECT_DIR/build/m68k/ppap_qemu_m68k.elf"
    BUILD_TARGET="qemu_m68k"
    QEMU_BIN="qemu-system-m68k"
    # Prefer locally-built QEMU if available
    LOCAL_QEMU="$PROJECT_DIR/third_party/qemu/build/qemu-system-m68k"
    if [[ -x "$LOCAL_QEMU" ]]; then
        QEMU_BIN="$LOCAL_QEMU"
    fi
    QEMU_ARGS=(-machine virt -cpu m68000)
else
    ELF="$PROJECT_DIR/build/arm_m/ppap_qemu_arm.elf"
    BUILD_TARGET="qemu_arm"
    QEMU_BIN="qemu-system-arm"
    QEMU_ARGS=(-M mps2-an500 -serial mon:stdio)
fi

# ── Build ────────────────────────────────────────────────────────────────────
if [[ $DO_BUILD -eq 1 ]]; then
    "$SCRIPT_DIR/build.sh" "$BUILD_TARGET"
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if ! command -v "$QEMU_BIN" &>/dev/null && [[ ! -x "$QEMU_BIN" ]]; then
    echo "[qemu] Error: $QEMU_BIN not found."
    if [[ "$TARGET" == "m68k" ]]; then
        echo "       Install with: sudo apt install qemu-system-misc"
        echo "       Or build from source: ./scripts/build-qemu.sh"
    else
        echo "       Install with: sudo apt install qemu-system-arm"
    fi
    exit 1
fi

if [[ ! -f "$ELF" ]]; then
    echo "[qemu] Error: $ELF not found."
    echo "       Run without --no-build to build first."
    exit 1
fi

# ── GDB stub option ───────────────────────────────────────────────────────────
GDB_ARGS=()
if [[ $DO_GDB -eq 1 ]]; then
    GDB_ARGS=(-s -S)   # -s = GDB server on :1234, -S = pause at reset
    echo "[qemu] Waiting for GDB on :1234 ..."
    echo "       Connect with: gdb-multiarch -ex 'target remote :1234' $ELF"
fi

# ── Run ───────────────────────────────────────────────────────────────────────
echo "[qemu] Running $ELF ..."
"$QEMU_BIN" \
    "${QEMU_ARGS[@]}" \
    -nographic \
    -kernel "$ELF" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

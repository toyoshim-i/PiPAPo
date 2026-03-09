#!/usr/bin/env bash
# qemu.sh — Run PPAP under QEMU
#
# Usage (from project root):
#   ./scripts/qemu.sh [OPTIONS] [TARGET]
#
# TARGET is one of: qemu_arm (default), qemu_m68k
#
# Options:
#   --build     Force rebuild before running (default: build if needed)
#   --no-build  Skip build, run existing binary
#   --test      Enable PPAP_TESTS, run automated test suite (implies --build)
#   --clean     Clean build directory before building (passed to build.sh)
#   --gdb       Pause at reset, wait for GDB on :1234
#   --m68k      Shorthand for TARGET=qemu_m68k (back-compat)
#
# Examples:
#   ./scripts/qemu.sh                     # build & run ARM interactively
#   ./scripts/qemu.sh qemu_m68k           # build & run m68k interactively
#   ./scripts/qemu.sh --test              # build ARM with tests, run & check
#   ./scripts/qemu.sh --test qemu_m68k    # build m68k with tests, run & check
#   ./scripts/qemu.sh --no-build --gdb    # run existing ARM binary under GDB
#
# Press Ctrl-A X to quit QEMU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse flags ───────────────────────────────────────────────────────────────
TARGET=""
DO_BUILD=1
DO_TEST=0
DO_CLEAN=0
DO_GDB=0
for arg in "$@"; do
    case "$arg" in
        --m68k)     TARGET="qemu_m68k" ;;
        --no-build) DO_BUILD=0 ;;
        --build)    DO_BUILD=1 ;;
        --test)     DO_TEST=1; DO_BUILD=1 ;;
        --clean)    DO_CLEAN=1; DO_BUILD=1 ;;
        --gdb)      DO_GDB=1 ;;
        qemu_arm|qemu_m68k) TARGET="$arg" ;;
        *)          echo "Unknown option: $arg"
                    echo "Usage: $0 [--test] [--clean] [--no-build] [--gdb] [qemu_arm|qemu_m68k]"
                    exit 1 ;;
    esac
done

# Default target
if [[ -z "$TARGET" ]]; then
    TARGET="qemu_arm"
fi

# ── Target-specific configuration ────────────────────────────────────────────
TIMEOUT=30
if [[ "$TARGET" == "qemu_m68k" ]]; then
    ELF="$PROJECT_DIR/build/m68k/ppap_qemu_m68k.elf"
    QEMU_BIN="qemu-system-m68k"
    # Prefer locally-built QEMU if available
    LOCAL_QEMU="$PROJECT_DIR/third_party/qemu/build/qemu-system-m68k"
    if [[ -x "$LOCAL_QEMU" ]]; then
        QEMU_BIN="$LOCAL_QEMU"
    fi
    QEMU_ARGS=(-machine virt -cpu m68000)
else
    ELF="$PROJECT_DIR/build/arm_m/ppap_qemu_arm.elf"
    QEMU_BIN="qemu-system-arm"
    QEMU_ARGS=(-M mps2-an500 -serial mon:stdio)
fi

# ── Build ────────────────────────────────────────────────────────────────────
if [[ $DO_BUILD -eq 1 ]]; then
    BUILD_ARGS=()
    if [[ $DO_CLEAN -eq 1 ]]; then
        BUILD_ARGS+=(--clean)
    fi
    if [[ $DO_TEST -eq 1 ]]; then
        BUILD_ARGS+=(--test)
    fi
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}" "$TARGET"
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if ! command -v "$QEMU_BIN" &>/dev/null && [[ ! -x "$QEMU_BIN" ]]; then
    echo "[qemu] Error: $QEMU_BIN not found."
    if [[ "$TARGET" == "qemu_m68k" ]]; then
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

# ── Test mode: run with timeout and check output ─────────────────────────────
if [[ $DO_TEST -eq 1 ]]; then
    echo "[test] Running on-target tests (timeout ${TIMEOUT}s)..."
    OUTPUT=$(timeout "$TIMEOUT" "$QEMU_BIN" \
        "${QEMU_ARGS[@]}" \
        -nographic \
        -kernel "$ELF" 2>&1 || true)

    echo "$OUTPUT"

    if echo "$OUTPUT" | grep -q "ALL.*TESTS PASSED"; then
        echo ""
        echo "[test] PASS — all on-target tests passed"
        exit 0
    else
        echo ""
        echo "[test] FAIL — tests did not all pass (or QEMU timed out)"
        exit 1
    fi
fi

# ── GDB stub option ───────────────────────────────────────────────────────────
GDB_ARGS=()
if [[ $DO_GDB -eq 1 ]]; then
    GDB_ARGS=(-s -S)   # -s = GDB server on :1234, -S = pause at reset
    echo "[qemu] Waiting for GDB on :1234 ..."
    echo "       Connect with: gdb-multiarch -ex 'target remote :1234' $ELF"
fi

# ── Run interactively ────────────────────────────────────────────────────────
echo "[qemu] Running $ELF ..."
"$QEMU_BIN" \
    "${QEMU_ARGS[@]}" \
    -nographic \
    -kernel "$ELF" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

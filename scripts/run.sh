#!/usr/bin/env bash
# run.sh — Build, flash, or run a PiPAPo target
#
# Usage:
#   ./scripts/run.sh [OPTIONS] [TARGET]
#
# TARGET is one of:
#   pico1, pico1calc   — Flash to RP2040 via OpenOCD
#   qemu_arm (default) — Run under QEMU ARM
#   qemu_m68k          — Run under QEMU m68k
#
# Options:
#   --build     Build before running
#   --no-build  Skip build, use existing binary (default)
#   --test      Enable PPAP_TESTS, run automated test suite (implies --build)
#   --clean     Clean build directory before building (implies --build)
#   --gdb       (QEMU only) Pause at reset, wait for GDB on :1234
#   --m68k      Shorthand for TARGET=qemu_m68k (back-compat)
#
# Examples:
#   ./scripts/run.sh                        # run qemu_arm (must be pre-built)
#   ./scripts/run.sh --build                # build & run qemu_arm
#   ./scripts/run.sh --build qemu_m68k      # build & run m68k
#   ./scripts/run.sh --test                 # build ARM with tests, run & check
#   ./scripts/run.sh --test qemu_m68k       # build m68k with tests, run & check
#   ./scripts/run.sh --gdb                  # run existing ARM binary under GDB
#   ./scripts/run.sh pico1calc              # flash pre-built pico1calc via OpenOCD
#   ./scripts/run.sh --build pico1calc      # build & flash pico1calc
#   ./scripts/run.sh --test pico1           # build with tests & flash pico1
#
# Alternatively, without a debug adapter, hold BOOTSEL, plug in USB, then:
#   cp build/arm_m/src/target/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
#
# Press Ctrl-A X to quit QEMU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse flags ─────────────────────────────────────────────────────────────
TARGET=""
DO_BUILD=0
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
        pico1|pico1calc|qemu_arm|qemu_m68k) TARGET="$arg" ;;
        -*)         echo "Unknown option: $arg" >&2; exit 1 ;;
        *)
            echo "Unknown target: $arg" >&2
            echo "Valid targets: pico1, pico1calc, qemu_arm, qemu_m68k" >&2
            exit 1
            ;;
    esac
done

# Default target
if [[ -z "$TARGET" ]]; then
    TARGET="qemu_arm"
fi

# ── Resolve ELF path ───────────────────────────────────────────────────────
case "$TARGET" in
    pico1|pico1calc|qemu_arm)
        BUILD_DIR="$PROJECT_DIR/build/arm_m"
        CMAKE_TARGET="ppap_${TARGET}"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"
        ;;
    qemu_m68k)
        BUILD_DIR="$PROJECT_DIR/build/m68k"
        CMAKE_TARGET="ppap_qemu_m68k"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"
        ;;
esac

# ── Build ───────────────────────────────────────────────────────────────────
if [[ $DO_BUILD -eq 1 ]]; then
    BUILD_ARGS=()
    if [[ $DO_CLEAN -eq 1 ]]; then BUILD_ARGS+=(--clean); fi
    if [[ $DO_TEST -eq 1 ]]; then BUILD_ARGS+=(--test); fi
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}" "$TARGET"
fi

# ── Pre-flight: ELF must exist ──────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo "[run] Error: $ELF not found."
    echo "      Run: ./scripts/build.sh $TARGET"
    exit 1
fi

# ── Flash targets (pico1, pico1calc) ────────────────────────────────────────
if [[ "$TARGET" == pico1 || "$TARGET" == pico1calc ]]; then
    CFG="$SCRIPT_DIR/debug/openocd.cfg"

    if ! command -v openocd &>/dev/null; then
        echo "[run] Error: openocd not found in PATH."
        echo "      Install with: sudo apt install openocd"
        exit 1
    fi

    # Stop any running OpenOCD (holds the adapter exclusively)
    if pgrep -x openocd &>/dev/null; then
        echo "[run] Stopping existing OpenOCD instance..."
        pkill -x openocd
        sleep 0.5
    fi

    echo "[run] Flashing $ELF ..."
    openocd \
        -f "$CFG" \
        -c "program \"$ELF\" verify reset exit"

    echo "[run] Done."
    exit 0
fi

# ── QEMU targets (qemu_arm, qemu_m68k) ─────────────────────────────────────
TIMEOUT=30
if [[ "$TARGET" == "qemu_m68k" ]]; then
    QEMU_BIN="qemu-system-m68k"
    # Prefer locally-built QEMU if available
    LOCAL_QEMU="$PROJECT_DIR/third_party/qemu/build/qemu-system-m68k"
    if [[ -x "$LOCAL_QEMU" ]]; then
        QEMU_BIN="$LOCAL_QEMU"
    fi
    QEMU_ARGS=(-machine virt -cpu m68000)
else
    QEMU_BIN="qemu-system-arm"
    QEMU_ARGS=(-M mps2-an500 -serial mon:stdio)
fi

if ! command -v "$QEMU_BIN" &>/dev/null && [[ ! -x "$QEMU_BIN" ]]; then
    echo "[run] Error: $QEMU_BIN not found."
    if [[ "$TARGET" == "qemu_m68k" ]]; then
        echo "      Install with: sudo apt install qemu-system-misc"
        echo "      Or build from source: ./third_party/build-qemu-system-m68k.sh"
    else
        echo "      Install with: sudo apt install qemu-system-arm"
    fi
    exit 1
fi

# ── Test mode: run with timeout and check output ───────────────────────────
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

# ── GDB stub option ───────────────────────────────────────────────────────
GDB_ARGS=()
if [[ $DO_GDB -eq 1 ]]; then
    GDB_ARGS=(-s -S)   # -s = GDB server on :1234, -S = pause at reset
    echo "[run] Waiting for GDB on :1234 ..."
    echo "      Connect with: gdb-multiarch -ex 'target remote :1234' $ELF"
fi

# ── Run interactively ──────────────────────────────────────────────────────
echo "[run] Running $ELF ..."
"$QEMU_BIN" \
    "${QEMU_ARGS[@]}" \
    -nographic \
    -kernel "$ELF" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

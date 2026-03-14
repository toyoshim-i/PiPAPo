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
#   x68k               — Build only (real-hardware target, no emulator integration)
#
# Options:
#   --build             Build before running
#   --no-build          Skip build, use existing binary (default)
#   --test              Enable PPAP_TESTS, run automated test suite (implies --build)
#   --test-extended     Enable PPAP_TESTS_EXTENDED, run extended test suite (implies --build)
#   --filter=<pattern>  Only run tests whose path contains <pattern> (implies --build)
#   --flaky             Also run tests marked FLAKY (implies --build)
#   --slow              Also run tests marked SLOW (implies --build)
#   --clean             Clean build directory before building (implies --build)
#   --overlay=<dir>     Extra overlay directory copied into romfs (implies --build)
#   --h68k-debug        Enable kernel Human68k debug diagnostics (implies --build)
#   --gdb               (QEMU only) Pause at reset, wait for GDB on :1234
#   --m68k              Shorthand for TARGET=qemu_m68k (back-compat)
#
# Examples:
#   ./scripts/run.sh                        # run qemu_arm (must be pre-built)
#   ./scripts/run.sh --build                # build & run qemu_arm
#   ./scripts/run.sh --build qemu_m68k      # build & run m68k
#   ./scripts/run.sh --test                 # build ARM with tests, run & check
#   ./scripts/run.sh --test qemu_m68k       # build m68k with tests, run & check
#   ./scripts/run.sh --test-extended        # build ARM with extended tests, run & check
#   ./scripts/run.sh --test --filter=pipe   # run only tests matching "pipe"
#   ./scripts/run.sh --test --flaky         # also run flaky tests
#   ./scripts/run.sh --test --slow          # also run slow tests
#   ./scripts/run.sh --gdb                  # run existing ARM binary under GDB
#   ./scripts/run.sh pico1calc              # flash pre-built pico1calc via OpenOCD
#   ./scripts/run.sh --build pico1calc      # build & flash pico1calc
#   ./scripts/run.sh --test pico1           # build with tests & flash pico1
#
# Alternatively, without a debug adapter, hold BOOTSEL, plug in USB, then:
#   cp build/pico1calc/ppap_pico1calc.uf2 /media/$USER/RPI-RP2/
#
# Press Ctrl-A X to quit QEMU.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse flags ─────────────────────────────────────────────────────────────
TARGET=""
DO_BUILD=0
DO_TEST=0
DO_TEST_EXTENDED=0
DO_CLEAN=0
DO_GDB=0
DO_H68K_DEBUG=0
OVERLAY=""
FILTER=""
RUN_FLAKY=0
RUN_SLOW=0

for arg in "$@"; do
    case "$arg" in
        --m68k)     TARGET="qemu_m68k" ;;
        --no-build) DO_BUILD=0 ;;
        --build)    DO_BUILD=1 ;;
        --test)     DO_TEST=1; DO_BUILD=1 ;;
        --test-extended) DO_TEST=1; DO_TEST_EXTENDED=1; DO_BUILD=1 ;;
        --filter=*) FILTER="${arg#--filter=}"; DO_BUILD=1 ;;
        --flaky)    RUN_FLAKY=1; DO_BUILD=1 ;;
        --slow)     RUN_SLOW=1; DO_BUILD=1 ;;
        --clean)    DO_CLEAN=1; DO_BUILD=1 ;;
        --overlay=*)OVERLAY="${arg#--overlay=}"; DO_BUILD=1 ;;
        --h68k-debug) DO_H68K_DEBUG=1; DO_BUILD=1 ;;
        --gdb)      DO_GDB=1 ;;
        pico1|pico1calc|qemu_arm|qemu_m68k|x68k) TARGET="$arg" ;;
        -*)         echo "Unknown option: $arg" >&2; exit 1 ;;
        *)
            echo "Unknown target: $arg" >&2
            echo "Valid targets: pico1, pico1calc, qemu_arm, qemu_m68k, x68k" >&2
            exit 1
            ;;
    esac
done

# Default target
if [[ -z "$TARGET" ]]; then
    TARGET="qemu_arm"
fi

# ── Resolve ELF path ───────────────────────────────────────────────────────
BUILD_DIR="$PROJECT_DIR/build/$TARGET"
CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Merge filter/flaky/slow into an overlay dir ─────────────────────────────
TEMP_OVERLAY=""
if [[ -n "$FILTER" || $RUN_FLAKY -eq 1 || $RUN_SLOW -eq 1 ]]; then
    TEMP_OVERLAY=$(mktemp -d)
    # Copy any user-supplied overlay first so test controls take precedence
    if [[ -n "$OVERLAY" ]]; then
        OVERLAY_ABS="$(cd "$OVERLAY" && pwd)"
        cp -r "$OVERLAY_ABS/." "$TEMP_OVERLAY/"
    fi
    mkdir -p "$TEMP_OVERLAY/etc"
    if [[ -n "$FILTER" ]]; then
        printf '%s' "$FILTER" > "$TEMP_OVERLAY/etc/test_filter"
    fi
    if [[ $RUN_FLAKY -eq 1 ]]; then
        touch "$TEMP_OVERLAY/etc/test_run_flaky"
    fi
    if [[ $RUN_SLOW -eq 1 ]]; then
        touch "$TEMP_OVERLAY/etc/test_run_slow"
    fi
    OVERLAY="$TEMP_OVERLAY"
fi

# ── Build ───────────────────────────────────────────────────────────────────
if [[ $DO_BUILD -eq 1 ]]; then
    BUILD_ARGS=()
    if [[ $DO_CLEAN -eq 1 ]]; then BUILD_ARGS+=(--clean); fi
    if [[ $DO_TEST_EXTENDED -eq 1 ]]; then
        BUILD_ARGS+=(--test-extended)
    elif [[ $DO_TEST -eq 1 ]]; then
        BUILD_ARGS+=(--test)
    fi
    if [[ -n "$OVERLAY" ]]; then BUILD_ARGS+=("--overlay=$OVERLAY"); fi
    if [[ $DO_H68K_DEBUG -eq 1 ]]; then BUILD_ARGS+=(--h68k-debug); fi
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}" "$TARGET"
    # Clean up temp overlay after build (romfs already baked in)
    if [[ -n "$TEMP_OVERLAY" ]]; then rm -rf "$TEMP_OVERLAY"; fi
fi

# ── Pre-flight: ELF must exist ──────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo "[run] Error: $ELF not found."
    echo "      Run: ./scripts/build.sh $TARGET"
    exit 1
fi

# ── X68000 target — build floppy image (no emulator integration in Phase X-1) ─
if [[ "$TARGET" == "x68k" ]]; then
    "$SCRIPT_DIR/mkx68kimg.sh"
    exit 0
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
TIMEOUT=60
if [[ "$TARGET" == "qemu_m68k" ]]; then
    # m68k full test runs are consistently slower than ARM test runs.
    TIMEOUT=90
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
        echo "      Or build from source: ./third_party/build_qemu_system_m68k.sh"
    else
        echo "      Install with: sudo apt install qemu-system-arm"
    fi
    exit 1
fi

# ── Test mode: run with timeout and check output ───────────────────────────
if [[ $DO_TEST -eq 1 ]]; then
    if [[ $DO_TEST_EXTENDED -eq 1 ]]; then
        if [[ "$TARGET" == "qemu_m68k" ]]; then
            TIMEOUT=150
        else
            TIMEOUT=90
        fi
    fi
    # Filtered runs still execute kernel tests first, so ARM often needs extra
    # wall-clock headroom to finish selected user tests without false timeouts.
    if [[ -n "$FILTER" && "$TARGET" == "qemu_arm" ]]; then
        if [[ $DO_TEST_EXTENDED -eq 1 && $TIMEOUT -lt 120 ]]; then
            TIMEOUT=120
        elif [[ $TIMEOUT -lt 90 ]]; then
            TIMEOUT=90
        fi
    fi
    if [[ "$TARGET" == "qemu_m68k" && $RUN_SLOW -eq 1 && $TIMEOUT -lt 150 ]]; then
        TIMEOUT=150
    fi
    if [[ "$TARGET" == "qemu_m68k" && $RUN_SLOW -eq 1 && $DO_TEST_EXTENDED -eq 1 ]]; then
        TIMEOUT=180
    fi
    echo "[test] Running on-target tests (timeout ${TIMEOUT}s)..."
    OUTPUT=$(timeout "$TIMEOUT" "$QEMU_BIN" \
        "${QEMU_ARGS[@]}" \
        -nographic \
        -kernel "$ELF" 2>&1 || true)

    echo "$OUTPUT"

    if echo "$OUTPUT" | grep -q "ALL TESTS PASSED"; then
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

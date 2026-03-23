#!/usr/bin/env bash
# run.sh — Build, flash, or run a PiPAPo target
#
# Usage:
#   ./scripts/run.sh [OPTIONS] [TARGET]
#
# TARGET is one of:
#   pico1, pico1calc   — Flash to RP2040 via OpenOCD
#   pico2              — Flash to RP2350 via OpenOCD
#   qemu_arm (default) — Run under QEMU ARM
#   qemu_m68k          — Run under QEMU m68k
#   x68k               — Build floppy image and launch XEiJ emulator
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

# ── Docker helpers ──────────────────────────────────────────────────────────

target_docker_image() {
    case "$1" in
        qemu_arm|pico1|pico1calc|pico2) echo "ppap/arm" ;;
        qemu_m68k|x68k)                 echo "ppap/m68k" ;;
        qemu_rv32|pico2rv)              echo "ppap/riscv" ;;
        xtensa_cc)                      echo "ppap/xtensa" ;;
        ibmpc)                          echo "ppap/ia16" ;;
        *)                              echo "" ;;
    esac
}

docker_image_exists() {
    docker image inspect "$1" &>/dev/null 2>&1
}

docker_run() {
    local image="$1"; shift
    local tty_flag=""
    if [[ -t 0 ]]; then tty_flag="-it"; fi
    docker run --rm $tty_flag \
        -u "$(id -u):$(id -g)" \
        -v "$PROJECT_DIR:/ppap" \
        -w /ppap \
        "$image" \
        "$@"
}

# ── Parse flags ─────────────────────────────────────────────────────────────
TARGET=""
DO_BUILD=0
DO_TEST=0
DO_TEST_EXTENDED=0
DO_CLEAN=0
DO_GDB=0
DO_HOST=0
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
        --host)     DO_HOST=1 ;;
        pico1|pico1calc|pico2|pico2rv|qemu_arm|qemu_rv32|qemu_m68k|x68k|xtensa_cc|ibmpc) TARGET="$arg" ;;
        -*)         echo "Unknown option: $arg" >&2; exit 1 ;;
        *)
            echo "Unknown target: $arg" >&2
            echo "Valid targets: pico1, pico1calc, pico2, pico2rv, qemu_arm, qemu_rv32, qemu_m68k, x68k, xtensa_cc, ibmpc" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 [options] <target>"
    echo ""
    echo "Targets:"
    echo "  qemu_arm qemu_m68k qemu_rv32 ibmpc  (emulator)"
    echo "  pico1 pico1calc pico2 pico2rv        (flash to hardware)"
    echo "  x68k xtensa_cc                       (emulator / flash)"
    echo ""
    echo "Options:"
    echo "  --build         Build before running"
    echo "  --test          Build with tests and run"
    echo "  --test-extended Build with extended tests and run"
    echo "  --clean         Clean build directory first"
    echo "  --gdb           Start QEMU with GDB server on :1234"
    echo "  --host          Run QEMU on host instead of Docker"
    echo "  --filter=NAME   Run only matching tests"
    echo "  --overlay=DIR   Use overlay directory for romfs"
    exit 0
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

# ── IBM PC target (ibmpc) — override ELF to floppy image ───────────────────
if [[ "$TARGET" == "ibmpc" ]]; then
    ELF="$PROJECT_DIR/build/ibmpc/ppap_ibmpc.img"
fi

# ── ESP-IDF target (xtensa_cc) — uses idf.py, not ELF directly ─────────────
if [[ "$TARGET" == "xtensa_cc" ]]; then
    XTENSA_TC_DIR="$PROJECT_DIR/tools/xtensa-toolchain"
    ESP_IDF_DIR="$PROJECT_DIR/third_party/esp-idf"
    if [[ ! -f "$ESP_IDF_DIR/export.sh" ]]; then
        echo "[run] Error: ESP-IDF not found. Run: ./scripts/setup_docker.sh xtensa"
        exit 1
    fi
    export IDF_TOOLS_PATH="$XTENSA_TC_DIR"
    # shellcheck disable=SC1091
    source "$ESP_IDF_DIR/export.sh" >/dev/null 2>&1
    echo "[run] Flashing and monitoring xtensa_cc..."
    cd "$PROJECT_DIR/src/target/xtensa_cc"
    idf.py -B "$PROJECT_DIR/build/xtensa_cc" -p "${PPAP_PORT:-/dev/ttyACM0}" flash monitor
    exit 0
fi

# ── Pre-flight: ELF must exist ──────────────────────────────────────────────
if [[ ! -f "$ELF" ]]; then
    echo "[run] Error: $ELF not found."
    echo "      Run: ./scripts/build.sh $TARGET"
    exit 1
fi

# ── X68000 target — build floppy image and launch XEiJ ───────────────────────
if [[ "$TARGET" == "x68k" ]]; then
    DOCKER_IMAGE="$(target_docker_image x68k)"
    docker_run "$DOCKER_IMAGE" /ppap/scripts/mkx68kimg.sh
    XDF="$PROJECT_DIR/build/x68k/ppap_x68k.xdf"
    XEIJ_DIR="$PROJECT_DIR/tools/xeij"
    XEIJ_JAR="$XEIJ_DIR/XEiJ.jar"
    if [[ ! -f "$XEIJ_JAR" ]]; then
        echo "[run] Error: XEiJ not found at $XEIJ_JAR"
        echo "      Install with: ./scripts/setup_docker.sh m68k"
        exit 1
    fi
    # XEiJ requires Java 25+; prefer /usr/lib/jvm/java-25-openjdk-* over default
    JAVA_BIN="java"
    for jvm in /usr/lib/jvm/java-25-openjdk-*/bin/java; do
        if [[ -x "$jvm" ]]; then JAVA_BIN="$jvm"; break; fi
    done
    if ! "$JAVA_BIN" -version &>/dev/null 2>&1; then
        echo "[run] Error: java not found"
        echo "      Install with: sudo apt install openjdk-25-jre-headless"
        exit 1
    fi
    # Route AUX serial to TCP so we can read it from stdout
    XEIJ_TCP_PORT=54321
    # "TCP/IP ⇔ AUX" URL-encoded
    XEIJ_RS232C="TCP%2FIP+%E2%87%94+AUX"

    echo "[run] Launching XEiJ with $XDF (serial on TCP port $XEIJ_TCP_PORT) ..."
    "$JAVA_BIN" -jar "$XEIJ_JAR" \
        -fd0="$XDF" -boot=fd0 -memory=2 -model=Hybrid -mpu=68000 \
        -rs232cconnection="$XEIJ_RS232C" \
        -tcpipport="$XEIJ_TCP_PORT" &
    XEIJ_PID=$!

    # Wait for XEiJ TCP server to become ready, then stream serial to stdout
    echo "[run] Waiting for XEiJ serial on port $XEIJ_TCP_PORT ..."
    for i in $(seq 1 30); do
        if nc -z localhost "$XEIJ_TCP_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.5
    done
    if ! nc -z localhost "$XEIJ_TCP_PORT" 2>/dev/null; then
        echo "[run] Warning: TCP port $XEIJ_TCP_PORT not ready after 15s"
    fi
    # Stream serial output; when XEiJ exits, nc will EOF
    nc localhost "$XEIJ_TCP_PORT" || true
    wait "$XEIJ_PID" 2>/dev/null || true
    exit 0
fi

# ── Flash targets (pico1, pico1calc, pico2, pico2rv) ────────────────────────
if [[ "$TARGET" == pico1 || "$TARGET" == pico1calc || "$TARGET" == pico2 || "$TARGET" == pico2rv ]]; then

    # pico2/pico2rv (RP2350) needs the Raspberry Pi OpenOCD fork; pico1/pico1calc use system openocd
    if [[ "$TARGET" == pico2 || "$TARGET" == pico2rv ]]; then
        OPENOCD_BIN="$PROJECT_DIR/tools/openocd-rp/bin/openocd"
        OPENOCD_SCRIPTS="$PROJECT_DIR/tools/openocd-rp/share/openocd/scripts"
        if [[ ! -x "$OPENOCD_BIN" ]]; then
            echo "[run] Error: Raspberry Pi OpenOCD not found at $OPENOCD_BIN"
            echo "      Build with: ./third_party/build_openocd_rp.sh"
            exit 1
        fi
        # Use architecture-appropriate OpenOCD config:
        #   pico2   → rp2350.cfg (ARM Cortex-M33 debug targets)
        #   pico2rv → rp2350-riscv.cfg (Hazard3 RISC-V debug targets)
        if [[ "$TARGET" == pico2rv ]]; then
            OPENOCD_TARGET_CFG="target/rp2350-riscv.cfg"
        else
            OPENOCD_TARGET_CFG="target/rp2350.cfg"
        fi
        OPENOCD_ARGS=(-s "$OPENOCD_SCRIPTS"
                      -f interface/cmsis-dap.cfg
                      -f "$OPENOCD_TARGET_CFG"
                      -c "adapter speed 5000")
    else
        OPENOCD_BIN="openocd"
        if ! command -v openocd &>/dev/null; then
            echo "[run] Error: openocd not found in PATH."
            echo "      Install with: sudo apt install openocd"
            exit 1
        fi
        OPENOCD_ARGS=(-f "$SCRIPT_DIR/debug/openocd.cfg")
    fi

    # Stop any running OpenOCD (holds the adapter exclusively)
    if pgrep -x openocd &>/dev/null; then
        echo "[run] Stopping existing OpenOCD instance..."
        pkill -x openocd
        sleep 0.5
    fi

    echo "[run] Flashing $ELF ..."
    if "$OPENOCD_BIN" \
        "${OPENOCD_ARGS[@]}" \
        -c "program \"$ELF\" verify reset exit" 2>&1; then
        echo "[run] Done."
        exit 0
    fi

    # RP2350: if primary config failed, the chip may be in the other arch mode.
    # Try the alternate config (ARM↔RISC-V) before giving up.
    if [[ "$TARGET" == pico2 || "$TARGET" == pico2rv ]]; then
        if [[ "$TARGET" == pico2 ]]; then
            ALT_CFG="target/rp2350-riscv.cfg"
            echo "[run] ARM target failed — retrying via RISC-V debug port..."
        else
            ALT_CFG="target/rp2350.cfg"
            echo "[run] RISC-V target failed — retrying via ARM debug port..."
            echo "      (chip may still be in ARM mode from a previous flash)"
        fi
        if "$OPENOCD_BIN" -s "$OPENOCD_SCRIPTS" \
            -f interface/cmsis-dap.cfg -f "$ALT_CFG" \
            -c "adapter speed 5000" \
            -c "program \"$ELF\" verify reset exit" 2>&1; then
            echo "[run] Done (flashed via alternate debug port)."
            exit 0
        fi
        echo "[run] Error: both ARM and RISC-V debug ports failed."
        echo "      Try: hold BOOTSEL, power cycle, then copy the UF2:"
        echo "        cp ${ELF%.elf}.uf2 /media/\$USER/RP2350/"
    fi
    exit 1
fi

# ── QEMU targets (qemu_arm, qemu_rv32, qemu_m68k) ──────────────────────────
TIMEOUT=90
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
elif [[ "$TARGET" == "qemu_rv32" ]]; then
    QEMU_BIN="qemu-system-riscv32"
    QEMU_ARGS=(-M virt -bios none -serial mon:stdio)
elif [[ "$TARGET" == "ibmpc" ]]; then
    QEMU_BIN="qemu-system-i386"
    if [[ $DO_HOST -eq 1 ]]; then
        # --host: open graphical VGA window + serial on stdio
        QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio
                   -drive "file=$ELF,format=raw,if=floppy")
    else
        # Docker: serial only (no display)
        QEMU_ARGS=(-machine pc -cpu 486 -m 1M -display none -serial mon:stdio
                   -drive "file=$ELF,format=raw,if=floppy")
    fi
    ELF=""  # already passed via -drive, clear so run_qemu doesn't add -kernel
else
    QEMU_BIN="qemu-system-arm"
    QEMU_ARGS=(-M mps2-an500 -serial mon:stdio)
    # Add -semihosting when the build uses the semihost UART driver
    if grep -q "PPAP_SEMIHOST:BOOL=ON" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        QEMU_ARGS+=(-semihosting)
    fi
fi

# Resolve Docker image — required unless --host is set
DOCKER_IMAGE=""
if [[ $DO_HOST -eq 1 ]]; then
    if ! command -v "$QEMU_BIN" &>/dev/null; then
        echo "[run] Error: --host specified but $QEMU_BIN not found on host."
        exit 1
    fi
    echo "[run] Using host $QEMU_BIN (--host)"
else
    DOCKER_IMAGE="$(target_docker_image "$TARGET")"
    if [[ -z "$DOCKER_IMAGE" ]] || ! docker_image_exists "$DOCKER_IMAGE"; then
        echo "[run] Error: Docker image for $TARGET not found."
        echo "      Run: ./scripts/setup_docker.sh $TARGET"
        exit 1
    fi
    echo "[run] Using Docker image $DOCKER_IMAGE"
fi

# Helper: run QEMU inside Docker or on host (--host)
run_qemu() {
    local kernel_args=()
    if [[ -n "$DOCKER_IMAGE" ]]; then
        if [[ -n "$ELF" ]]; then
            kernel_args=(-kernel "/ppap/${ELF#"$PROJECT_DIR/"}")
        fi
        local remapped=()
        for a in "$@"; do
            remapped+=("${a//"$PROJECT_DIR"/"/ppap"}")
        done
        docker_run "$DOCKER_IMAGE" "$QEMU_BIN" "${remapped[@]}" "${kernel_args[@]}"
    else
        if [[ -n "$ELF" ]]; then
            kernel_args=(-kernel "$ELF")
        fi
        "$QEMU_BIN" "$@" "${kernel_args[@]}"
    fi
}

# ── Test mode: run with timeout and check output ───────────────────────────
if [[ $DO_TEST -eq 1 ]]; then
    if [[ $DO_TEST_EXTENDED -eq 1 ]]; then
        if [[ "$TARGET" == "qemu_m68k" ]]; then
            TIMEOUT=150
        else
            TIMEOUT=180
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
    TEST_DISPLAY=(-nographic)
    if [[ "$TARGET" == "ibmpc" ]]; then TEST_DISPLAY=(); fi
    OUTPUT=$(timeout "$TIMEOUT" run_qemu \
        "${QEMU_ARGS[@]}" \
        "${TEST_DISPLAY[@]+"${TEST_DISPLAY[@]}"}" 2>&1 || true)

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
echo "[run] Running ${ELF:-$TARGET} ..."
# ibmpc already specifies -display none; other targets use -nographic
DISPLAY_ARGS=(-nographic)
if [[ "$TARGET" == "ibmpc" ]]; then
    DISPLAY_ARGS=()
fi
run_qemu \
    "${QEMU_ARGS[@]}" \
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

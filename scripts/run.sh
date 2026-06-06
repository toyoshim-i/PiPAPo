#!/usr/bin/env bash
# run.sh — Build, flash, or run a PiPAPo target
#
# Usage:
#   ./scripts/run.sh [OPTIONS] [TARGET]
#
# TARGET is one of:
#   pico1, pico1calc        — Flash to RP2040 via OpenOCD
#   pico2, pico2rv          — Flash to RP2350 via OpenOCD
#   qemu_arm (default)      — Run under QEMU ARM
#   qemu_rv32               — Run under QEMU RISC-V
#   qemu_m68k               — Run under QEMU m68k
#   x68k                    — Build floppy image and launch XEiJ emulator
#   xtensa_cc               — Flash to ESP32-S3
#   pcxt                    — Run under PC/XT emulator
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
#   --hdd               (pcxt only) Boot from HDD image instead of floppy
#   --m68k              Shorthand for TARGET=qemu_m68k (back-compat)
#
# Examples:
#   ./scripts/run.sh                        # run qemu_arm (must be pre-built)
#   ./scripts/run.sh --build qemu_m68k      # build & run a target
#   ./scripts/run.sh --test                 # build ARM with tests, run & check
#   ./scripts/run.sh --test --filter=pipe   # run only tests matching "pipe"
#   ./scripts/run.sh --gdb                  # run existing ARM binary under GDB
#   ./scripts/run.sh --build pico1          # build & flash to hardware
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
        pcxt)                          echo "ppap/ia16" ;;
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
    local gui_flags=""
    if [[ $DO_GUI -eq 1 ]]; then
        # Docker Desktop: use host networking for X11 (no socket mount needed)
        gui_flags="--net=host -e DISPLAY=$DISPLAY"
    fi
    docker run --rm $tty_flag $gui_flags \
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
DO_GUI=0
DO_HDD=0
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
        --gui)      DO_GUI=1 ;;
        --hdd)      DO_HDD=1 ;;
        pico1|pico1calc|pico2|pico2rv|qemu_arm|qemu_rv32|qemu_m68k|x68k|xtensa_cc|pcxt) TARGET="$arg" ;;
        -*)         echo "Unknown option: $arg" >&2; exit 1 ;;
        *)
            echo "Unknown target: $arg" >&2
            echo "Valid targets: pico1, pico1calc, pico2, pico2rv, qemu_arm, qemu_rv32, qemu_m68k, x68k, xtensa_cc, pcxt" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 [options] <target>"
    echo ""
    echo "Targets:"
    echo "  qemu_arm qemu_m68k qemu_rv32 pcxt  (emulator)"
    echo "  pico1 pico1calc pico2 pico2rv        (flash to hardware)"
    echo "  x68k xtensa_cc                       (emulator / flash)"
    echo ""
    echo "Options:"
    echo "  --build         Build before running"
    echo "  --test          Build with tests and run"
    echo "  --test-extended Build with extended tests and run"
    echo "  --clean         Clean build directory first"
    echo "  --gdb           Start QEMU with GDB server on :1234"
    echo "  --host          Run QEMU on host instead of Docker (with GUI)"
    echo "  --gui           Run Docker QEMU with X11-forwarded VGA window"
    echo "  --hdd           Boot pcxt from HDD image instead of floppy"
    echo "  --filter=NAME   Run only matching tests"
    echo "  --overlay=DIR   Use overlay directory for romfs"
    exit 0
fi

# ── Resolve ELF path ───────────────────────────────────────────────────────
BUILD_DIR="$PROJECT_DIR/build/$TARGET"
if [[ $DO_TEST -eq 1 ]]; then
    BUILD_DIR="${BUILD_DIR}_test"
fi
CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Merge filter/flaky/slow into an overlay dir ─────────────────────────────
TEMP_OVERLAY=""
if [[ -n "$FILTER" || $RUN_FLAKY -eq 1 || $RUN_SLOW -eq 1 ||
      ( ( "$TARGET" == "pico1" || "$TARGET" == "pico2" ) &&
        $DO_TEST -eq 1 ) ]]; then
    mkdir -p "$PROJECT_DIR/build"
    TEMP_OVERLAY=$(mktemp -d "$PROJECT_DIR/build/test_overlay.XXXXXX")
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
    if [[ ( "$TARGET" == "pico1" || "$TARGET" == "pico2" ) &&
          $DO_TEST -eq 1 ]]; then
        touch "$TEMP_OVERLAY/etc/test_smp_hardware"
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

# ── PC/XT target (pcxt) — override ELF to floppy or HDD image ──────────────
if [[ "$TARGET" == "pcxt" ]]; then
    if [[ $DO_HDD -eq 1 ]]; then
        ELF="$BUILD_DIR/ppap_pcxt_hdd.img"
    else
        ELF="$BUILD_DIR/ppap_pcxt.img"
    fi
fi

# ── ESP-IDF target (xtensa_cc) — flash via esptool in Docker ──────────────
if [[ "$TARGET" == "xtensa_cc" ]]; then
    DOCKER_IMAGE="$(target_docker_image xtensa_cc)"
    if [[ -z "$DOCKER_IMAGE" ]] || ! docker_image_exists "$DOCKER_IMAGE"; then
        echo "[run] Error: Docker image for xtensa_cc not found."
        echo "      Run: ./scripts/setup.sh xtensa"
        exit 1
    fi

    FLASH_ARGS_FILE="$BUILD_DIR/flasher_args.json"
    if [[ ! -f "$FLASH_ARGS_FILE" ]]; then
        echo "[run] Error: $FLASH_ARGS_FILE not found."
        echo "      Run: ./scripts/build.sh xtensa_cc"
        exit 1
    fi

    PPAP_PORT="${PPAP_PORT:-/dev/ttyACM0}"
    if [[ -z "${PPAP_XTENSA_FLASH_BAUD:-}" ]]; then
        if [[ $DO_TEST -eq 1 ]]; then
            PPAP_XTENSA_FLASH_BAUD=115200
        else
            PPAP_XTENSA_FLASH_BAUD=460800
        fi
    fi
    if [[ ! -e "$PPAP_PORT" ]]; then
        echo "[run] Error: serial port $PPAP_PORT not found."
        echo "      Set PPAP_PORT to the correct device."
        exit 1
    fi

    # Resolve the device's group ID so the container can access it
    # without running as root (typically "dialout", GID 20).
    DEV_GID="$(stat -c '%g' "$PPAP_PORT")"

    # Translate the host build dir to the container path under /ppap.
    CONTAINER_BUILD_DIR="/ppap/${BUILD_DIR#"$PROJECT_DIR"/}"

    echo "[run] Flashing xtensa_cc via Docker ($PPAP_PORT @ "\
"$PPAP_XTENSA_FLASH_BAUD bps) ..."
    docker run --rm \
        --device="$PPAP_PORT" --group-add "$DEV_GID" \
        -v "$PROJECT_DIR:/ppap" -w /ppap \
        "$DOCKER_IMAGE" bash -c "
            export IDF_TOOLS_PATH=/opt/ppap/xtensa-tools && \
            source /opt/ppap/src/esp-idf/export.sh >/dev/null 2>&1 && \
            cd $CONTAINER_BUILD_DIR && \
            esptool.py --chip esp32s3 -p $PPAP_PORT -b $PPAP_XTENSA_FLASH_BAUD \
                --before default_reset --after hard_reset \
                write_flash \$(cat flash_args) \
        " 2>&1

    if [[ $DO_TEST -eq 1 ]]; then
        # Test mode: capture serial output non-interactively, watch for
        # the runtests pass/fail markers, exit with CI-friendly status.
        # Hardware boot + 33 user tests typically completes well under
        # 180s; the timeout is the upper bound, not the expected duration.
        TEST_TIMEOUT_S=180
        TEST_OUTPUT_LOG="$BUILD_DIR/test_output.log"
        echo "[run] Capturing test output (up to ${TEST_TIMEOUT_S}s) -> $TEST_OUTPUT_LOG ..."
        # tee to the log file in real time so partial output survives even if
        # docker / the device disconnects mid-run (e.g. host xHCI confusion
        # from CDC-ACM passthrough).  Grep the log file rather than a shell
        # var so the comparison is against what was actually persisted.
        docker run --rm \
            --device="$PPAP_PORT" --group-add "$DEV_GID" \
            -v "$PROJECT_DIR:/ppap" -w /ppap \
            "$DOCKER_IMAGE" bash -c "
                export IDF_TOOLS_PATH=/opt/ppap/xtensa-tools && \
                source /opt/ppap/src/esp-idf/export.sh >/dev/null 2>&1 && \
                python3 /ppap/scripts/xtensa_cc_test_monitor.py \
                    --port $PPAP_PORT --timeout $TEST_TIMEOUT_S
            " 2>&1 | tee "$TEST_OUTPUT_LOG"
        if grep -q "ALL TESTS PASSED" "$TEST_OUTPUT_LOG"; then
            echo ""
            echo "[test] PASS — all on-target tests passed"
            exit 0
        fi
        echo ""
        echo "[test] FAIL — tests did not all pass (or hardware timed out)"
        exit 1
    fi

    echo "[run] Flash complete. Starting serial monitor..."
    echo "      (Press Ctrl-] to quit)"

    # Monitor: stream serial output via pyserial inside Docker.
    # Use idf_monitor.py for ESP32 panic decoding and auto-reset.
    docker run --rm -it \
        --device="$PPAP_PORT" --group-add "$DEV_GID" \
        -v "$PROJECT_DIR:/ppap" -w /ppap \
        "$DOCKER_IMAGE" bash -c "
            export IDF_TOOLS_PATH=/opt/ppap/xtensa-tools && \
            source /opt/ppap/src/esp-idf/export.sh >/dev/null 2>&1 && \
            python3 -m esp_idf_monitor \
                --port $PPAP_PORT \
                --baud 115200 \
                $CONTAINER_BUILD_DIR/ppap_xtensa_cc.elf \
        " || true
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
        echo "      Install with: ./scripts/setup.sh m68k"
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

    DOCKER_IMAGE="$(target_docker_image "$TARGET")"
    if [[ -z "$DOCKER_IMAGE" ]] || ! docker_image_exists "$DOCKER_IMAGE"; then
        echo "[run] Error: Docker image for $TARGET not found."
        echo "      Run: ./scripts/setup.sh $TARGET"
        exit 1
    fi

    # OpenOCD uses CMSIS-DAP via USB HID (libusb).  Pass the entire USB bus
    # so the container can enumerate the debug probe.
    if [[ ! -d /dev/bus/usb ]]; then
        echo "[run] Error: /dev/bus/usb not found — Docker Engine required."
        exit 1
    fi

    OPENOCD_BIN=/opt/ppap/bin/openocd
    OPENOCD_SCRIPTS=/opt/ppap/share/openocd/scripts

    if [[ "$TARGET" == pico2 || "$TARGET" == pico2rv ]]; then
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
        OPENOCD_ARGS=(-f /ppap/scripts/debug/openocd.cfg)
    fi

    # Helper: run OpenOCD inside Docker with USB access
    run_openocd() {
        docker run --rm --privileged \
            -v /dev/bus/usb:/dev/bus/usb \
            -v "$PROJECT_DIR:/ppap" -w /ppap \
            "$DOCKER_IMAGE" \
            "$OPENOCD_BIN" "$@"
    }

    # Flash the ELF via the primary debug config.  On RP2350 the chip may
    # currently be running the other architecture, so its primary debug port
    # won't attach — fall back to the alternate config (ARM↔RISC-V).  Flashing
    # the image through the alternate port is sufficient: the bootrom reads the
    # IMAGE_DEF on reset and selects the matching arch.  The OpenOCD config
    # scripts are arch-independent, so the same Docker image serves both.
    flash_target() {
        if run_openocd \
            "${OPENOCD_ARGS[@]}" \
            -c "program \"$DOCKER_ELF\" verify reset exit" 2>&1; then
            return 0
        fi

        if [[ "$TARGET" == pico2 || "$TARGET" == pico2rv ]]; then
            local alt_cfg
            if [[ "$TARGET" == pico2 ]]; then
                alt_cfg="target/rp2350-riscv.cfg"
                echo "[run] ARM target failed — retrying via RISC-V debug port..."
                echo "      (chip may still be in RISC-V mode from a previous flash)"
            else
                alt_cfg="target/rp2350.cfg"
                echo "[run] RISC-V target failed — retrying via ARM debug port..."
                echo "      (chip may still be in ARM mode from a previous flash)"
            fi
            if run_openocd \
                -s "$OPENOCD_SCRIPTS" \
                -f interface/cmsis-dap.cfg \
                -f "$alt_cfg" \
                -c "adapter speed 5000" \
                -c "program \"$DOCKER_ELF\" verify reset exit" 2>&1; then
                echo "[run] Done (flashed via alternate debug port)."
                return 0
            fi
            echo "[run] Error: both ARM and RISC-V debug ports failed."
            echo "      Try: hold BOOTSEL, power cycle, then copy the UF2:"
            echo "        cp ${ELF%.elf}.uf2 /media/\$USER/RP2350/"
        fi
        return 1
    }

    run_pico2_semihost_test() {
        TEST_TIMEOUT_S=180
        TEST_OUTPUT_LOG="$BUILD_DIR/test_output.log"

        echo "[run] Capturing pico2 test output via ARM semihosting"
        echo "      (up to ${TEST_TIMEOUT_S}s) -> $TEST_OUTPUT_LOG"
        if ! flash_target >"$TEST_OUTPUT_LOG" 2>&1; then
            sed -n '1,220p' "$TEST_OUTPUT_LOG"
            echo ""
            echo "[test] FAIL — pico2 flashing failed"
            exit 1
        fi
        set +e
        docker run --rm --privileged \
            -v /dev/bus/usb:/dev/bus/usb \
            -v "$PROJECT_DIR:/ppap" -w /ppap \
            "$DOCKER_IMAGE" sh -c "
                set -u
                LOG=/tmp/openocd.log
                GDB_LOG=/tmp/gdb.log
                /opt/ppap/bin/openocd \
                    -s '$OPENOCD_SCRIPTS' \
                    -f interface/cmsis-dap.cfg \
                    -c 'set USE_CORE cm0' \
                    -f '$OPENOCD_TARGET_CFG' \
                    -c 'adapter speed 5000' \
                    -c init \
                    -c 'targets rp2350.cm0' \
                    -c 'reset halt' \
                    -c 'arm semihosting enable' \
                    -c 'arm semihosting_fileio enable' \
                    >\"\$LOG\" 2>&1 &
                OPENOCD_PID=\$!
                sleep 1
                arm-none-eabi-gdb -q '$DOCKER_ELF' \
                    -ex 'set pagination off' \
                    -ex 'target remote :3333' \
                    -ex continue \
                    >\"\$GDB_LOG\" 2>&1 &
                GDB_PID=\$!

                STATUS=3
                DEADLINE=\$((\$(date +%s) + $TEST_TIMEOUT_S))
                while kill -0 \"\$GDB_PID\" 2>/dev/null; do
                    if grep -q 'KERNEL TESTS FAILED\|SOME TESTS FAILED' \
                        \"\$GDB_LOG\" 2>/dev/null; then
                        STATUS=2
                        break
                    fi
                    if grep -q 'ALL TESTS PASSED' \"\$GDB_LOG\" 2>/dev/null; then
                        STATUS=0
                        break
                    fi
                    if [ \"\$(date +%s)\" -ge \"\$DEADLINE\" ]; then
                        break
                    fi
                    sleep 1
                done
                kill \"\$GDB_PID\" 2>/dev/null
                kill \"\$OPENOCD_PID\" 2>/dev/null
                wait \"\$GDB_PID\" 2>/dev/null
                wait \"\$OPENOCD_PID\" 2>/dev/null
                cat \"\$GDB_LOG\"
                cat \"\$LOG\" >&2
                exit \$STATUS
            " >>"$TEST_OUTPUT_LOG" 2>&1
        TEST_STATUS=$?
        set -e

        sed -n '1,220p' "$TEST_OUTPUT_LOG"
        if [[ $TEST_STATUS -eq 0 ]] ||
           { [[ $TEST_STATUS -eq 143 ]] &&
             grep -q "ALL TESTS PASSED" "$TEST_OUTPUT_LOG"; }; then
            echo ""
            echo "[test] PASS — pico2 tests passed"
            exit 0
        fi
        echo ""
        case "$TEST_STATUS" in
            2) echo "[test] FAIL — pico2 semihost output contains failures" ;;
            3) echo "[test] FAIL — pico2 semihost test timed out" ;;
            4) echo "[test] FAIL — pico2 OpenOCD semihost session exited" ;;
            *) echo "[test] FAIL — pico2 tests did not all pass" ;;
        esac
        exit 1
    }

    echo "[run] Flashing $ELF ..."
    DOCKER_ELF="/ppap/${ELF#"$PROJECT_DIR/"}"
    if [[ "$TARGET" == pico2 && $DO_TEST -eq 1 ]]; then
        run_pico2_semihost_test
    fi
    if [[ ( "$TARGET" == pico1 || "$TARGET" == pico2rv ) &&
          $DO_TEST -eq 1 ]]; then
        TEST_TIMEOUT_S=180
        TEST_OUTPUT_LOG="$BUILD_DIR/test_output.log"
        PPAP_PICO_PORT="${PPAP_PICO_PORT:-}"
        if [[ -z "$PPAP_PICO_PORT" ]]; then
            for port in /dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_on_Pico__CMSIS-DAP__*-if01; do
                if [[ -e "$port" ]]; then
                    PPAP_PICO_PORT="$port"
                    break
                fi
            done
        fi
        if [[ -z "$PPAP_PICO_PORT" || ! -e "$PPAP_PICO_PORT" ]]; then
            echo "[run] Error: Pico Debug Probe UART endpoint not found."
            echo "      Set PPAP_PICO_PORT to the GP0/GP1 serial adapter."
            exit 1
        fi
        echo "[run] Capturing $TARGET test output from $PPAP_PICO_PORT"
        echo "      (up to ${TEST_TIMEOUT_S}s) -> $TEST_OUTPUT_LOG"
        set +e
        if [[ "$TARGET" == pico2rv ]]; then
            flash_target 2>&1 | tee "$TEST_OUTPUT_LOG"
            FLASH_STATUS=${PIPESTATUS[0]}
            if [[ $FLASH_STATUS -eq 0 ]]; then
                python3 "$SCRIPT_DIR/serial_test_monitor.py" \
                    --port "$PPAP_PICO_PORT" --timeout "$TEST_TIMEOUT_S" \
                    2>&1 | tee -a "$TEST_OUTPUT_LOG"
            fi
        else
            python3 "$SCRIPT_DIR/serial_test_monitor.py" \
                --port "$PPAP_PICO_PORT" --timeout "$TEST_TIMEOUT_S" \
                2>&1 | tee "$TEST_OUTPUT_LOG" &
            MONITOR_PID=$!
            sleep 1
            flash_target
            FLASH_STATUS=$?
            if [[ $FLASH_STATUS -ne 0 ]]; then
                kill "$MONITOR_PID" 2>/dev/null
            fi
            wait "$MONITOR_PID"
        fi
        set -e
        if [[ $FLASH_STATUS -ne 0 ]]; then
            echo ""
            echo "[test] FAIL — $TARGET flashing failed"
            exit 1
        fi
        if grep -q "KERNEL TESTS FAILED\|SOME TESTS FAILED" "$TEST_OUTPUT_LOG"; then
            echo ""
            echo "[test] FAIL — $TARGET test output contains failures"
            exit 1
        fi
        if grep -q "ALL TESTS PASSED" "$TEST_OUTPUT_LOG"; then
            echo ""
            echo "[test] PASS — $TARGET tests passed"
            exit 0
        fi
        echo ""
        echo "[test] FAIL — tests did not all pass (or hardware timed out)"
        exit 1
    fi
    if flash_target; then
        echo "[run] Done."
        exit 0
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
    # Use stdio mux so serial input works and Ctrl-A X can quit QEMU.
    QEMU_ARGS=(-M virt -bios none -nographic -serial mon:stdio)
elif [[ "$TARGET" == "pcxt" ]]; then
    QEMU_BIN="qemu-system-i386"
    if [[ $DO_HDD -eq 1 ]]; then
        PCXT_DRIVE="-drive file=$ELF,format=raw,if=ide"
    else
        PCXT_DRIVE="-drive file=$ELF,format=raw,if=floppy"
    fi
    PCXT_POFF="-device isa-debug-exit,iobase=0x501,iosize=2"
    if [[ $DO_HOST -eq 1 ]]; then
        # --host: open graphical VGA window + serial on stdio
        QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio $PCXT_DRIVE $PCXT_POFF)
    elif [[ $DO_GUI -eq 1 ]]; then
        # --gui: X11 forwarded VGA window + serial on stdio
        QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio $PCXT_DRIVE $PCXT_POFF)
    else
        # Docker headless: keep serial on stdio for kernel logs and
        # user-space debugging over ttyS0.
        QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio $PCXT_DRIVE $PCXT_POFF)
    fi
    ELF=""  # already passed via -drive, clear so run_qemu doesn't add -kernel
else
    QEMU_BIN="qemu-system-arm"
    QEMU_ARGS=(-M mps2-an500 -serial mon:stdio -semihosting)
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
        echo "      Run: ./scripts/setup.sh $TARGET"
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
    if [[ "$TARGET" == "pcxt" && $TIMEOUT -lt 180 ]]; then
        TIMEOUT=180
    fi
    if [[ "$TARGET" == "qemu_m68k" && $RUN_SLOW -eq 1 && $TIMEOUT -lt 150 ]]; then
        TIMEOUT=150
    fi
    if [[ "$TARGET" == "qemu_m68k" && $RUN_SLOW -eq 1 && $DO_TEST_EXTENDED -eq 1 ]]; then
        TIMEOUT=180
    fi
    echo "[test] Running on-target tests (timeout ${TIMEOUT}s)..."
    # Build QEMU command for timeout (can't use bash functions with timeout)
    QEMU_CMD=()
    if [[ -n "$DOCKER_IMAGE" ]]; then
        # Test mode is non-interactive; avoid -it so timeout/signal handling
        # remains deterministic and output capture is stable.
        QEMU_CMD=(docker run --rm
            -u "$(id -u):$(id -g)"
            -v "$PROJECT_DIR:/ppap" -w /ppap
            "$DOCKER_IMAGE")
        REMAPPED_ARGS=()
        for a in "${QEMU_ARGS[@]}"; do
            REMAPPED_ARGS+=("${a//"$PROJECT_DIR"/"/ppap"}")
        done
        TEST_KERNEL_ARGS=()
        if [[ -n "$ELF" ]]; then
            TEST_KERNEL_ARGS=(-kernel "/ppap/${ELF#"$PROJECT_DIR/"}")
        fi
        QEMU_CMD+=("$QEMU_BIN" "${REMAPPED_ARGS[@]}" -nographic "${TEST_KERNEL_ARGS[@]}")
    else
        TEST_KERNEL_ARGS=()
        if [[ -n "$ELF" ]]; then TEST_KERNEL_ARGS=(-kernel "$ELF"); fi
        QEMU_CMD=("$QEMU_BIN" "${QEMU_ARGS[@]}" -nographic "${TEST_KERNEL_ARGS[@]}")
    fi
    # Some QEMU runs ignore SIGTERM under heavy load; send SIGKILL after a
    # short grace period so --test never hangs indefinitely.
    OUTPUT=$(timeout -k 15s "${TIMEOUT}s" "${QEMU_CMD[@]}" 2>&1 || true)

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
DISPLAY_ARGS=(-nographic)
if [[ $DO_HOST -eq 1 || $DO_GUI -eq 1 ]]; then
    DISPLAY_ARGS=()
fi
run_qemu \
    "${QEMU_ARGS[@]}" \
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}" \
    "${GDB_ARGS[@]+"${GDB_ARGS[@]}"}"

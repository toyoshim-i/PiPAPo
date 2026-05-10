#!/usr/bin/env bash
# build.sh — Build a PPAP target
#
# Usage:
#   ./scripts/build.sh [OPTIONS] TARGET
#
# TARGET is one of:
#   all,
#   pico1, pico1calc, pico2, pico2rv,
#   qemu_arm, qemu_rv32, qemu_m68k,
#   x68k, xtensa_cc, pcxt
#
# Options:
#   --test              Enable PPAP_TESTS (kernel + userland test suite)
#   --test-extended     Enable PPAP_TESTS + PPAP_TESTS_EXTENDED
#   --clean             Remove build directory before building (full rebuild)
#   --overlay=<dir>     Extra overlay directory copied into romfs (highest priority)
#   --h68k-debug        Enable kernel Human68k debug diagnostics
#
# Examples:
#   ./scripts/build.sh all                # build all targets
#   ./scripts/build.sh pico1              # build a specific target
#   ./scripts/build.sh --test qemu_arm    # build with tests
#   ./scripts/build.sh --clean qemu_m68k  # clean rebuild
#   ./scripts/build.sh --overlay=~/my_x68k qemu_m68k  # add custom files to romfs

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Docker helpers ──────────────────────────────────────────────────────────

# Map target → Docker image family
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

# Check if a Docker image exists locally
docker_image_exists() {
    docker image inspect "$1" &>/dev/null 2>&1
}

# Run a command inside the target's Docker container
docker_run() {
    local image="$1"; shift
    docker run --rm \
        -u "$(id -u):$(id -g)" \
        -v "$PROJECT_DIR:/ppap" \
        -w /ppap \
        "$image" \
        "$@"
}

# ── Parse arguments ──────────────────────────────────────────────────────────
TESTS=OFF
TESTS_EXTENDED=OFF
CLEAN=0
OVERLAY=""
H68K_DEBUG=OFF
TARGET=""

# Targets swept by "build.sh all" — device/emulator targets only.  `host` is
# a developer tool (native build of select user-space apps), opt-in only.
VALID_TARGETS=(
    pico1 pico1calc pico2 pico2rv
    qemu_arm qemu_rv32 qemu_m68k
    x68k xtensa_cc pcxt
)

for arg in "$@"; do
    case "$arg" in
        --test)       TESTS=ON ;;
        --test-extended) TESTS=ON; TESTS_EXTENDED=ON ;;
        --clean)      CLEAN=1 ;;
        --overlay=*)  OVERLAY="${arg#--overlay=}" ;;
        --h68k-debug) H68K_DEBUG=ON ;;
        -*)           echo "Unknown option: $arg" >&2; exit 1 ;;
        *)            TARGET="$arg" ;;
    esac
done

# Show usage if no target specified
if [[ -z "$TARGET" ]]; then
    sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
    exit 0
fi

# Pseudo target: build all supported targets with current flags.
if [[ "$TARGET" == "all" ]]; then
    echo "[build] Building all targets: ${VALID_TARGETS[*]}"
    BUILD_ARGS=()
    if [[ "$TESTS_EXTENDED" == "ON" ]]; then
        BUILD_ARGS+=(--test-extended)
    elif [[ "$TESTS" == "ON" ]]; then
        BUILD_ARGS+=(--test)
    fi
    if [[ $CLEAN -eq 1 ]]; then
        BUILD_ARGS+=(--clean)
    fi
    if [[ -n "$OVERLAY" ]]; then
        BUILD_ARGS+=("--overlay=$OVERLAY")
    fi
    if [[ "$H68K_DEBUG" == "ON" ]]; then
        BUILD_ARGS+=(--h68k-debug)
    fi

    for t in "${VALID_TARGETS[@]}"; do
        echo "[build] ===== target: $t ====="
        "$0" "${BUILD_ARGS[@]}" "$t"
    done
    echo "[build] All targets built successfully"
    exit 0
fi

# ── Repository boundary checks ───────────────────────────────────────────────
if ! command -v python3 &>/dev/null; then
  echo "[build] Error: python3 is required for pre-build checks."
  echo "        Install it with: sudo apt install python3"
  exit 1
fi
# Note: source-level checks (clang-format, include order, etc.) run *after*
# the build so that syntax errors (e.g. missing braces) are surfaced by the
# compiler first. Reformatting syntactically-broken sources rearranges code
# in unintended ways.
run_source_checks() {
    "$SCRIPT_DIR/check_allocator_boundaries.sh"
    "$SCRIPT_DIR/check_module_boundaries.sh"
    "$SCRIPT_DIR/check_common_no_c.sh"
    "$SCRIPT_DIR/check_no_extern_in_c.sh"
    "$SCRIPT_DIR/check_include_paths.sh"
    python3 "$SCRIPT_DIR/check_include_order.py"
    python3 "$SCRIPT_DIR/check_include_guards.py"
    python3 "$SCRIPT_DIR/check_own_header.py"
    "$SCRIPT_DIR/check_clang_format.sh"
}

# ── Determine source and build directories ──────────────────────────────────
case "$TARGET" in
    qemu_arm|qemu_rv32|pico1|pico1calc|pico2|pico2rv)
        SOURCE_DIR="$PROJECT_DIR/src/target/$TARGET"
        BUILD_DIR="$PROJECT_DIR/build/$TARGET"
        ;;
    qemu_m68k)
        SOURCE_DIR="$PROJECT_DIR/src/target/qemu_m68k"
        BUILD_DIR="$PROJECT_DIR/build/qemu_m68k"
        ;;
    x68k)
        SOURCE_DIR="$PROJECT_DIR/src/target/x68k"
        BUILD_DIR="$PROJECT_DIR/build/x68k"
        ;;
    xtensa_cc)
        SOURCE_DIR="$PROJECT_DIR/src/target/xtensa_cc"
        BUILD_DIR="$PROJECT_DIR/build/xtensa_cc"
        ;;
    pcxt)
        SOURCE_DIR="$PROJECT_DIR/src/target/pcxt"
        BUILD_DIR="$PROJECT_DIR/build/pcxt"
        ;;
    host)
        SOURCE_DIR="$PROJECT_DIR/src/target/host"
        BUILD_DIR="$PROJECT_DIR/build/host"
        ;;
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: all, ${VALID_TARGETS[*]}"
        exit 1
        ;;
esac

# Separate build dirs for test vs non-test to avoid stale cached objects
# when PPAP_TESTS changes (target_init_path depends on the define).
if [[ "$TESTS" == "ON" ]]; then
    BUILD_DIR="${BUILD_DIR}_test"
fi

CMAKE_TARGET="ppap_${TARGET}"
ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

# ── Clean build directory if requested ───────────────────────────────────────
if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "[build] Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# ── Pre-build cross-arch binaries if needed ─────────────────────────────────
# ARM targets include m68k test binaries for the m68k emulator.  When building
# inside Docker, the ARM container doesn't have m68k-elf-gcc, so we pre-build
# the cross ELFs in the m68k container first.
M68K_CROSS_IMAGE="$(target_docker_image qemu_m68k)"
case "$TARGET" in
    qemu_arm|pico1|pico1calc|pico2|qemu_rv32|pico2rv)
        if ! command -v m68k-elf-gcc &>/dev/null && \
           [[ -n "$M68K_CROSS_IMAGE" ]] && docker_image_exists "$M68K_CROSS_IMAGE"; then
            SHARED_BUILD="$PROJECT_DIR/build/shared_m68k"
            echo "[build] Pre-building m68k cross binaries in $M68K_CROSS_IMAGE..."
            docker_run "$M68K_CROSS_IMAGE" bash -c "
                mkdir -p /ppap/build/shared_m68k && \
                M68K_CC=m68k-elf-gcc && \
                M68K_LD=/ppap/src/arch/m68k/user/user.ld && \
                M68K_LIBGCC=\$(\$M68K_CC -m68000 -print-libgcc-file-name) && \
                \$M68K_CC -m68000 -c -o /ppap/build/shared_m68k/hello_m68k.o \
                    /ppap/src/arch/m68k/user/hello.S && \
                \$M68K_CC -m68000 -nostdlib -T \$M68K_LD \
                    -Wl,--gc-sections -Wl,--build-id=none \
                    -o /ppap/build/shared_m68k/hello_m68k.elf \
                    /ppap/build/shared_m68k/hello_m68k.o \$M68K_LIBGCC
            "
            echo "[build] m68k cross binaries ready"
        fi
        ;;
esac

# ── Build ────────────────────────────────────────────────────────────────────
EXTRA_ARGS=(-DPPAP_EXTRA_OVERLAY=)
case "$TARGET" in
    qemu_arm)
        # Bare-metal ARM (no Pico SDK) — needs explicit toolchain file
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_arm_m.cmake")
        ;;
    qemu_m68k|x68k)
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_m68k.cmake")
        ;;
esac

# ── ESP-IDF build (xtensa_cc) ────────────────────────────────────────────────
if [[ "$TARGET" == "xtensa_cc" ]]; then
    ESP_IDF_DIR="${IDF_PATH:-/opt/ppap/src/esp-idf}"
    XTENSA_TC_DIR="${IDF_TOOLS_PATH:-/opt/ppap/xtensa-tools}"
    if [[ ! -f "$ESP_IDF_DIR/export.sh" ]]; then
        # Running on host — re-exec inside Docker
        DOCKER_IMAGE="$(target_docker_image xtensa_cc)"
        if [[ -z "$DOCKER_IMAGE" ]] || ! docker_image_exists "$DOCKER_IMAGE"; then
            echo "[build] Error: Docker image for xtensa_cc not found."
            echo "        Run: ./scripts/setup.sh xtensa"
            exit 1
        fi
        echo "[build] Building xtensa_cc via Docker ($DOCKER_IMAGE)..."
        BUILD_ARGS=()
        if [[ $CLEAN -eq 1 ]]; then BUILD_ARGS+=(--clean); fi
        if [[ "$TESTS" == "ON" ]]; then BUILD_ARGS+=(--test); fi
        docker_run "$DOCKER_IMAGE" bash -c "
            export IDF_TOOLS_PATH=/opt/ppap/xtensa-tools && \
            source /opt/ppap/src/esp-idf/export.sh >/dev/null 2>&1 && \
            /ppap/scripts/build.sh ${BUILD_ARGS[*]+"${BUILD_ARGS[*]}"} xtensa_cc
        "
        exit $?
    fi
    # Source ESP-IDF environment with project-local toolchain
    export IDF_TOOLS_PATH="$XTENSA_TC_DIR"
    # shellcheck disable=SC1091
    source "$ESP_IDF_DIR/export.sh" >/dev/null 2>&1
    if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
        echo "[build] Cleaning $BUILD_DIR..."
        rm -rf "$BUILD_DIR"
    fi
    XTENSA_CC=xtensa-esp-elf-gcc
    XTENSA_STRIP=xtensa-esp-elf-strip
    # ESP32-S3 dynconfig (selects little-endian LX7 instruction encoding).
    # The .so lives under the versioned xtensa-esp-elf subtree.
    XTENSA_LIB_DIR="$(find "$XTENSA_TC_DIR/tools/xtensa-esp-elf" -name "xtensa_esp32s3.so" -printf '%h' -quit 2>/dev/null)"
    if [[ -z "$XTENSA_LIB_DIR" ]]; then
        echo "[build] Error: xtensa_esp32s3.so not found in toolchain"
        exit 1
    fi
    export LD_LIBRARY_PATH="$XTENSA_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    XTENSA_DYNCONFIG="-mdynconfig=xtensa_esp32s3.so"
    MKROMFS="$PROJECT_DIR/tools/mkromfs/mkromfs"
    USER_ARCH_DIR="$PROJECT_DIR/src/arch/xtensa/user"

    xtensa_xip_report() {
        local elf="$1"
        local counts
        local text_blocked=0
        local text_literal_refs=0
        local literal_abs=0
        local literal_data_refs=0
        local flash_base=0
        local flash_prefix=""
        local literal_prelinked=0
        local status

        counts="$(
            readelf -r "$elf" 2>/dev/null | awk '
                /^Relocation section / {
                    section = $3
                    gsub(/'\''/, "", section)
                    next
                }
                $1 ~ /^[0-9a-f]+$/ {
                    if (section == ".rela.text" &&
                        ($3 == "R_XTENSA_32" || $3 == "R_XTENSA_PLT"))
                        text_blocked++
                    if (section == ".rela.text" &&
                        $3 == "R_XTENSA_SLOT0_OP" && $5 == ".literal")
                        text_literal_refs++
                    if (section == ".rela.literal" && $3 == "R_XTENSA_32")
                        literal_abs++
                    if (section == ".rela.literal" && $3 == "R_XTENSA_32" &&
                        ($5 == ".data" || $5 == ".bss"))
                        literal_data_refs++
                }
                END {
                    printf "%u %u %u %u\n",
                           text_blocked, text_literal_refs,
                           literal_abs, literal_data_refs
                }'
        )"
        read -r text_blocked text_literal_refs literal_abs literal_data_refs \
            <<<"$counts"

        flash_base="$(
            readelf -s "$elf" 2>/dev/null | awk '
                $8 == "__ppap_xip_flash_base" {
                    print "0x" $2
                    exit
                }'
        )"
        if [[ -n "$flash_base" && "$flash_base" != "0x00000000" &&
              "$literal_abs" -gt 0 ]]; then
            flash_prefix="$(printf '%04x' $((flash_base >> 16)))"
            literal_prelinked="$(
                readelf -x .literal "$elf" 2>/dev/null | awk -v prefix="$flash_prefix" '
                    BEGIN { ok = 1; words = 0 }
                    $1 ~ /^0x[0-9a-f]+$/ {
                        for (i = 2; i <= 5; i++) {
                            if ($i !~ /^[0-9a-f]{8}$/) continue
                            word = substr($i, 7, 2) substr($i, 5, 2) \
                                   substr($i, 3, 2) substr($i, 1, 2)
                            if (substr(word, 1, 4) != prefix)
                                ok = 0
                            words++
                        }
                    }
                    END {
                        if (words > 0 && ok)
                            print 1
                        else
                            print 0
                    }'
            )"
        fi

        if [[ "$text_blocked" -gt 0 ]]; then
            status="text-blocked"
        elif [[ "$text_literal_refs" -gt 0 || "$literal_abs" -gt 0 ]]; then
            if [[ "$literal_data_refs" -gt 0 ]]; then
                status="text-clean, data-coupled"
            elif [[ "$literal_prelinked" == "1" ]]; then
                status="text-clean, literal-prelinked"
            else
                status="text-clean, literal-coupled"
            fi
        else
            status="XIP-clean"
        fi

        echo "[build] Xtensa XIP: $(basename "$elf"): $status" \
             "(text=$text_blocked, slot0->literal=$text_literal_refs," \
             "literal32=$literal_abs, data32=$literal_data_refs," \
             "prelinked=$literal_prelinked)"

        if [[ "${PPAP_XTENSA_XIP_STRICT:-0}" == "1" &&
              "$text_blocked" -gt 0 ]]; then
            echo "[build] Xtensa XIP strict mode: refusing text-blocked ELF"
            return 1
        fi
        return 0
    }

    # Build mkromfs host tool if needed
    if [[ ! -x "$MKROMFS" ]]; then
        echo "[build] Building mkromfs host tool..."
        cc -O2 -I"$PROJECT_DIR/src/kernel/fs" \
            -o "$MKROMFS" "$PROJECT_DIR/tools/mkromfs/mkromfs.c"
    fi

    # ── ESP-IDF configure (before romfs, since set-target does fullclean) ────
    cd "$SOURCE_DIR"
    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        echo "[build] Configuring xtensa_cc for esp32s3..."
        idf.py -B "$BUILD_DIR" set-target esp32s3
    fi

    # ── Build user-space binaries and romfs ──────────────────────────────────
    # Must come after set-target (which may fullclean BUILD_DIR) but before
    # idf.py build (which embeds romfs.bin via .incbin).
    ROMFS_STAGING="$BUILD_DIR/romfs_staging"
    ROMFS_BIN="$BUILD_DIR/romfs.bin"

    # Build user binaries (call0 ABI, PIC, no libc)
    mkdir -p "$BUILD_DIR/user"
    echo "[build] Compiling user binaries (xtensa call0)..."
    USER_DIR="$PROJECT_DIR/src/user"
    XTENSA_USER_COMMON_FLAGS="$XTENSA_DYNCONFIG -mabi=call0 \
        -ffreestanding -nostdlib -Os -fPIC -Wl,--emit-relocs \
        -I$USER_DIR -I$PROJECT_DIR/src -isystem $USER_DIR/include \
        $USER_ARCH_DIR/crt0.S $USER_ARCH_DIR/syscall.S \
        $USER_DIR/lib/string.c $USER_DIR/lib/stdio.c \
        $USER_DIR/lib/stdlib.c $USER_DIR/lib/alloc.c \
        $USER_DIR/lib/file.c $USER_DIR/lib/time.c \
        $USER_DIR/lib/errno.c $USER_DIR/lib/signal.c \
        $USER_DIR/lib/sigaction.c $USER_DIR/lib/util.c"
    XTENSA_RAM_USER_FLAGS="$XTENSA_USER_COMMON_FLAGS \
        -T $USER_ARCH_DIR/user.ld"
    XTENSA_XIP_USER_FLAGS="$XTENSA_USER_COMMON_FLAGS \
        -T $USER_ARCH_DIR/user_xip.ld"
    XTENSA_XIP_FIXED_USER_FLAGS="$XTENSA_USER_COMMON_FLAGS \
        -Wl,--defsym=__ppap_xip_flash_base=0x3C000000 \
        -T $USER_ARCH_DIR/user_xip.ld"

    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_RAM_USER_FLAGS "$USER_DIR/hello.c" \
        -o "$BUILD_DIR/user/hello.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_RAM_USER_FLAGS "$USER_DIR/init.c" \
        -o "$BUILD_DIR/user/init.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_RAM_USER_FLAGS "$USER_DIR/getty.c" \
        -o "$BUILD_DIR/user/getty.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_RAM_USER_FLAGS "$USER_DIR/push.c" \
        "$USER_DIR/push_line.c" \
        -o "$BUILD_DIR/user/push.elf"

    echo "[build] Compiling Xtensa XIP-layout variants..."
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_USER_FLAGS "$USER_DIR/hello.c" \
        -o "$BUILD_DIR/user/hello.xip.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_USER_FLAGS "$USER_DIR/init.c" \
        -o "$BUILD_DIR/user/init.xip.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_USER_FLAGS "$USER_DIR/getty.c" \
        -o "$BUILD_DIR/user/getty.xip.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_USER_FLAGS "$USER_DIR/push.c" \
        "$USER_DIR/push_line.c" \
        -o "$BUILD_DIR/user/push.xip.elf"

    echo "[build] Compiling Xtensa fixed-base XIP variants..."
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_FIXED_USER_FLAGS "$USER_DIR/hello.c" \
        -o "$BUILD_DIR/user/hello.xipfix.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_FIXED_USER_FLAGS "$USER_DIR/init.c" \
        -o "$BUILD_DIR/user/init.xipfix.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_FIXED_USER_FLAGS "$USER_DIR/getty.c" \
        -o "$BUILD_DIR/user/getty.xipfix.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_XIP_FIXED_USER_FLAGS "$USER_DIR/push.c" \
        "$USER_DIR/push_line.c" \
        -o "$BUILD_DIR/user/push.xipfix.elf"

    # Strip debug symbols (keep relocation info)
    for f in "$BUILD_DIR"/user/*.elf; do
        $XTENSA_STRIP --strip-debug "$f"
    done

    for f in "$BUILD_DIR"/user/*.xip*.elf; do
        xtensa_xip_report "$f"
    done

    # Stage romfs directory
    rm -rf "$ROMFS_STAGING"
    mkdir -p "$ROMFS_STAGING"/{bin,sbin,etc,dev,proc,tmp}
    cp "$BUILD_DIR/user/init.elf"  "$ROMFS_STAGING/sbin/init"
    cp "$BUILD_DIR/user/hello.elf" "$ROMFS_STAGING/bin/hello"
    cp "$BUILD_DIR/user/getty.elf" "$ROMFS_STAGING/bin/getty"
    cp "$BUILD_DIR/user/push.elf"  "$ROMFS_STAGING/bin/push"
    cp "$BUILD_DIR/user/init.xip.elf"  "$ROMFS_STAGING/sbin/init.xip"
    cp "$BUILD_DIR/user/hello.xip.elf" "$ROMFS_STAGING/bin/hello.xip"
    cp "$BUILD_DIR/user/getty.xip.elf" "$ROMFS_STAGING/bin/getty.xip"
    cp "$BUILD_DIR/user/push.xip.elf"  "$ROMFS_STAGING/bin/push.xip"
    ln -sf push "$ROMFS_STAGING/bin/sh"

    # Install /etc files (base + target overlay)
    cp "$PROJECT_DIR/src/etc/"* "$ROMFS_STAGING/etc/" 2>/dev/null || true
    OVERLAY_DIR="$PROJECT_DIR/src/target/xtensa_cc/romfs"
    if [[ -d "$OVERLAY_DIR" ]]; then
        cp -r "$OVERLAY_DIR"/* "$ROMFS_STAGING/" 2>/dev/null || true
    fi

    # Generate romfs.bin
    echo "[build] Generating romfs.bin..."
    "$MKROMFS" "$ROMFS_STAGING" "$ROMFS_BIN"
    echo "[build] romfs.bin: $(wc -c < "$ROMFS_BIN") bytes"

    # ── ESP-IDF kernel build ─────────────────────────────────────────────────
    echo "[build] Building xtensa_cc via idf.py..."
    idf.py -B "$BUILD_DIR" build
    echo "[build] Built xtensa_cc"
    run_source_checks
    exit 0
fi

if [[ -n "$OVERLAY" ]]; then
    # Resolve to absolute path
    OVERLAY="$(cd "$OVERLAY" 2>/dev/null && pwd)" || {
        echo "[build] Error: overlay directory '$OVERLAY' not found" >&2
        exit 1
    }
    EXTRA_ARGS[0]="-DPPAP_EXTRA_OVERLAY=$OVERLAY"
fi

# ── Host build — native toolchain, no Docker ────────────────────────────────
if [[ "$TARGET" == "host" ]]; then
    echo "[build] Building host target (native)"
    cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_host.cmake" \
          -S "$SOURCE_DIR" -B "$BUILD_DIR" >/dev/null
    cmake --build "$BUILD_DIR" -- -j"$(nproc 2>/dev/null || echo 4)"
    echo "[build] Host binaries in $BUILD_DIR"
    run_source_checks
    exit 0
fi

echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS, PPAP_TESTS_EXTENDED=$TESTS_EXTENDED)..."

# Convert host-absolute paths to container-relative for Docker builds
DOCKER_IMAGE="$(target_docker_image "$TARGET")"
if [[ -z "$DOCKER_IMAGE" ]] || ! docker_image_exists "$DOCKER_IMAGE"; then
    echo "[build] Error: Docker image $DOCKER_IMAGE not found."
    echo "        Run: ./scripts/setup.sh $TARGET"
    exit 1
fi

echo "[build] Using Docker image $DOCKER_IMAGE"
D_SOURCE="/ppap/${SOURCE_DIR#"$PROJECT_DIR/"}"
D_BUILD="/ppap/${BUILD_DIR#"$PROJECT_DIR/"}"
D_EXTRA_ARGS=()
for arg in "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"; do
    D_EXTRA_ARGS+=("${arg//"$PROJECT_DIR"/"/ppap"}")
done
docker_run "$DOCKER_IMAGE" bash -c "
    cmake ${D_EXTRA_ARGS[*]+"${D_EXTRA_ARGS[*]}"} \
          -DPPAP_TESTS=$TESTS \
          -DPPAP_TESTS_EXTENDED=$TESTS_EXTENDED \
          -DH68K_DEBUG=$H68K_DEBUG \
          -S '$D_SOURCE' -B '$D_BUILD' >/dev/null 2>&1 && \
    cmake --build '$D_BUILD' -- -j\$(nproc)
"

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

run_source_checks

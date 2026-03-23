#!/usr/bin/env bash
# build.sh — Build a PPAP target
#
# Usage:
#   ./scripts/build.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc, pico2, qemu_arm, qemu_m68k, x68k
#
# Options:
#   --test              Enable PPAP_TESTS (kernel + userland test suite)
#   --test-extended     Enable PPAP_TESTS + PPAP_TESTS_EXTENDED
#   --clean             Remove build directory before building (full rebuild)
#   --overlay=<dir>     Extra overlay directory copied into romfs (highest priority)
#   --h68k-debug        Enable kernel Human68k debug diagnostics
#
# Examples:
#   ./scripts/build.sh pico1              # build pico1
#   ./scripts/build.sh pico1calc          # build pico1calc
#   ./scripts/build.sh --test qemu_arm    # build qemu_arm with tests
#   ./scripts/build.sh --test-extended qemu_arm  # build qemu_arm with extended tests
#   ./scripts/build.sh --clean qemu_m68k  # clean rebuild m68k
#   ./scripts/build.sh qemu_m68k          # build m68k QEMU target
#   ./scripts/build.sh x68k               # build X68000 target (Phase X-1)
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
        ibmpc)                          echo "ppap/ia16" ;;
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
    ibmpc)
        SOURCE_DIR="$PROJECT_DIR/src/target/ibmpc"
        BUILD_DIR="$PROJECT_DIR/build/ibmpc"
        ;;
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, pico2, pico2rv, qemu_arm, qemu_rv32, qemu_m68k, x68k, xtensa_cc, ibmpc"
        exit 1
        ;;
esac

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
                M68K_LD=/ppap/src/user/arch/m68k/user.ld && \
                M68K_LIBGCC=\$(\$M68K_CC -m68000 -print-libgcc-file-name) && \
                \$M68K_CC -m68000 -c -o /ppap/build/shared_m68k/hello_m68k.o \
                    /ppap/src/user/arch/m68k/hello.S && \
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
    XTENSA_TC_DIR="$PROJECT_DIR/tools/xtensa-toolchain"
    ESP_IDF_DIR="${IDF_PATH:-$PROJECT_DIR/third_party/esp-idf}"
    if [[ ! -f "$ESP_IDF_DIR/export.sh" ]]; then
        # Not on host — run the entire xtensa build inside Docker
        DOCKER_IMAGE="$(target_docker_image xtensa_cc)"
        if [[ -n "$DOCKER_IMAGE" ]] && docker_image_exists "$DOCKER_IMAGE"; then
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
        echo "[build] Error: ESP-IDF not found. Run: ./scripts/setup_docker.sh xtensa"
        exit 1
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
    USER_ARCH_DIR="$PROJECT_DIR/src/user/arch/xtensa"

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
    XTENSA_USER_FLAGS="$XTENSA_DYNCONFIG -mabi=call0 -mlongcalls \
        -ffreestanding -nostdlib -Os -fPIC -Wl,--emit-relocs \
        -I$USER_DIR -I$PROJECT_DIR/src -T $USER_ARCH_DIR/user.ld \
        $USER_ARCH_DIR/crt0.S $USER_ARCH_DIR/syscall.S"

    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_USER_FLAGS "$USER_DIR/hello.c" \
        -o "$BUILD_DIR/user/hello.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_USER_FLAGS "$USER_DIR/init.c" \
        -o "$BUILD_DIR/user/init.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_USER_FLAGS "$USER_DIR/getty.c" \
        -o "$BUILD_DIR/user/getty.elf"
    # shellcheck disable=SC2086
    $XTENSA_CC $XTENSA_USER_FLAGS "$USER_DIR/push.c" "$USER_DIR/push_line.c" \
        -o "$BUILD_DIR/user/push.elf"

    # Strip debug symbols (keep relocation info)
    for f in "$BUILD_DIR"/user/*.elf; do
        $XTENSA_STRIP --strip-debug "$f"
    done

    # Stage romfs directory
    rm -rf "$ROMFS_STAGING"
    mkdir -p "$ROMFS_STAGING"/{bin,sbin,etc,dev,proc,tmp}
    cp "$BUILD_DIR/user/init.elf"  "$ROMFS_STAGING/sbin/init"
    cp "$BUILD_DIR/user/hello.elf" "$ROMFS_STAGING/bin/hello"
    cp "$BUILD_DIR/user/getty.elf" "$ROMFS_STAGING/bin/getty"
    cp "$BUILD_DIR/user/push.elf"  "$ROMFS_STAGING/bin/push"
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

echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS, PPAP_TESTS_EXTENDED=$TESTS_EXTENDED)..."

# Convert host-absolute paths to container-relative for Docker builds
DOCKER_IMAGE="$(target_docker_image "$TARGET")"
if [[ -n "$DOCKER_IMAGE" ]] && docker_image_exists "$DOCKER_IMAGE"; then
    echo "[build] Using Docker image $DOCKER_IMAGE"
    # Paths inside container: /ppap is the project root
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
else
    cmake "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
          -DPPAP_TESTS="$TESTS" \
          -DPPAP_TESTS_EXTENDED="$TESTS_EXTENDED" \
          -DH68K_DEBUG="$H68K_DEBUG" \
          -S "$SOURCE_DIR" -B "$BUILD_DIR" >/dev/null 2>&1
    cmake --build "$BUILD_DIR" -- -j"$(nproc)"
fi

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

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
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, pico2, pico2rv, qemu_arm, qemu_rv32, qemu_m68k, x68k, xtensa_cc"
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

# ── Build ────────────────────────────────────────────────────────────────────
EXTRA_ARGS=(-DPPAP_EXTRA_OVERLAY=)
case "$TARGET" in
    qemu_arm)
        # Bare-metal ARM (no Pico SDK) — needs explicit toolchain file
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_arm_m.cmake")
        ;;
    qemu_m68k|x68k)
        # Ensure custom m68k-elf toolchain is available
        M68K_TC="$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-gcc"
        if [[ ! -x "$M68K_TC" ]]; then
            echo "[build] m68k-elf-gcc not found. Building toolchain..."
            "$PROJECT_DIR/third_party/build_gcc_m68k.sh"
        fi
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain_m68k.cmake")
        ;;
esac

# ── ESP-IDF build (xtensa_cc) ────────────────────────────────────────────────
if [[ "$TARGET" == "xtensa_cc" ]]; then
    XTENSA_TC_DIR="$PROJECT_DIR/tools/xtensa-toolchain"
    ESP_IDF_DIR="$PROJECT_DIR/third_party/esp-idf"
    if [[ ! -f "$ESP_IDF_DIR/export.sh" ]]; then
        echo "[build] Error: ESP-IDF not found. Run: ./scripts/setup_toolchain.sh"
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
cmake "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
      -DPPAP_TESTS="$TESTS" \
      -DPPAP_TESTS_EXTENDED="$TESTS_EXTENDED" \
      -DH68K_DEBUG="$H68K_DEBUG" \
      -S "$SOURCE_DIR" -B "$BUILD_DIR" >/dev/null 2>&1
cmake --build "$BUILD_DIR" -- -j"$(nproc)"

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

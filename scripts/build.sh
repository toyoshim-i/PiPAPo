#!/usr/bin/env bash
# build.sh — Build a PPAP target
#
# Usage:
#   ./scripts/build.sh [OPTIONS] TARGET
#
# TARGET is one of: pico1, pico1calc, qemu_arm, qemu_m68k, host_qemu
#
# Options:
#   --test    Enable PPAP_TESTS (kernel integration tests + userland test suite)
#   --clean   Remove build directory before building (full rebuild)
#
# Examples:
#   ./scripts/build.sh pico1              # build pico1
#   ./scripts/build.sh pico1calc          # build pico1calc
#   ./scripts/build.sh --test qemu_arm    # build qemu_arm with tests
#   ./scripts/build.sh --clean qemu_m68k  # clean rebuild m68k
#   ./scripts/build.sh host_qemu          # build QEMU m68k emulator from source

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Parse arguments ──────────────────────────────────────────────────────────
TESTS=OFF
CLEAN=0
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --test)   TESTS=ON ;;
        --clean)  CLEAN=1 ;;
        -*)       echo "Unknown option: $arg" >&2; exit 1 ;;
        *)        TARGET="$arg" ;;
    esac
done

# Show usage if no target specified
if [[ -z "$TARGET" ]]; then
    sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
    exit 0
fi

# ── Determine build directory ────────────────────────────────────────────────
case "$TARGET" in
    pico1|pico1calc|qemu_arm) BUILD_DIR="$PROJECT_DIR/build/arm_m" ;;
    qemu_m68k)                BUILD_DIR="$PROJECT_DIR/build/m68k" ;;
    host_qemu)                BUILD_DIR="$PROJECT_DIR/third_party/qemu/build" ;;
    *)
        echo "[build] Error: unknown target '$TARGET'"
        echo "        Valid targets: pico1, pico1calc, qemu_arm, qemu_m68k, host_qemu"
        exit 1
        ;;
esac

# ── Clean build directory if requested ───────────────────────────────────────
if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "[build] Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# ── Target-specific build ────────────────────────────────────────────────────
case "$TARGET" in
    pico1|pico1calc|qemu_arm)
        BUILD_DIR="$PROJECT_DIR/build/arm_m"
        CMAKE_TARGET="ppap_${TARGET}"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

        echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS)..."
        cmake -B "$BUILD_DIR" -DPPAP_TESTS="$TESTS" "$PROJECT_DIR" >/dev/null 2>&1
        cmake --build "$BUILD_DIR" --target "$CMAKE_TARGET" -- -j"$(nproc)"
        ;;
    qemu_m68k)
        BUILD_DIR="$PROJECT_DIR/build/m68k"
        CMAKE_TARGET="ppap_qemu_m68k"
        ELF="$BUILD_DIR/${CMAKE_TARGET}.elf"

        # Ensure custom m68k-elf toolchain is available
        M68K_TC="$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-gcc"
        if [[ ! -x "$M68K_TC" ]]; then
            echo "[build] m68k-elf-gcc not found. Building toolchain..."
            "$PROJECT_DIR/third_party/build-gcc-m68k.sh"
        fi

        echo "[build] Building $CMAKE_TARGET (PPAP_TESTS=$TESTS)..."
        cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchain-m68k.cmake" \
              -DPPAP_TESTS="$TESTS" \
              -S "$PROJECT_DIR/src/target/qemu_m68k" -B "$BUILD_DIR" >/dev/null 2>&1
        cmake --build "$BUILD_DIR" -- -j"$(nproc)"
        ;;
    host_qemu)
        QEMU_SRC="$PROJECT_DIR/third_party/qemu"
        QEMU_M68K="$BUILD_DIR/qemu-system-m68k"

        # Already built?
        if [[ -x "$QEMU_M68K" ]]; then
            echo "[build] QEMU m68k already built: $QEMU_M68K"
            "$QEMU_M68K" --version | head -1
            exit 0
        fi

        # Initialise submodule if needed
        if [[ ! -f "$QEMU_SRC/meson.build" ]]; then
            echo "[build] Initialising QEMU submodule..."
            git -C "$PROJECT_DIR" submodule update --init third_party/qemu
        fi

        # Install build dependencies if missing
        BUILD_DEPS=(meson ninja-build libpixman-1-dev libglib2.0-dev pkg-config python3-venv)
        MISSING=()
        for pkg in "${BUILD_DEPS[@]}"; do
            dpkg -s "$pkg" &>/dev/null || MISSING+=("$pkg")
        done
        if [[ ${#MISSING[@]} -gt 0 ]]; then
            echo "[build] Installing build dependencies: ${MISSING[*]}"
            sudo apt-get update -qq
            sudo apt-get install -y "${MISSING[@]}"
        fi

        echo "[build] Configuring QEMU (m68k-softmmu only)..."
        cd "$QEMU_SRC"
        ./configure --target-list=m68k-softmmu \
                    --disable-docs --disable-tools --disable-guest-agent \
                    --prefix="$BUILD_DIR/install" 2>&1 | tail -3

        echo "[build] Building QEMU (this may take a few minutes)..."
        make -j"$(nproc)" 2>&1 | tail -5

        if [[ -x "$QEMU_M68K" ]]; then
            echo "[build] QEMU m68k built: $QEMU_M68K"
            "$QEMU_M68K" --version | head -1
        else
            echo "[build] Error: QEMU m68k build failed"
            exit 1
        fi
        exit 0
        ;;
esac

if [[ ! -f "$ELF" ]]; then
    echo "[build] Error: $ELF not found after build."
    exit 1
fi

echo "[build] Built $ELF"

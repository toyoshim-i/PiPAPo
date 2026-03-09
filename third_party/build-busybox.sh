#!/bin/bash
# Build busybox variants for PiPAPo
#
# This script builds BusyBox variants:
#   1. busybox       — full (all applets) for transient commands
#   2. busybox.sh    — shell + builtins only (interactive shell, resident)
#
# Each variant shares musl libc, libgcc, and linker script; only the
# .config (applet selection) differs.  -ffunction-sections + -fdata-sections
# enable dead-code stripping in the split binaries.
#
# Usage: ./third_party/build-busybox.sh [--m68k] [--clean]
#   --m68k    Build for m68k (default: ARM Cortex-M0+)
#   --clean   Remove build artifacts and exit
#
# Prerequisites:
#   - Cross compiler (arm-none-eabi-gcc or m68k-elf-gcc from build-gcc-m68k.sh)
#   - musl sysroot already built (run build-musl.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BB_SRC="$SCRIPT_DIR/busybox"
CONFIGS_DIR="$SCRIPT_DIR/configs"

# --- Parse flags ---
ARCH=arm
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --m68k) ARCH=m68k ;;
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Arch-specific configuration ---
if [[ "$ARCH" == "m68k" ]]; then
    M68K_TC="$PROJECT_ROOT/tools/m68k-toolchain"
    BB_OUT="$PROJECT_ROOT/build/m68k/busybox"
    MUSL_SYSROOT="$PROJECT_ROOT/build/m68k/musl-sysroot"
    SPECS_FILE="$PROJECT_ROOT/build/m68k/musl-m68k.specs"
    CC="${M68K_TC}/bin/m68k-elf-gcc"
    CROSS_PREFIX="${M68K_TC}/bin/m68k-elf-"
    STRIP="${M68K_TC}/bin/m68k-elf-strip"
    SIZE_CMD="${M68K_TC}/bin/m68k-elf-size"
    BB_ARCH=m68k
    GCC_INCLUDE="$($CC -print-file-name=include)"
    GCC_LIBDIR="$(dirname "$($CC -m68000 -print-libgcc-file-name)")"
    BUSYBOX_LD="$CONFIGS_DIR/busybox-m68k.ld"
    CFLAGS_PPAP="-m68000 -Os -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE -msep-data -ffunction-sections -fdata-sections -pie"
    ARCH_LABEL="m68k (68000)"
else
    BB_OUT="$PROJECT_ROOT/build/arm_m/busybox"
    MUSL_SYSROOT="$PROJECT_ROOT/build/arm_m/musl-sysroot"
    SPECS_FILE="$PROJECT_ROOT/build/arm_m/musl-arm.specs"
    CC=arm-none-eabi-gcc
    CROSS_PREFIX=arm-none-eabi-
    STRIP=arm-none-eabi-strip
    SIZE_CMD=arm-none-eabi-size
    BB_ARCH=arm
    GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
    GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"
    BUSYBOX_LD="$CONFIGS_DIR/busybox.ld"
    CFLAGS_PPAP="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections -pie"
    ARCH_LABEL="armv6m-thumb (Cortex-M0+)"
fi

# Variant definitions: output_name:fragment_file
VARIANTS=(
    "busybox:busybox_ppap.fragment"
    "busybox.sh:busybox_sh.fragment"
)

# --- Handle --clean ---
if $CLEAN; then
    echo "busybox [$ARCH]: cleaning build artifacts..."
    cd "$BB_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$BB_OUT" "$SPECS_FILE"
    echo "busybox [$ARCH]: clean done."
    exit 0
fi

# --- Skip if all variants already built ---
all_built=true
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    if [[ ! -f "$BB_OUT/$name" ]]; then
        all_built=false
        break
    fi
done
if $all_built; then
    echo "busybox [$ARCH]: all variants already exist at $BB_OUT — skipping."
    echo "busybox [$ARCH]: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v "$CC" &>/dev/null; then
    echo "ERROR: $CC not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "ERROR: musl sysroot not found at $MUSL_SYSROOT" >&2
    echo "  Run: ./third_party/build-musl.sh${ARCH:+ --$ARCH}" >&2
    exit 1
fi

if [[ ! -f "$BB_SRC/Makefile" ]]; then
    echo "ERROR: busybox submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/busybox" >&2
    exit 1
fi

# --- Generate musl specs file ---
echo "busybox [$ARCH]: generating musl specs file..."
mkdir -p "$(dirname "$SPECS_FILE")"
cat > "$SPECS_FILE" <<SPECS
*startfile:
$MUSL_SYSROOT/lib/crt1.o $MUSL_SYSROOT/lib/crti.o

*endfile:
$MUSL_SYSROOT/lib/crtn.o

*lib:
$MUSL_SYSROOT/lib/libc.a

*libgcc:
$GCC_LIBDIR/libgcc.a
SPECS
echo "  specs: $SPECS_FILE"

mkdir -p "$BB_OUT"

# --- Build each variant ---
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    fragment="${variant#*:}"

    # Skip if this variant already exists
    if [[ -f "$BB_OUT/$name" ]]; then
        echo "busybox [$name]: already exists — skipping."
        continue
    fi

    echo ""
    echo "========================================"
    echo "busybox [$name] ($ARCH_LABEL): building..."
    echo "========================================"

    # Restore submodule to clean state
    cd "$BB_SRC"
    git checkout . 2>/dev/null || true
    git clean -fdx 2>/dev/null || true

    # Generate .config from allnoconfig + fragment
    echo "busybox [$name]: generating .config from $fragment..."
    make allnoconfig ARCH="$BB_ARCH" 2>&1 | tail -1

    # Apply fragment: for each CONFIG_FOO=y line, enable it in .config
    while IFS= read -r line; do
        # Skip comments and empty lines
        [[ "$line" =~ ^# ]] && continue
        [[ -z "$line" ]] && continue

        key="${line%%=*}"

        # Replace "# CONFIG_FOO is not set" with "CONFIG_FOO=y"
        # or replace existing CONFIG_FOO=n with CONFIG_FOO=y
        if grep -q "# ${key} is not set" .config 2>/dev/null; then
            sed -i "s/# ${key} is not set/${line}/" .config
        elif grep -q "^${key}=" .config 2>/dev/null; then
            sed -i "s/^${key}=.*/${line}/" .config
        else
            echo "$line" >> .config
        fi
    done < "$CONFIGS_DIR/$fragment"

    # Inject CFLAGS into .config
    sed -i 's|^CONFIG_SYSROOT=.*|CONFIG_SYSROOT=""|' .config
    sed -i 's|^CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="'"$CFLAGS_PPAP -specs=$SPECS_FILE -T $BUSYBOX_LD"'"|' .config
    sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS=""|' .config
    sed -i 's|^CONFIG_EXTRA_LDLIBS=.*|CONFIG_EXTRA_LDLIBS=""|' .config

    # Resolve dependencies
    echo "" | make oldconfig ARCH="$BB_ARCH" 2>&1 | tail -3

    echo "busybox [$name]: enabled applets:"
    grep '=y' .config | grep -v '^#' | grep -v '_FEATURE_\|_STATIC\|_NOMMU\|_LFS\|_CROSS\|_PREFIX\|_EXTRA\|_SH_\|_PREFER\|_OPTIMIZE\|_INTERNAL\|_BUILTIN\|_ALIAS\|_CMDCMD' | sed 's/CONFIG_/  /' | sort

    # Build
    echo "busybox [$name]: compiling (musl, $ARCH_LABEL)..."
    make ARCH="$BB_ARCH" \
        CROSS_COMPILE="$CROSS_PREFIX" \
        SKIP_STRIP=y \
        -j"$(nproc)" 2>&1

    # Strip debug info and copy to output
    $STRIP -s -o "$BB_OUT/$name" busybox
    echo "busybox [$name]: installed to $BB_OUT/$name"
done

# --- Restore submodule ---
echo ""
echo "busybox [$ARCH]: restoring submodule to clean state..."
cd "$BB_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify all variants ---
echo ""
echo "busybox [$ARCH]: build summary"
echo "========================================"
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    if [[ -f "$BB_OUT/$name" ]]; then
        SIZE=$(stat -c%s "$BB_OUT/$name" 2>/dev/null || stat -f%z "$BB_OUT/$name")
        echo "  $name: $SIZE bytes"
        $SIZE_CMD "$BB_OUT/$name" 2>/dev/null | tail -1 | sed 's/^/    /'
    else
        echo "ERROR: $name not found after build" >&2
        exit 1
    fi
done
echo "busybox [$ARCH]: SUCCESS — all variants built."

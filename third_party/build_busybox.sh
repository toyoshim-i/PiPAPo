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
# Normally invoked from cmake via user.cmake (PPAP_CONFIG set).
# Standalone: ./third_party/build_busybox.sh [--m68k] [--clean]
#
# Prerequisites:
#   - Cross compiler (arm-none-eabi-gcc or m68k-elf-gcc)
#   - musl sysroot already built (run build_musl.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BB_SRC="$SCRIPT_DIR/busybox"
CONFIGS_DIR="$SCRIPT_DIR/patches/busybox"

# --- Parse flags ---
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --m68k) PPAP_ARCH=m68k ;;
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Source cmake-generated config if available ---
if [[ -n "${PPAP_CONFIG:-}" && -f "$PPAP_CONFIG" ]]; then
    source "$PPAP_CONFIG"
elif [[ -z "${PPAP_ARCH:-}" ]]; then
    PPAP_ARCH=arm
fi

# --- Derive variables from PPAP_ARCH (fallback for standalone use) ---
if [[ -z "${PPAP_CC:-}" ]]; then
    if [[ "$PPAP_ARCH" == "m68k" ]]; then
        M68K_TC="$PROJECT_ROOT/tools/m68k-toolchain"
        PPAP_CC="${M68K_TC}/bin/m68k-elf-gcc"
        PPAP_CROSS_PREFIX="${M68K_TC}/bin/m68k-elf-"
        PPAP_STRIP="${M68K_TC}/bin/m68k-elf-strip"
        PPAP_SIZE_CMD="${M68K_TC}/bin/m68k-elf-size"
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/m68k/musl-sysroot"
        PPAP_SPECS_FILE="$PROJECT_ROOT/build/m68k/musl-m68k.specs"
        PPAP_BUSYBOX_LD="$SCRIPT_DIR/patches/musl/libc_m68k.ld"
        PPAP_APP_CFLAGS="-m68000 -Os -nostdinc -isystem $PPAP_MUSL_SYSROOT/include -isystem $($PPAP_CC -print-file-name=include) -msep-data -ffunction-sections -fdata-sections -pie"
        PPAP_GCC_INCLUDE="$($PPAP_CC -print-file-name=include)"
        PPAP_GCC_LIBDIR="$(dirname "$($PPAP_CC -m68000 -print-libgcc-file-name)")"
        PPAP_BB_ARCH=m68k
        PPAP_ARCH_LABEL="m68k (68000)"
    else
        PPAP_CC=arm-none-eabi-gcc
        PPAP_CROSS_PREFIX=arm-none-eabi-
        PPAP_STRIP=arm-none-eabi-strip
        PPAP_SIZE_CMD=arm-none-eabi-size
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/arm_m/musl-sysroot"
        PPAP_SPECS_FILE="$PROJECT_ROOT/build/arm_m/musl-arm.specs"
        PPAP_BUSYBOX_LD="$SCRIPT_DIR/patches/musl/libc_arm_m.ld"
        PPAP_GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
        PPAP_GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"
        PPAP_APP_CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -nostdinc -isystem $PPAP_MUSL_SYSROOT/include -isystem $PPAP_GCC_INCLUDE -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections -pie"
        PPAP_BB_ARCH=arm
        PPAP_ARCH_LABEL="armv6m-thumb (Cortex-M0+)"
    fi
fi

# Derive BB_OUT from PPAP_MUSL_SYSROOT (which reflects the correct shared
# build dir: arm_m vs arm_m33 vs m68k).  Fallback to arch-based path.
if [[ -n "${PPAP_MUSL_SYSROOT:-}" ]]; then
    BB_OUT="$(dirname "$PPAP_MUSL_SYSROOT")/busybox"
else
    BB_OUT="$PROJECT_ROOT/build/${PPAP_ARCH/#arm/arm_m}/busybox"
fi

# Variant definitions: output_name:fragment_file
VARIANTS=(
    "busybox:busybox_ppap.fragment"
    "busybox.sh:busybox_sh.fragment"
)

# --- Handle --clean ---
if $CLEAN; then
    echo "busybox [$PPAP_ARCH]: cleaning build artifacts..."
    cd "$BB_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$BB_OUT" "$PPAP_SPECS_FILE"
    echo "busybox [$PPAP_ARCH]: clean done."
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
    echo "busybox [$PPAP_ARCH]: all variants already exist at $BB_OUT — skipping."
    echo "busybox [$PPAP_ARCH]: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v "$PPAP_CC" &>/dev/null; then
    echo "ERROR: $PPAP_CC not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$PPAP_MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "ERROR: musl sysroot not found at $PPAP_MUSL_SYSROOT" >&2
    echo "  Run: ./third_party/build_musl.sh${PPAP_ARCH:+ --$PPAP_ARCH}" >&2
    exit 1
fi

if [[ ! -f "$BB_SRC/Makefile" ]]; then
    echo "ERROR: busybox submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/busybox" >&2
    exit 1
fi

# --- Generate musl specs file (if not already present from cmake) ---
if [[ ! -f "$PPAP_SPECS_FILE" ]]; then
    echo "busybox [$PPAP_ARCH]: generating musl specs file..."
    mkdir -p "$(dirname "$PPAP_SPECS_FILE")"
    cat > "$PPAP_SPECS_FILE" <<SPECS
*startfile:
$PPAP_MUSL_SYSROOT/lib/crt1.o $PPAP_MUSL_SYSROOT/lib/crti.o

*endfile:
$PPAP_MUSL_SYSROOT/lib/crtn.o

*lib:
$PPAP_MUSL_SYSROOT/lib/libc.a

*libgcc:
$PPAP_GCC_LIBDIR/libgcc.a
SPECS
    echo "  specs: $PPAP_SPECS_FILE"
fi

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
    echo "busybox [$name] ($PPAP_ARCH_LABEL): building..."
    echo "========================================"

    # Restore submodule to clean state
    cd "$BB_SRC"
    git checkout . 2>/dev/null || true
    git clean -fdx 2>/dev/null || true

    # Generate .config from allnoconfig + fragment
    echo "busybox [$name]: generating .config from $fragment..."
    make allnoconfig ARCH="$PPAP_BB_ARCH" 2>&1 | tail -1

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
    sed -i 's|^CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="'"$PPAP_APP_CFLAGS -specs=$PPAP_SPECS_FILE -T $PPAP_BUSYBOX_LD"'"|' .config
    sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS="-L'"$PPAP_MUSL_SYSROOT"'/lib"|' .config
    sed -i 's|^CONFIG_EXTRA_LDLIBS=.*|CONFIG_EXTRA_LDLIBS=""|' .config

    # Resolve dependencies
    echo "" | make oldconfig ARCH="$PPAP_BB_ARCH" 2>&1 | tail -3

    echo "busybox [$name]: enabled applets:"
    grep '=y' .config | grep -v '^#' | grep -v '_FEATURE_\|_STATIC\|_NOMMU\|_LFS\|_CROSS\|_PREFIX\|_EXTRA\|_SH_\|_PREFER\|_OPTIMIZE\|_INTERNAL\|_BUILTIN\|_ALIAS\|_CMDCMD' | sed 's/CONFIG_/  /' | sort

    # Build
    echo "busybox [$name]: compiling (musl, $PPAP_ARCH_LABEL)..."
    make ARCH="$PPAP_BB_ARCH" \
        CROSS_COMPILE="$PPAP_CROSS_PREFIX" \
        SKIP_STRIP=y \
        -j"$(nproc)" 2>&1

    # Strip debug info and copy to output
    $PPAP_STRIP -s -o "$BB_OUT/$name" busybox
    echo "busybox [$name]: installed to $BB_OUT/$name"
done

# --- Restore submodule ---
echo ""
echo "busybox [$PPAP_ARCH]: restoring submodule to clean state..."
cd "$BB_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify all variants ---
echo ""
echo "busybox [$PPAP_ARCH]: build summary"
echo "========================================"
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    if [[ -f "$BB_OUT/$name" ]]; then
        SIZE=$(stat -c%s "$BB_OUT/$name" 2>/dev/null || stat -f%z "$BB_OUT/$name")
        echo "  $name: $SIZE bytes"
        $PPAP_SIZE_CMD "$BB_OUT/$name" 2>/dev/null | tail -1 | sed 's/^/    /'
    else
        echo "ERROR: $name not found after build" >&2
        exit 1
    fi
done
echo "busybox [$PPAP_ARCH]: SUCCESS — all variants built."

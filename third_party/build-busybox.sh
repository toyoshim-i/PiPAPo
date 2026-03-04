#!/bin/bash
# Build busybox variants for PicoPiAndPortable (ARMv6-M Thumb / Cortex-M0+)
#
# This script builds three BusyBox variants:
#   1. busybox       — full (all applets) for transient commands
#   2. busybox.init  — init-only (PID 1 resident)
#   3. busybox.sh    — shell + builtins only (interactive shell, resident)
#
# Each variant shares musl libc, libgcc, and linker script; only the
# .config (applet selection) differs.  -ffunction-sections + -fdata-sections
# enable dead-code stripping in the split binaries.
#
# Usage: ./third_party/build-busybox.sh [--clean]
#   --clean   Remove build artifacts and exit
#
# Prerequisites:
#   - arm-none-eabi-gcc in PATH
#   - musl sysroot already built (run build-musl.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BB_SRC="$SCRIPT_DIR/busybox"
BB_OUT="$PROJECT_ROOT/build/busybox"
MUSL_SYSROOT="$PROJECT_ROOT/build/musl-sysroot"
CONFIGS_DIR="$SCRIPT_DIR/configs"
SPECS_FILE="$PROJECT_ROOT/build/musl-arm.specs"

GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"

# CFLAGS: Thumb-1 target flags + musl headers (before GCC builtins to win stdint.h)
# PIC flags: all data references go through GOT (r9 = GOT base, set by kernel).
# -mno-pic-data-is-text-relative: .text lives in flash (XIP) while .data/.got live in SRAM.
# -T busybox.ld: link at address 0 with text+data PT_LOAD segments (ignored by gcc -c).
# -pie: generate R_ARM_RELATIVE relocations so exec.c can fix up function-pointer
#        arrays in .data (e.g. applet_main[]) whose entries are raw link-time addresses.
# -ffunction-sections -fdata-sections: enable per-function/per-variable sections for
#        dead-code stripping (especially useful for split init/sh binaries).
BUSYBOX_LD="$CONFIGS_DIR/busybox.ld"
CFLAGS_PPAP="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections -pie"

# Variant definitions: output_name:fragment_file
VARIANTS=(
    "busybox:busybox_ppap.fragment"
    "busybox.init:busybox_init.fragment"
    "busybox.sh:busybox_sh.fragment"
)

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "busybox: cleaning build artifacts..."
    cd "$BB_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$BB_OUT" "$SPECS_FILE"
    echo "busybox: clean done."
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
    echo "busybox: all variants already exist at $BB_OUT — skipping."
    echo "busybox: run '$0 --clean' to force rebuild."
    exit 0
fi

# --- Check prerequisites ---
if ! command -v arm-none-eabi-gcc &>/dev/null; then
    echo "ERROR: arm-none-eabi-gcc not found in PATH" >&2
    exit 1
fi

if [[ ! -f "$MUSL_SYSROOT/lib/libc.a" ]]; then
    echo "ERROR: musl sysroot not found at $MUSL_SYSROOT" >&2
    echo "  Run: ./third_party/build-musl.sh" >&2
    exit 1
fi

if [[ ! -f "$BB_SRC/Makefile" ]]; then
    echo "ERROR: busybox submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/busybox" >&2
    exit 1
fi

# --- Generate musl specs file ---
# Override GCC's default CRT and library specs to use musl instead of newlib.
# This is the cleanest way to cross-compile against musl with arm-none-eabi-gcc:
# the specs file replaces startfile/endfile/lib/libgcc so GCC automatically
# uses the correct CRT objects, libc, and libgcc for linking.
echo "busybox: generating musl specs file..."
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
    echo "busybox [$name]: building..."
    echo "========================================"

    # Restore submodule to clean state
    cd "$BB_SRC"
    git checkout . 2>/dev/null || true
    git clean -fdx 2>/dev/null || true

    # Generate .config from allnoconfig + fragment
    echo "busybox [$name]: generating .config from $fragment..."
    make allnoconfig ARCH=arm 2>&1 | tail -1

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

    # Inject CFLAGS into .config (paths are build-time specific).
    # -specs= goes in CFLAGS (used by gcc, not by ld -r for partial links).
    # It overrides GCC's default CRT/lib to use musl instead of newlib.
    # CONFIG_EXTRA_LDFLAGS must stay empty because it feeds LDFLAGS which is
    # shared between "ld -r" partial links and the final gcc link.
    sed -i 's|^CONFIG_SYSROOT=.*|CONFIG_SYSROOT=""|' .config
    sed -i 's|^CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="'"$CFLAGS_PPAP -specs=$SPECS_FILE -T $BUSYBOX_LD"'"|' .config
    sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS=""|' .config
    sed -i 's|^CONFIG_EXTRA_LDLIBS=.*|CONFIG_EXTRA_LDLIBS=""|' .config

    # Resolve dependencies
    echo "" | make oldconfig ARCH=arm 2>&1 | tail -3

    echo "busybox [$name]: enabled applets:"
    grep '=y' .config | grep -v '^#' | grep -v '_FEATURE_\|_STATIC\|_NOMMU\|_LFS\|_CROSS\|_PREFIX\|_EXTRA\|_SH_\|_PREFER\|_OPTIMIZE\|_INTERNAL\|_BUILTIN\|_ALIAS\|_CMDCMD' | sed 's/CONFIG_/  /' | sort

    # Build
    echo "busybox [$name]: compiling (musl, Cortex-M0+)..."
    make ARCH=arm \
        CROSS_COMPILE=arm-none-eabi- \
        SKIP_STRIP=y \
        -j"$(nproc)" 2>&1

    # Strip debug info and copy to output.
    # Removes .debug_*, .symtab, .strtab — saves ~80% file size.
    # PT_LOAD segments (.text, .data, .rel.dyn, .got) are preserved.
    arm-none-eabi-strip -s -o "$BB_OUT/$name" busybox
    echo "busybox [$name]: installed to $BB_OUT/$name"
done

# --- Restore submodule ---
echo ""
echo "busybox: restoring submodule to clean state..."
cd "$BB_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify all variants ---
echo ""
echo "busybox: build summary"
echo "========================================"
for variant in "${VARIANTS[@]}"; do
    name="${variant%%:*}"
    if [[ -f "$BB_OUT/$name" ]]; then
        SIZE=$(stat -c%s "$BB_OUT/$name" 2>/dev/null || stat -f%z "$BB_OUT/$name")
        echo "  $name: $SIZE bytes"
        arm-none-eabi-size "$BB_OUT/$name" 2>/dev/null | tail -1 | sed 's/^/    /'
    else
        echo "ERROR: $name not found after build" >&2
        exit 1
    fi
done
echo "busybox: SUCCESS — all variants built."

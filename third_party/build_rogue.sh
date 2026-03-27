#!/bin/bash
# Build Rogue 5.4.4 for PiPAPo
#
# Cross-compiles rogue against musl libc with a minimal curses shim.
# xcrypt.c is excluded (71 KB BSS for DES tables, only used by disabled
# wizard mode); a stub xcrypt() is provided in the curses shim.
#
# Normally invoked from cmake via user.cmake (PPAP_CONFIG set).
# Standalone: ./third_party/build_rogue.sh [--m68k] [--clean]
#
# Prerequisites:
#   - Cross compiler (arm-none-eabi-gcc or m68k-elf-gcc)
#   - musl sysroot already built (run build_musl.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ROGUE_SRC="$SCRIPT_DIR/rogue"
PATCHES_DIR="$SCRIPT_DIR/patches/rogue"
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
        PPAP_CC="m68k-elf-gcc"
        PPAP_STRIP="m68k-elf-strip"
        PPAP_SIZE_CMD="m68k-elf-size"
        PPAP_TARGET_FLAGS="-m68000"
        PPAP_PIC_FLAGS="-msep-data"
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/m68k/musl-sysroot"
        PPAP_SPECS_FILE="$PROJECT_ROOT/build/m68k/musl-m68k.specs"
        PPAP_BUSYBOX_LD="$SCRIPT_DIR/patches/musl/libc_m68k.ld"
        PPAP_GCC_INCLUDE="$($PPAP_CC -print-file-name=include)"
        PPAP_GCC_LIBDIR="$(dirname "$($PPAP_CC -m68000 -print-libgcc-file-name)")"
        PPAP_ARCH_LABEL="m68k (68000)"
    else
        PPAP_CC=arm-none-eabi-gcc
        PPAP_STRIP=arm-none-eabi-strip
        PPAP_SIZE_CMD=arm-none-eabi-size
        PPAP_TARGET_FLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft"
        PPAP_PIC_FLAGS="-fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"
        PPAP_MUSL_SYSROOT="$PROJECT_ROOT/build/arm_m/musl-sysroot"
        PPAP_SPECS_FILE="$PROJECT_ROOT/build/arm_m/musl-arm.specs"
        PPAP_BUSYBOX_LD="$SCRIPT_DIR/patches/musl/libc_arm_m.ld"
        PPAP_GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
        PPAP_GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"
        PPAP_ARCH_LABEL="armv6m-thumb (Cortex-M0+)"
    fi
fi

# Derive ROGUE_OUT from PPAP_MUSL_SYSROOT (correct shared build dir).
if [[ -n "${PPAP_MUSL_SYSROOT:-}" ]]; then
    ROGUE_OUT="$(dirname "$PPAP_MUSL_SYSROOT")/rogue"
else
    ROGUE_OUT="$PROJECT_ROOT/build/${PPAP_ARCH/#arm/arm_m}/rogue"
fi

# Build CFLAGS
CFLAGS="$PPAP_TARGET_FLAGS -Os"
# Our curses shim / config.h / pwd.h override system headers (must come first)
CFLAGS="$CFLAGS -nostdinc -isystem $PATCHES_DIR -isystem $PPAP_MUSL_SYSROOT/include -isystem $PPAP_GCC_INCLUDE"
# PIC for PPAP's XIP model
CFLAGS="$CFLAGS $PPAP_PIC_FLAGS"
# Dead-code stripping
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
# Rogue uses autoconf-style HAVE_CONFIG_H
CFLAGS="$CFLAGS -DHAVE_CONFIG_H"
# Suppress upstream warnings we can't fix
CFLAGS="$CFLAGS -Wall -Wno-bool-operation -Wno-misleading-indentation -Wno-unused-variable -std=gnu11"

# Rogue source files (all .c except xcrypt.c)
ROGUE_SRCS=(
    armor.c chase.c command.c daemon.c daemons.c extern.c fight.c
    init.c io.c list.c mach_dep.c main.c mdport.c misc.c monsters.c
    move.c new_level.c options.c pack.c passages.c potions.c rings.c
    rip.c rooms.c save.c scrolls.c state.c sticks.c things.c vers.c
    weapons.c wizard.c
)

# --- Handle --clean ---
if $CLEAN; then
    echo "rogue [$PPAP_ARCH]: cleaning build artifacts..."
    rm -rf "$ROGUE_OUT"
    echo "rogue [$PPAP_ARCH]: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$ROGUE_OUT/rogue" ]]; then
    echo "rogue [$PPAP_ARCH]: already exists at $ROGUE_OUT/rogue — skipping."
    echo "rogue [$PPAP_ARCH]: run '$0 --clean' to force rebuild."
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

if [[ ! -f "$ROGUE_SRC/main.c" ]]; then
    echo "ERROR: rogue submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/rogue" >&2
    exit 1
fi

# --- Generate musl specs file (if not already present from cmake) ---
if [[ ! -f "$PPAP_SPECS_FILE" ]]; then
    echo "rogue [$PPAP_ARCH]: generating musl specs file..."
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
fi

mkdir -p "$ROGUE_OUT/obj"

# --- Compile rogue sources ---
echo "rogue [$PPAP_ARCH]: compiling (musl, $PPAP_ARCH_LABEL)..."
OBJS=()
for src in "${ROGUE_SRCS[@]}"; do
    obj="$ROGUE_OUT/obj/${src%.c}.o"
    OBJS+=("$obj")
    $PPAP_CC $CFLAGS -c "$ROGUE_SRC/$src" -o "$obj"
done

# Compile curses shim
echo "rogue [$PPAP_ARCH]: compiling curses shim..."
$PPAP_CC $CFLAGS -c "$PATCHES_DIR/curses.c" -o "$ROGUE_OUT/obj/curses.o"
OBJS+=("$ROGUE_OUT/obj/curses.o")

# --- Link ---
# Use -specs= to provide musl CRT/libc/libgcc (do NOT use -nostdlib,
# which overrides specs startfile/endfile).  -pie emits relocations
# for exec.c to patch at load time.
echo "rogue [$PPAP_ARCH]: linking..."
LINK_FLAGS="$PPAP_TARGET_FLAGS"
if [[ "${PPAP_RISCV_EPIC:-}" == "ON" ]]; then
    # ePIC clang + lld (link flags from user.cmake)
    $PPAP_CC $LINK_FLAGS $PPAP_EPIC_LINK_FLAGS \
        $PPAP_EPIC_LINK_PRE \
        "${OBJS[@]}" \
        $PPAP_EPIC_LINK_POST \
        -o "$ROGUE_OUT/rogue.elf"
else
    if [[ -n "${PPAP_PIE_FLAG:-}" ]]; then
        LINK_FLAGS="$LINK_FLAGS $PPAP_PIE_FLAG"
    elif [[ "$PPAP_ARCH" != "riscv" ]]; then
        LINK_FLAGS="$LINK_FLAGS -pie"
    fi
    if [[ "$PPAP_ARCH" == "riscv" && -z "${PPAP_PIE_FLAG:-}" ]]; then
        LINK_FLAGS="$LINK_FLAGS -Wl,--emit-relocs -Wl,--no-relax"
    fi
    $PPAP_CC $LINK_FLAGS \
        -specs="$PPAP_SPECS_FILE" \
        -T "$PPAP_BUSYBOX_LD" \
        -Wl,--gc-sections \
        "${OBJS[@]}" \
        -o "$ROGUE_OUT/rogue.elf"
fi

# --- Strip ---
$PPAP_STRIP -s -o "$ROGUE_OUT/rogue" "$ROGUE_OUT/rogue.elf"

# --- Summary ---
echo ""
echo "rogue [$PPAP_ARCH]: build summary"
echo "========================================"
SIZE=$(stat -c%s "$ROGUE_OUT/rogue" 2>/dev/null || stat -f%z "$ROGUE_OUT/rogue")
echo "  stripped ELF: $SIZE bytes"
$PPAP_SIZE_CMD "$ROGUE_OUT/rogue.elf" 2>/dev/null | tail -1 | sed 's/^/  /'
echo "rogue [$PPAP_ARCH]: SUCCESS — installed to $ROGUE_OUT/rogue"

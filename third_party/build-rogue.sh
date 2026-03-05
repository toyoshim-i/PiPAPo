#!/bin/bash
# Build Rogue 5.4.4 for PicoPiAndPortable (ARMv6-M Thumb / Cortex-M0+)
#
# Cross-compiles rogue against musl libc with a minimal curses shim.
# xcrypt.c is excluded (71 KB BSS for DES tables, only used by disabled
# wizard mode); a stub xcrypt() is provided in the curses shim.
#
# Usage: ./third_party/build-rogue.sh [--clean]
#   --clean   Remove build artifacts and exit
#
# Prerequisites:
#   - arm-none-eabi-gcc in PATH
#   - musl sysroot already built (run build-musl.sh first)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ROGUE_SRC="$SCRIPT_DIR/rogue"
ROGUE_OUT="$PROJECT_ROOT/build/rogue"
MUSL_SYSROOT="$PROJECT_ROOT/build/musl-sysroot"
PATCHES_DIR="$SCRIPT_DIR/patches/rogue"
CONFIGS_DIR="$SCRIPT_DIR/configs"

CC=arm-none-eabi-gcc
GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"
SPECS_FILE="$PROJECT_ROOT/build/musl-arm.specs"
LINKER_SCRIPT="$CONFIGS_DIR/busybox.ld"

# Target flags (same as busybox build)
CFLAGS="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os"
# musl headers before GCC builtins
CFLAGS="$CFLAGS -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE"
# PIC for PPAP's XIP-from-flash model
CFLAGS="$CFLAGS -fPIC -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative"
# Dead-code stripping
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
# Our curses shim / config.h / pwd.h override system headers
CFLAGS="$CFLAGS -isystem $PATCHES_DIR"
# Rogue uses autoconf-style HAVE_CONFIG_H
CFLAGS="$CFLAGS -DHAVE_CONFIG_H"
# Suppress upstream warnings we can't fix
CFLAGS="$CFLAGS -Wall -Wno-bool-operation -Wno-misleading-indentation -Wno-unused-variable"

# Rogue source files (all .c except xcrypt.c)
ROGUE_SRCS=(
    armor.c chase.c command.c daemon.c daemons.c extern.c fight.c
    init.c io.c list.c mach_dep.c main.c mdport.c misc.c monsters.c
    move.c new_level.c options.c pack.c passages.c potions.c rings.c
    rip.c rooms.c save.c scrolls.c state.c sticks.c things.c vers.c
    weapons.c wizard.c
)

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "rogue: cleaning build artifacts..."
    rm -rf "$ROGUE_OUT"
    echo "rogue: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$ROGUE_OUT/rogue" ]]; then
    echo "rogue: already exists at $ROGUE_OUT/rogue — skipping."
    echo "rogue: run '$0 --clean' to force rebuild."
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

if [[ ! -f "$ROGUE_SRC/main.c" ]]; then
    echo "ERROR: rogue submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/rogue" >&2
    exit 1
fi

# --- Generate musl specs file (if not already present) ---
if [[ ! -f "$SPECS_FILE" ]]; then
    echo "rogue: generating musl specs file..."
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
fi

mkdir -p "$ROGUE_OUT/obj"

# --- Compile rogue sources ---
echo "rogue: compiling (musl, Cortex-M0+)..."
OBJS=()
for src in "${ROGUE_SRCS[@]}"; do
    obj="$ROGUE_OUT/obj/${src%.c}.o"
    OBJS+=("$obj")
    $CC $CFLAGS -c "$ROGUE_SRC/$src" -o "$obj"
done

# Compile curses shim
echo "rogue: compiling curses shim..."
$CC $CFLAGS -c "$PATCHES_DIR/curses.c" -o "$ROGUE_OUT/obj/curses.o"
OBJS+=("$ROGUE_OUT/obj/curses.o")

# --- Link ---
# Use -specs= to provide musl CRT/libc/libgcc (do NOT use -nostdlib,
# which overrides specs startfile/endfile).  -pie emits R_ARM_RELATIVE
# relocations for exec.c to patch at load time.
echo "rogue: linking..."
$CC -mthumb -mcpu=cortex-m0plus \
    -pie \
    -specs="$SPECS_FILE" \
    -T "$LINKER_SCRIPT" \
    -Wl,--gc-sections \
    "${OBJS[@]}" \
    -o "$ROGUE_OUT/rogue.elf"

# --- Strip ---
arm-none-eabi-strip -s -o "$ROGUE_OUT/rogue" "$ROGUE_OUT/rogue.elf"

# --- Summary ---
echo ""
echo "rogue: build summary"
echo "========================================"
SIZE=$(stat -c%s "$ROGUE_OUT/rogue" 2>/dev/null || stat -f%z "$ROGUE_OUT/rogue")
echo "  stripped ELF: $SIZE bytes"
arm-none-eabi-size "$ROGUE_OUT/rogue.elf" 2>/dev/null | tail -1 | sed 's/^/  /'
echo "rogue: SUCCESS — installed to $ROGUE_OUT/rogue"

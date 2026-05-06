#!/bin/bash
# Build Rogue 5.4.4 for PiPAPo, linked against PPAP's own libc.
#
# Cross-compiles rogue + the curses shim with the PPAP libc headers and
# objects (no musl).  CRT and libc TUs come pre-built from the cmake
# user-program pipeline ($PPAP_SHARED_BUILD/{crt0,syscall,libc_*}.o).
#
# Always invoked from cmake (cmake/user.cmake → _ppap_add_rogue) with
# PPAP_CONFIG pointing at the generated target-config script.  All
# toolchain / target settings come from there — there is no standalone
# fallback path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROGUE_SRC="$SCRIPT_DIR/rogue"
PATCHES_DIR="$SCRIPT_DIR/patches/rogue"

# --- Parse flags ---
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# --- Source cmake-generated config (required) ---
if [[ -z "${PPAP_CONFIG:-}" || ! -f "$PPAP_CONFIG" ]]; then
    echo "ERROR: PPAP_CONFIG not set or missing." >&2
    echo "  build_rogue.sh must be invoked from cmake.  Run:" >&2
    echo "  ./scripts/build.sh <target>" >&2
    exit 1
fi
source "$PPAP_CONFIG"

ROGUE_OUT="$PPAP_SHARED_BUILD/rogue"

# Build CFLAGS — match the regular PPAP user-app build (freestanding,
# no libc), with PPAP libc headers + the rogue-specific shim/config
# overrides ahead of everything else.
CFLAGS="$PPAP_TARGET_FLAGS -Os -ffreestanding -nostdlib"
CFLAGS="$CFLAGS -nostdinc"
CFLAGS="$CFLAGS -isystem $PATCHES_DIR"            # config.h, curses.h, pwd.h, rogue.h
CFLAGS="$CFLAGS -isystem $PPAP_ROOT/src/user/include"
CFLAGS="$CFLAGS -isystem $PPAP_GCC_INCLUDE"
CFLAGS="$CFLAGS -I $PPAP_ROOT/src/user -I $PPAP_ROOT/src"
CFLAGS="$CFLAGS $PPAP_PIC_FLAGS"
CFLAGS="$CFLAGS -ffunction-sections -fdata-sections"
CFLAGS="$CFLAGS -DHAVE_CONFIG_H"
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

if [[ ! -f "$ROGUE_SRC/main.c" ]]; then
    echo "ERROR: rogue submodule not initialised." >&2
    echo "  Run: git submodule update --init third_party/rogue" >&2
    exit 1
fi

# Build CRT object list — these come from the regular cmake user-program
# pipeline.  Standalone callers must run cmake first to produce them.
CRT_OBJS=(
    "$PPAP_SHARED_BUILD/crt0.o"
    "$PPAP_SHARED_BUILD/syscall.o"
)
for unit in $PPAP_LIBC_UNITS; do
    CRT_OBJS+=("$PPAP_SHARED_BUILD/libc_${unit}.o")
done
# Optional arch overlays.
[[ -f "$PPAP_SHARED_BUILD/sigaction.o" ]] && CRT_OBJS+=("$PPAP_SHARED_BUILD/sigaction.o")
[[ -f "$PPAP_SHARED_BUILD/setjmp.o" ]]    && CRT_OBJS+=("$PPAP_SHARED_BUILD/setjmp.o")

for o in "${CRT_OBJS[@]}"; do
    if [[ ! -f "$o" ]]; then
        echo "ERROR: missing $o" >&2
        echo "  Run cmake configure + ./scripts/build.sh first." >&2
        exit 1
    fi
done

mkdir -p "$ROGUE_OUT/obj"

# --- Compile rogue sources ---
echo "rogue [$PPAP_ARCH]: compiling ($PPAP_ARCH_LABEL, PPAP libc)..."
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
echo "rogue [$PPAP_ARCH]: linking..."
LINK_FLAGS="$PPAP_TARGET_FLAGS -nostdlib"
if [[ -n "${PPAP_PIE_FLAG:-}" ]]; then
    LINK_FLAGS="$LINK_FLAGS $PPAP_PIE_FLAG"
fi
if [[ "$PPAP_ARCH" == "riscv" && -z "${PPAP_PIE_FLAG:-}" ]]; then
    LINK_FLAGS="$LINK_FLAGS -Wl,--emit-relocs -Wl,--no-relax"
fi

if [[ "${PPAP_RISCV_EPIC:-}" == "ON" ]]; then
    # ePIC clang + lld: same approach as the regular user-program path,
    # but linking PPAP libc objects + libgcc instead of musl.
    $PPAP_CC $LINK_FLAGS \
        -fuse-ld="$PPAP_LLD" -Wl,--strip-debug -Wl,--gc-sections \
        -T "$PPAP_USER_LD" \
        "${CRT_OBJS[@]}" \
        "${OBJS[@]}" \
        "$PPAP_LIBGCC" \
        -o "$ROGUE_OUT/rogue.elf"
else
    $PPAP_CC $LINK_FLAGS \
        -T "$PPAP_USER_LD" \
        -Wl,--gc-sections -Wl,--build-id=none \
        "${CRT_OBJS[@]}" \
        "${OBJS[@]}" \
        "$PPAP_LIBGCC" \
        -o "$ROGUE_OUT/rogue.elf"
fi

# --- Strip ---
$PPAP_STRIP ${PPAP_STRIP_FLAGS:---strip-unneeded} -o "$ROGUE_OUT/rogue" "$ROGUE_OUT/rogue.elf"

# --- Summary ---
echo ""
echo "rogue [$PPAP_ARCH]: build summary"
echo "========================================"
SIZE=$(stat -c%s "$ROGUE_OUT/rogue" 2>/dev/null || stat -f%z "$ROGUE_OUT/rogue")
echo "  stripped ELF: $SIZE bytes"
$PPAP_SIZE_CMD "$ROGUE_OUT/rogue.elf" 2>/dev/null | tail -1 | sed 's/^/  /'
echo "rogue [$PPAP_ARCH]: SUCCESS — installed to $ROGUE_OUT/rogue"

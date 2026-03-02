#!/bin/bash
# Build busybox for PicoPiAndPortable (ARMv6-M Thumb / Cortex-M0+)
#
# This script:
#   1. Resets the busybox submodule to clean state
#   2. Generates a musl-arm GCC specs file (replaces newlib CRT/libs with musl)
#   3. Generates .config from allnoconfig + PPAP fragment
#   4. Builds busybox as a static binary linked with musl libc
#   5. Copies the result to build/busybox/
#   6. Restores the submodule to clean state
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
FRAGMENT="$SCRIPT_DIR/configs/busybox_ppap.fragment"
SPECS_FILE="$PROJECT_ROOT/build/musl-arm.specs"

GCC_INCLUDE="$(arm-none-eabi-gcc -print-file-name=include)"
GCC_LIBDIR="$(dirname "$(arm-none-eabi-gcc -mthumb -mcpu=cortex-m0plus -print-libgcc-file-name)")"

# CFLAGS: Thumb-1 target flags + musl headers (before GCC builtins to win stdint.h)
CFLAGS_PPAP="-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft -Os -nostdinc -isystem $MUSL_SYSROOT/include -isystem $GCC_INCLUDE"

# --- Handle --clean ---
if [[ "${1:-}" == "--clean" ]]; then
    echo "busybox: cleaning build artifacts..."
    cd "$BB_SRC" && git checkout . && git clean -fdx 2>/dev/null || true
    rm -rf "$BB_OUT" "$SPECS_FILE"
    echo "busybox: clean done."
    exit 0
fi

# --- Skip if already built ---
if [[ -f "$BB_OUT/busybox" ]]; then
    echo "busybox: binary already exists at $BB_OUT/busybox — skipping."
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

# --- Restore submodule to clean state ---
echo "busybox: resetting submodule to clean state..."
cd "$BB_SRC"
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Generate .config from allnoconfig + PPAP fragment ---
echo "busybox: generating .config..."
make allnoconfig ARCH=arm 2>&1 | tail -1

# Apply our fragment: for each CONFIG_FOO=y line, enable it in .config
while IFS= read -r line; do
    # Skip comments and empty lines
    [[ "$line" =~ ^# ]] && continue
    [[ -z "$line" ]] && continue

    key="${line%%=*}"
    val="${line#*=}"

    # Replace "# CONFIG_FOO is not set" with "CONFIG_FOO=y"
    # or replace existing CONFIG_FOO=n with CONFIG_FOO=y
    if grep -q "# ${key} is not set" .config 2>/dev/null; then
        sed -i "s/# ${key} is not set/${line}/" .config
    elif grep -q "^${key}=" .config 2>/dev/null; then
        sed -i "s/^${key}=.*/${line}/" .config
    else
        echo "$line" >> .config
    fi
done < "$FRAGMENT"

# Inject CFLAGS into .config (paths are build-time specific).
# -specs= goes in CFLAGS (used by gcc, not by ld -r for partial links).
# It overrides GCC's default CRT/lib to use musl instead of newlib.
# CONFIG_EXTRA_LDFLAGS must stay empty because it feeds LDFLAGS which is
# shared between "ld -r" partial links and the final gcc link.
sed -i 's|^CONFIG_SYSROOT=.*|CONFIG_SYSROOT=""|' .config
sed -i 's|^CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="'"$CFLAGS_PPAP -specs=$SPECS_FILE"'"|' .config
sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS=""|' .config
sed -i 's|^CONFIG_EXTRA_LDLIBS=.*|CONFIG_EXTRA_LDLIBS=""|' .config

echo "busybox: musl sysroot=$MUSL_SYSROOT"
grep -E 'CONFIG_EXTRA_CFLAGS|CONFIG_EXTRA_LDFLAGS|CONFIG_EXTRA_LDLIBS' .config

# Resolve dependencies
echo "" | make oldconfig ARCH=arm 2>&1 | tail -3

echo "busybox: enabled applets:"
grep '=y' .config | grep -v '^#' | grep -v '_FEATURE_\|_STATIC\|_NOMMU\|_LFS\|_CROSS\|_PREFIX\|_EXTRA\|_SH_\|_PREFER\|_OPTIMIZE\|_INTERNAL\|_BUILTIN\|_ALIAS\|_CMDCMD' | sed 's/CONFIG_/  /' | sort

# --- Build ---
echo "busybox: building (static, musl, Cortex-M0+)..."
make ARCH=arm \
    CROSS_COMPILE=arm-none-eabi- \
    SKIP_STRIP=y \
    -j"$(nproc)" 2>&1

# --- Install ---
echo "busybox: installing to $BB_OUT..."
mkdir -p "$BB_OUT"
cp busybox "$BB_OUT/busybox"
# Generate applet list
./busybox_unstripped --list 2>/dev/null > "$BB_OUT/applets.txt" || \
    grep 'IF_.*APPLET' include/applets.h 2>/dev/null | \
    sed -n 's/.*APPLET[^(]*(\([^,]*\).*/\1/p' | sort > "$BB_OUT/applets.txt" || true

# --- Restore submodule ---
echo "busybox: restoring submodule to clean state..."
git checkout . 2>/dev/null || true
git clean -fdx 2>/dev/null || true

# --- Verify ---
if [[ -f "$BB_OUT/busybox" ]]; then
    SIZE=$(stat -c%s "$BB_OUT/busybox" 2>/dev/null || stat -f%z "$BB_OUT/busybox")
    echo "busybox: SUCCESS — busybox built ($SIZE bytes)"
    echo "  binary: $BB_OUT/busybox"
    arm-none-eabi-readelf -h "$BB_OUT/busybox" 2>/dev/null | grep -E "Machine|Class|Type" | sed 's/^/  /'
    arm-none-eabi-size "$BB_OUT/busybox" 2>/dev/null | sed 's/^/  /'
else
    echo "ERROR: busybox binary not found after build" >&2
    exit 1
fi

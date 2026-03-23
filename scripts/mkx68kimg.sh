#!/usr/bin/env bash
# mkx68kimg.sh — Create a bootable X68000 2HD floppy image for PPAP (Phase X-3)
#
# Usage:
#   ./scripts/mkx68kimg.sh [KERNEL_BIN [OUTPUT_XDF]]
#
# Defaults:
#   KERNEL_BIN = build/x68k/ppap_x68k.bin
#   OUTPUT_XDF = build/x68k/ppap_x68k.xdf
#
# Floppy layout (2HD: 77 tracks × 2 heads × 8 sectors × 1024 B = 1,261,568 B):
#   Sector 0        (1024 B): stage1   — IPL boot sector
#   Sectors 1–3    (3072 B): stage2   — UFS kernel + rootfs loader
#   Sectors 4+    (rest)   : outer UFS — boot/kernel + boot/rootfs.ufs
#
# The outer UFS contains:
#   boot/kernel      — the kernel binary (ppap_x68k.bin)
#   boot/rootfs.ufs  — inner UFS rootfs (userland from romfs staging dir)
#
# Prerequisites:
#   - build/x68k/ppap_x68k.bin       (kernel, built by cmake)
#   - build/x68k/romfs_ppap_x68k/    (userland staging, built by cmake)
#   - m68k-elf-gcc (on PATH, or set M68K_GCC/M68K_OBJCOPY/M68K_NM)
#   - build/qemu_arm/mkufs or build/m68k/mkufs (any built ARM/m68k target)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

KERNEL_BIN="${1:-$PROJECT_DIR/build/x68k/ppap_x68k.bin}"
OUTPUT_XDF="${2:-$PROJECT_DIR/build/x68k/ppap_x68k.xdf}"

M68K_GCC="${M68K_GCC:-m68k-elf-gcc}"
M68K_OBJCOPY="${M68K_OBJCOPY:-m68k-elf-objcopy}"
M68K_NM="${M68K_NM:-m68k-elf-nm}"
# mkufs may be built by any ARM target (qemu_arm) or m68k build (host tools)
MKUFS="${MKUFS:-}"
for _d in "$PROJECT_DIR/build/m68k" "$PROJECT_DIR/build/qemu_arm" "$PROJECT_DIR/build/host"; do
    if [[ -x "$_d/mkufs" ]]; then MKUFS="$_d/mkufs"; break; fi
done

STAGE1_S="$PROJECT_DIR/src/target/x68k/boot/stage1.S"
STAGE1_LD="$PROJECT_DIR/src/target/x68k/boot/bootldr.ld"   # VMA=0x002000, LMA=0
STAGE2_HEAD_S="$PROJECT_DIR/src/target/x68k/boot/stage2_head.S"
STAGE2_C="$PROJECT_DIR/src/target/x68k/boot/stage2.c"
STAGE2_LD="$PROJECT_DIR/src/target/x68k/boot/stage2.ld"

ROMFS_STAGING="$PROJECT_DIR/build/x68k/romfs_ppap_x68k"

# ── Verify prerequisites ──────────────────────────────────────────────────────

if [[ ! -f "$KERNEL_BIN" ]]; then
    echo "[mkx68kimg] Error: kernel binary not found: $KERNEL_BIN"
    echo "            Build first: ./scripts/run.sh --build x68k"
    exit 1
fi

if ! command -v "$M68K_GCC" &>/dev/null; then
    echo "[mkx68kimg] Error: m68k-elf-gcc not found: $M68K_GCC"
    exit 1
fi

if [[ -z "$MKUFS" || ! -x "$MKUFS" ]]; then
    echo "[mkx68kimg] Error: mkufs not found: $MKUFS"
    echo "            Build m68k tools first: ./scripts/run.sh --build qemu_m68k"
    exit 1
fi

if [[ ! -d "$ROMFS_STAGING" ]]; then
    echo "[mkx68kimg] Error: romfs staging dir not found: $ROMFS_STAGING"
    echo "            Build x68k kernel first (cmake generates it during ppap_generate_romfs)"
    exit 1
fi

# ── X68000 2HD floppy geometry ────────────────────────────────────────────────

FLOPPY_TRACKS=77
FLOPPY_HEADS=2
FLOPPY_SECTORS_PER_TRACK=8
FLOPPY_SECTOR_SIZE=1024
FLOPPY_TOTAL_SECTORS=$(( FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS_PER_TRACK ))
FLOPPY_SIZE=$(( FLOPPY_TOTAL_SECTORS * FLOPPY_SECTOR_SIZE ))

STAGE1_MAX=1024
STAGE2_OFFSET=1024     # byte offset of sector 1
STAGE2_MAX=3072        # 3 sectors × 1024 B
UFS_OFFSET=4096        # byte offset of sector 4

# ── Working directory ─────────────────────────────────────────────────────────

TMPROOT=$(mktemp -d)
trap "rm -rf '$TMPROOT'" EXIT
TMPDIR="$TMPROOT/work"
mkdir -p "$TMPDIR"

# ── Build stage1.bin ──────────────────────────────────────────────────────────

echo "[mkx68kimg] Building stage1..."
$M68K_GCC -m68000 -nostdlib -ffreestanding \
    -T "$STAGE1_LD" \
    -Wl,--build-id=none \
    -o "$TMPDIR/stage1.elf" \
    "$STAGE1_S"
$M68K_OBJCOPY -O binary -j .text "$TMPDIR/stage1.elf" "$TMPDIR/stage1.bin"

STAGE1_SIZE=$(stat -c%s "$TMPDIR/stage1.bin")
echo "[mkx68kimg] Stage1:   $STAGE1_SIZE / $STAGE1_MAX bytes"
if [[ $STAGE1_SIZE -gt $STAGE1_MAX ]]; then
    echo "[mkx68kimg] Error: stage1 too large"
    exit 1
fi

# ── Build stage2.bin ──────────────────────────────────────────────────────────

# Read __page_pool_start from the kernel ELF — stage2 loads rootfs there so
# it lands above the kernel BSS region (which Reset_Handler zeros on boot).
KERNEL_ELF="$PROJECT_DIR/build/x68k/ppap_x68k.elf"
if [[ ! -f "$KERNEL_ELF" ]]; then
    echo "[mkx68kimg] Error: kernel ELF not found: $KERNEL_ELF"
    exit 1
fi
POOL_BASE_HEX=$($M68K_NM "$KERNEL_ELF" \
    | awk '/[ \t]__page_pool_start$/{print "0x"$1"u"; exit}')
if [[ -z "$POOL_BASE_HEX" ]]; then
    echo "[mkx68kimg] Error: __page_pool_start symbol not found in $KERNEL_ELF"
    exit 1
fi
echo "[mkx68kimg] Rootfs load base: $POOL_BASE_HEX (= __page_pool_start)"

echo "[mkx68kimg] Building stage2..."
$M68K_GCC -m68000 -nostdlib -ffreestanding -Os \
    -DROOTFS_BASE="${POOL_BASE_HEX}" \
    -T "$STAGE2_LD" \
    -Wl,--build-id=none \
    -o "$TMPDIR/stage2.elf" \
    "$STAGE2_HEAD_S" "$STAGE2_C"
$M68K_OBJCOPY -O binary -j .text -j .rodata "$TMPDIR/stage2.elf" "$TMPDIR/stage2.bin"

STAGE2_SIZE=$(stat -c%s "$TMPDIR/stage2.bin")
echo "[mkx68kimg] Stage2:   $STAGE2_SIZE / $STAGE2_MAX bytes"
if [[ $STAGE2_SIZE -gt $STAGE2_MAX ]]; then
    echo "[mkx68kimg] Error: stage2 too large ($STAGE2_SIZE > $STAGE2_MAX bytes)"
    echo "            Consider increasing stage2 to 6 sectors (6144 bytes)"
    exit 1
fi

# ── Build inner rootfs UFS ────────────────────────────────────────────────────

echo "[mkx68kimg] Building inner rootfs.ufs from $ROMFS_STAGING..."
# Compute inner UFS size: staging content + 15% overhead + 32 KB metadata,
# rounded up to 4 KB (UFS block size).  Keep it as small as possible so the
# whole image fits on a 2HD floppy (1261568 bytes).
STAGING_KB=$(du -sk "$ROMFS_STAGING" | awk '{print $1}')
INNER_SIZE_KB=$(( (STAGING_KB * 115 / 100 + 32 + 3) / 4 * 4 ))
# Count inodes needed: one per filesystem entry (file, dir, symlink) + 25% margin
STAGING_INODES=$(find "$ROMFS_STAGING" | wc -l)
INNER_INODES=$(( STAGING_INODES * 5 / 4 + 16 ))
"$MKUFS" -s "${INNER_SIZE_KB}K" -i "$INNER_INODES" -B -p "$ROMFS_STAGING" "$TMPDIR/rootfs.ufs"
INNER_SIZE=$(stat -c%s "$TMPDIR/rootfs.ufs")
echo "[mkx68kimg] Rootfs:   $INNER_SIZE bytes (${INNER_SIZE_KB} KB allocated)"

# ── Build outer UFS (boot/kernel + boot/rootfs.ufs) ───────────────────────────

echo "[mkx68kimg] Building outer UFS..."
mkdir -p "$TMPDIR/outer/boot"
cp "$KERNEL_BIN"          "$TMPDIR/outer/boot/kernel"
cp "$TMPDIR/rootfs.ufs"   "$TMPDIR/outer/boot/rootfs.ufs"

KERNEL_SIZE=$(stat -c%s "$KERNEL_BIN")
OUTER_DATA=$(( KERNEL_SIZE + INNER_SIZE ))
# Outer UFS metadata: super + bmap + imap + 1 inode block = 4 blocks = 16 KB,
# plus 2 indirect blocks + 2 dir blocks + 8 KB margin = 32 KB overhead.
# Keep total image ≤ FLOPPY_SIZE - UFS_OFFSET = 1257472 B.
OUTER_SIZE_KB=$(( (OUTER_DATA / 1024) + 32 + 4 ))
OUTER_SIZE_KB=$(( ((OUTER_SIZE_KB + 3) / 4) * 4 ))  # align to 4 KB
# Outer UFS only contains 4 inodes (root, boot, kernel, rootfs.ufs)
"$MKUFS" -s "${OUTER_SIZE_KB}K" -i 16 -B -p "$TMPDIR/outer" "$TMPDIR/outer.ufs"
OUTER_SIZE=$(stat -c%s "$TMPDIR/outer.ufs")
echo "[mkx68kimg] Outer UFS: $OUTER_SIZE bytes (${OUTER_SIZE_KB} KB allocated)"

# ── Verify floppy capacity ────────────────────────────────────────────────────

TOTAL_BYTES=$(( UFS_OFFSET + OUTER_SIZE ))
if [[ $TOTAL_BYTES -gt $FLOPPY_SIZE ]]; then
    echo "[mkx68kimg] Error: image ($TOTAL_BYTES B) exceeds 2HD floppy ($FLOPPY_SIZE B)"
    echo "            Reduce kernel or rootfs size"
    exit 1
fi
USED_SECTORS=$(( (TOTAL_BYTES + FLOPPY_SECTOR_SIZE - 1) / FLOPPY_SECTOR_SIZE ))
echo "[mkx68kimg] Floppy:   $USED_SECTORS / $FLOPPY_TOTAL_SECTORS sectors used"

# ── Assemble floppy image ─────────────────────────────────────────────────────

mkdir -p "$(dirname "$OUTPUT_XDF")"

# Zero-fill the entire floppy
dd if=/dev/zero of="$OUTPUT_XDF" bs="$FLOPPY_SECTOR_SIZE" count="$FLOPPY_TOTAL_SECTORS" \
    status=none

# Write stage1 at sector 0 (byte offset 0), zero-padded to 1024 B
dd if="$TMPDIR/stage1.bin" of="$OUTPUT_XDF" bs=1 conv=notrunc status=none

# Write stage2 at sector 1 (byte offset 1024), zero-padded to 3072 B
dd if="$TMPDIR/stage2.bin" of="$OUTPUT_XDF" bs=1 seek="$STAGE2_OFFSET" \
    conv=notrunc status=none

# Write outer UFS at sector 4 (byte offset 4096)
dd if="$TMPDIR/outer.ufs" of="$OUTPUT_XDF" bs=1 seek="$UFS_OFFSET" \
    conv=notrunc status=none

echo "[mkx68kimg] Created: $OUTPUT_XDF"
echo ""
echo "[mkx68kimg] To run with an emulator:"
echo "   XEiJ:      set FD0 to $OUTPUT_XDF"
echo "   XM6 TypeG: set FD0 to $OUTPUT_XDF"

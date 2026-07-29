#!/usr/bin/env bash
# mkx68kimg.sh — Create a bootable X68000 2HD floppy image for PPAP
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
#   Sectors 1–3    (3072 B): stage2   — 44bsd UFS kernel loader
#   Sectors 4+    (rest)   : 44bsd UFS — /boot/kernel + userland
#
# Stage2 reads /boot/kernel from this UFS and loads it to 0x006000;
# the kernel later mounts the same UFS as the rootfs via iocs_blk ("fd0").
# No second UFS layer, no in-RAM rootfs image.
#
# Prerequisites:
#   - build/x68k/ppap_x68k.bin       (kernel, built by cmake)
#   - build/x68k/romfs_ppap_x68k/    (userland staging, built by cmake)
#   - m68k-elf-gcc (on PATH, or set M68K_GCC/M68K_OBJCOPY)
#   - mkufs from build/qemu_arm/ or build/m68k/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

KERNEL_BIN="${1:-$PROJECT_DIR/build/x68k/ppap_x68k.bin}"
OUTPUT_XDF="${2:-$PROJECT_DIR/build/x68k/ppap_x68k.xdf}"
OUTPUT_HDS="${OUTPUT_XDF%.xdf}.hds"

M68K_GCC="${M68K_GCC:-m68k-elf-gcc}"
M68K_OBJCOPY="${M68K_OBJCOPY:-m68k-elf-objcopy}"
MKUFS="${MKUFS:-}"
for _d in "$PROJECT_DIR/build/m68k" "$PROJECT_DIR/build/qemu_arm" "$PROJECT_DIR/build/host"; do
    if [[ -x "$_d/mkufs" ]]; then MKUFS="$_d/mkufs"; break; fi
done

STAGE1_S="$PROJECT_DIR/src/target/x68k/boot/stage1.S"
STAGE1_LD="$PROJECT_DIR/src/target/x68k/boot/bootldr.ld"
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

echo "[mkx68kimg] Building stage2..."
$M68K_GCC -m68000 -nostdlib -ffreestanding -Os \
    -T "$STAGE2_LD" \
    -Wl,--build-id=none \
    -o "$TMPDIR/stage2.elf" \
    "$STAGE2_HEAD_S" "$STAGE2_C"
$M68K_OBJCOPY -O binary -j .text -j .rodata "$TMPDIR/stage2.elf" "$TMPDIR/stage2.bin"

STAGE2_SIZE=$(stat -c%s "$TMPDIR/stage2.bin")
echo "[mkx68kimg] Stage2:   $STAGE2_SIZE / $STAGE2_MAX bytes"
if [[ $STAGE2_SIZE -gt $STAGE2_MAX ]]; then
    echo "[mkx68kimg] Error: stage2 too large ($STAGE2_SIZE > $STAGE2_MAX bytes)"
    exit 1
fi

# ── Stage the rootfs UFS (kernel under /boot, userland at /) ──────────────────

echo "[mkx68kimg] Staging UFS contents..."
UFS_STAGING="$TMPDIR/ufs_root"
mkdir -p "$UFS_STAGING/boot"
cp -a "$ROMFS_STAGING/." "$UFS_STAGING/"
cp "$KERNEL_BIN" "$UFS_STAGING/boot/kernel"

# ── Build the 44bsd UFS that becomes both the boot source and the rootfs ──────

echo "[mkx68kimg] Building rootfs UFS (44bsd)..."
STAGING_KB=$(du -sk "$UFS_STAGING" | awk '{print $1}')
# Data + 15% overhead + 32 KB metadata, rounded up to 4 KB (UFS block size).
UFS_SIZE_KB=$(( (STAGING_KB * 115 / 100 + 32 + 3) / 4 * 4 ))
STAGING_INODES=$(find "$UFS_STAGING" | wc -l)
UFS_INODES=$(( STAGING_INODES * 5 / 4 + 16 ))
"$MKUFS" -f 44bsd -s "${UFS_SIZE_KB}K" -i "$UFS_INODES" -B \
    -p "$UFS_STAGING" "$TMPDIR/rootfs.ufs"
UFS_SIZE=$(stat -c%s "$TMPDIR/rootfs.ufs")
echo "[mkx68kimg] UFS:      $UFS_SIZE bytes (${UFS_SIZE_KB} KB allocated)"

# ── Build the SCSI HDD image (.hds): X68SCSI1 header (LBA 0) + UFS (LBA 1+) ────
#
# XEiJ .hds device-init header (512 bytes at offset 0, big-endian):
#   0x00  "X68SCSI1"   magic
#   0x08  word         bytes/record (512)
#   0x0A  long         disk record count (valid LBAs 0..count-1)
#   0x0E  byte         Read(10)/Write(10)-capable flag (0x01)
# The rootfs UFS follows at LBA 1 (byte 512).  Guest LBA 0 IS the header
# (XEiJ maps LBA→offset directly), so scsi_blk maps blkdev sector S to
# LBA (1 + S) — matching SCSI_UFS_BASE in scsi_blk.c.

HDS_RECORD=512
HDS_UFS_RECORDS=$(( UFS_SIZE / HDS_RECORD ))
HDS_TOTAL_RECORDS=$(( 1 + HDS_UFS_RECORDS ))   # +1 for the header at LBA 0
python3 - "$TMPDIR/hds_header.bin" "$HDS_TOTAL_RECORDS" <<'PY'
import struct, sys
out, total = sys.argv[1], int(sys.argv[2])
hdr = bytearray(512)
hdr[0:8] = b"X68SCSI1"
struct.pack_into(">H", hdr, 0x08, 512)     # bytes/record
struct.pack_into(">I", hdr, 0x0A, total)   # disk record count
hdr[0x0E] = 0x01                            # Read(10)/Write(10) capable
open(out, "wb").write(hdr)
PY
mkdir -p "$(dirname "$OUTPUT_HDS")"
cat "$TMPDIR/hds_header.bin" "$TMPDIR/rootfs.ufs" > "$OUTPUT_HDS"
HDS_SIZE=$(stat -c%s "$OUTPUT_HDS")
echo "[mkx68kimg] HDD:      $OUTPUT_HDS ($HDS_SIZE bytes, $HDS_TOTAL_RECORDS records)"

# ── Floppy capacity (skip .xdf gracefully if the UFS overflows) ───────────────

FLOPPY_OK=1
TOTAL_BYTES=$(( UFS_OFFSET + UFS_SIZE ))
if [[ $TOTAL_BYTES -gt $FLOPPY_SIZE ]]; then
    echo "[mkx68kimg] WARNING: image ($TOTAL_BYTES B) exceeds 2HD floppy ($FLOPPY_SIZE B); skipping .xdf (HDD image only)"
    FLOPPY_OK=0
    rm -f "$OUTPUT_XDF"
else
    USED_SECTORS=$(( (TOTAL_BYTES + FLOPPY_SECTOR_SIZE - 1) / FLOPPY_SECTOR_SIZE ))
    echo "[mkx68kimg] Floppy:   $USED_SECTORS / $FLOPPY_TOTAL_SECTORS sectors used"
fi

# ── Assemble floppy image (if it fits) ────────────────────────────────────────

if [[ $FLOPPY_OK -eq 1 ]]; then
    mkdir -p "$(dirname "$OUTPUT_XDF")"
    dd if=/dev/zero of="$OUTPUT_XDF" bs="$FLOPPY_SECTOR_SIZE" \
        count="$FLOPPY_TOTAL_SECTORS" status=none

    dd if="$TMPDIR/stage1.bin" of="$OUTPUT_XDF" bs=1 conv=notrunc status=none
    dd if="$TMPDIR/stage2.bin" of="$OUTPUT_XDF" bs=1 seek="$STAGE2_OFFSET" \
        conv=notrunc status=none
    dd if="$TMPDIR/rootfs.ufs" of="$OUTPUT_XDF" bs=1 seek="$UFS_OFFSET" \
        conv=notrunc status=none

    echo "[mkx68kimg] Created: $OUTPUT_XDF"
fi

echo ""
echo "[mkx68kimg] To run with an emulator (XEiJ):"
echo "   FD0 (floppy boot):  $OUTPUT_XDF"
echo "   SCSI id0:           $OUTPUT_HDS  (-sc0=, -boot=sc0 once HDD-bootable)"

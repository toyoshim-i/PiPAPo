#!/usr/bin/env bash
# =============================================================================
# mkpcimg.sh — Assemble PC/XT bootable floppy and HDD images
# =============================================================================
#
# Floppy layout (720 KB, 1440 × 512-byte sectors):
#   Sector 0          stage1 boot sector (512 B, BPB header)
#   Sectors 1-8       stage2 UFS loader  (4 KB)
#   Sectors 9+        UFS partition (contains /boot/kernel)
#
# HDD layout (64 MB, 131072 × 512-byte sectors):
#   Sector 0          MBR boot sector (512 B, partition table)
#   Sectors 1-8       stage2 UFS loader  (4 KB, same binary)
#   Sectors 9+        UFS partition (type 0xA9, same content)
#
# Usage:
#   ./scripts/mkpcimg.sh [build_dir]
#
# Expects build artifacts in the selected build directory:
#   stage1.bin      — floppy boot sector flat binary (512 B)
#   stage1_hdd.bin  — MBR boot sector flat binary (512 B)
#   stage2.bin      — stage2 flat binary (≤4 KB)
#   ppap_pcxt_text.bin — kernel .text+.rodata flat binary
#   ppap_pcxt_data.bin — kernel .data flat binary
#
# Output:
#   build/pcxt/ppap_pcxt.img       (720 KB floppy image)
#   build/pcxt/ppap_pcxt_hdd.img   (64 MB HDD image)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$PROJECT_DIR/build/pcxt}"
MKUFS="$PROJECT_DIR/tools/mkufs/mkufs"
# Stage2 now parses 44BSD UFS layout (SB at sector 16, variable dirents).
# Keep PPAP_MKUFS_FORMAT override for explicit testing.
MKUFS_FORMAT="${PPAP_MKUFS_FORMAT:-44bsd}"

STAGE1="$BUILD_DIR/stage1.bin"
STAGE1_HDD="$BUILD_DIR/stage1_hdd.bin"
STAGE2="$BUILD_DIR/stage2.bin"
KERNEL_TEXT="$BUILD_DIR/ppap_pcxt_text.bin"
KERNEL_DATA="$BUILD_DIR/ppap_pcxt_data.bin"
IMG="$BUILD_DIR/ppap_pcxt.img"
IMG_HDD="$BUILD_DIR/ppap_pcxt_hdd.img"

# Common parameters
SECTOR_SIZE=512
STAGE2_SECTORS=8     # 4 KB for stage2
UFS_START_SECTOR=9   # sector 0 = stage1, 1-8 = stage2

# Floppy parameters
FLOPPY_SECTORS=1440  # 720 KB (3.5" DD, 80 cyl × 2 heads × 9 spt)
FLOPPY_INODES="${PPAP_PCXT_FLOPPY_INODES:-256}"

# HDD parameters
HDD_SIZE_MB=64
HDD_SECTORS=$(( HDD_SIZE_MB * 1024 * 1024 / SECTOR_SIZE ))  # 131072
HDD_INODES="${PPAP_PCXT_HDD_INODES:-1024}"

# ── Verify inputs ──────────────────────────────────────────────────────────

for f in "$STAGE1" "$STAGE1_HDD" "$STAGE2" "$KERNEL_TEXT" "$KERNEL_DATA"; do
  if [[ ! -f "$f" ]]; then
    echo "[mkpcimg] Error: $f not found" >&2
    exit 1
  fi
done

# ── Build mkufs if needed ─────────────────────────────────────────────────

if [[ ! -x "$MKUFS" ]]; then
  echo "[mkpcimg] Building mkufs..."
  cc -O2 -o "$MKUFS" "$PROJECT_DIR/tools/mkufs/mkufs.c"
fi

# ── Stage UFS content ─────────────────────────────────────────────────────

USER_BUILD_DIR="$BUILD_DIR/user"
UFS_STAGING="$BUILD_DIR/ufs_staging"
rm -rf "$UFS_STAGING"
mkdir -p "$UFS_STAGING/boot" "$UFS_STAGING/bin" "$UFS_STAGING/sbin" \
         "$UFS_STAGING/etc" "$UFS_STAGING/dev" "$UFS_STAGING/proc" \
         "$UFS_STAGING/tmp"
cp "$KERNEL_TEXT" "$UFS_STAGING/boot/kernel_text"
cp "$KERNEL_DATA" "$UFS_STAGING/boot/kernel_data"

# Include VFS module if built (code + data as separate files)
VFS_BIN="$BUILD_DIR/ppap_pcxt_vfs.bin"
VFS_DATA="$BUILD_DIR/ppap_pcxt_vfs_data.bin"
if [[ -f "$VFS_BIN" ]]; then
  cp "$VFS_BIN" "$UFS_STAGING/boot/kernel_vfs"
fi
if [[ -f "$VFS_DATA" ]]; then
  cp "$VFS_DATA" "$UFS_STAGING/boot/kernel_vfs_data"
fi

# Install base /etc files
cp "$PROJECT_DIR/src/etc/"* "$UFS_STAGING/etc/" 2>/dev/null || true

# Include first-party user programs from src/user if built.
# Keep init under /sbin and expose push as /bin/sh.
USER_APPS=(hello getty init pdb push cat ls ps df top pi uname sleep mkdir reset rmdir rm kill touch date)
for app in "${USER_APPS[@]}"; do
  elf="$USER_BUILD_DIR/$app.elf"
  if [[ ! -f "$elf" ]]; then
    continue
  fi
  if [[ "$app" == "init" ]]; then
    cp "$elf" "$UFS_STAGING/sbin/init"
  else
    cp "$elf" "$UFS_STAGING/bin/$app"
  fi
done
if [[ -f "$USER_BUILD_DIR/push.elf" ]]; then
  cp "$USER_BUILD_DIR/push.elf" "$UFS_STAGING/bin/sh"
fi
# /bin/vi → pi symlink
if [[ -f "$UFS_STAGING/bin/pi" ]]; then
  ln -sf pi "$UFS_STAGING/bin/vi"
fi
if [[ -f "$USER_BUILD_DIR/runtests.elf" ]]; then
  cp "$USER_BUILD_DIR/runtests.elf" "$UFS_STAGING/bin/runtests"
fi
if [[ -f "$USER_BUILD_DIR/runtests_ext.elf" ]]; then
  cp "$USER_BUILD_DIR/runtests_ext.elf" "$UFS_STAGING/bin/runtests_ext"
fi
TEST_BINS=(test_exec test_vfork test_pipe test_brk test_fd test_signal test_poll
           test_sleep_intr test_orphan test_id test_time test_iov test_stat
           test_env test_fs test_rw test_tmpfs test_ufs test_msdos)
for tst in "${TEST_BINS[@]}"; do
  if [[ -f "$USER_BUILD_DIR/$tst.elf" ]]; then
    cp "$USER_BUILD_DIR/$tst.elf" "$UFS_STAGING/bin/$tst"
  fi
done

# Apply --overlay (via PPAP_EXTRA_OVERLAY env var) on top of the base
# staging.  Files in the overlay shadow any identically-named base files.
if [[ -n "${PPAP_EXTRA_OVERLAY:-}" && -d "$PPAP_EXTRA_OVERLAY" ]]; then
  echo "[mkpcimg] Applying overlay: $PPAP_EXTRA_OVERLAY"
  cp -r "$PPAP_EXTRA_OVERLAY/." "$UFS_STAGING/"
fi


# ── Pad stage2 ──────────────────────────────────────────────────────────

STAGE2_PAD="$BUILD_DIR/stage2_padded.bin"
dd if=/dev/zero of="$STAGE2_PAD" bs=$SECTOR_SIZE count=$STAGE2_SECTORS 2>/dev/null
dd if="$STAGE2" of="$STAGE2_PAD" bs=1 conv=notrunc 2>/dev/null

# ── Create floppy UFS image ─────────────────────────────────────────────

# Available space for UFS: sectors 9..2879 = 2871 sectors × 512 = 1,470,072 B
UFS_FD_SIZE=$(( (FLOPPY_SECTORS - UFS_START_SECTOR) * SECTOR_SIZE ))
UFS_FD_IMG="$BUILD_DIR/ufs_boot.img"
FLOPPY_OK=1

# Check staging content size against the floppy UFS budget.  If the
# payload exceeds the floppy capacity (e.g. with a large --overlay),
# skip floppy generation gracefully — HDD still gets built below.
STAGING_BYTES=$(du -sb "$UFS_STAGING" 2>/dev/null | awk '{print $1}')
STAGING_BYTES=${STAGING_BYTES:-0}
if (( STAGING_BYTES > UFS_FD_SIZE - 32768 )); then
  echo "[mkpcimg] WARNING: staging content ($(( STAGING_BYTES / 1024 )) KB) exceeds floppy UFS budget ($(( UFS_FD_SIZE / 1024 )) KB); skipping floppy image"
  FLOPPY_OK=0
fi

if (( FLOPPY_OK )); then
  echo "[mkpcimg] Creating floppy UFS image ($(( UFS_FD_SIZE / 1024 )) KB)..."
  if ! "$MKUFS" -f "$MKUFS_FORMAT" -s "$UFS_FD_SIZE" -i "$FLOPPY_INODES" -p "$UFS_STAGING" "$UFS_FD_IMG"; then
    echo "[mkpcimg] WARNING: mkufs failed for floppy (payload too large?); skipping floppy image"
    FLOPPY_OK=0
  fi
fi

# ── Assemble floppy image ───────────────────────────────────────────────

if (( FLOPPY_OK )); then
  echo "[mkpcimg] Assembling floppy image..."

  dd if=/dev/zero of="$IMG" bs=$SECTOR_SIZE count=$FLOPPY_SECTORS 2>/dev/null
  dd if="$STAGE1" of="$IMG" bs=$SECTOR_SIZE conv=notrunc 2>/dev/null
  dd if="$STAGE2_PAD" of="$IMG" bs=$SECTOR_SIZE seek=1 conv=notrunc 2>/dev/null
  dd if="$UFS_FD_IMG" of="$IMG" bs=$SECTOR_SIZE seek=$UFS_START_SECTOR conv=notrunc 2>/dev/null

  echo "[mkpcimg] Floppy: $IMG ($(wc -c < "$IMG") bytes)"
  echo "[mkpcimg]   stage1: $(wc -c < "$STAGE1") bytes"
  echo "[mkpcimg]   stage2: $(wc -c < "$STAGE2") bytes"
  echo "[mkpcimg]   kernel: $(wc -c < "$KERNEL") bytes"
  echo "[mkpcimg]   UFS:    $(wc -c < "$UFS_FD_IMG") bytes"
else
  # Remove any stale floppy image so a previous build doesn't get booted.
  rm -f "$IMG" "$UFS_FD_IMG"
fi

# ── Create HDD UFS image ────────────────────────────────────────────────

UFS_HDD_SECTORS=$(( HDD_SECTORS - UFS_START_SECTOR ))
UFS_HDD_SIZE=$(( UFS_HDD_SECTORS * SECTOR_SIZE ))
UFS_HDD_IMG="$BUILD_DIR/ufs_hdd.img"

echo "[mkpcimg] Creating HDD UFS image ($(( UFS_HDD_SIZE / 1024 )) KB)..."
"$MKUFS" -f "$MKUFS_FORMAT" -s "$UFS_HDD_SIZE" -i "$HDD_INODES" -p "$UFS_STAGING" "$UFS_HDD_IMG"

# ── Assemble HDD image ──────────────────────────────────────────────────

echo "[mkpcimg] Assembling HDD image..."

dd if=/dev/zero of="$IMG_HDD" bs=$SECTOR_SIZE count=$HDD_SECTORS 2>/dev/null

# Write stage1_hdd (first 446 bytes of MBR)
dd if="$STAGE1_HDD" of="$IMG_HDD" bs=1 count=446 conv=notrunc 2>/dev/null

# Write MBR partition table (offset 446, 4 × 16-byte entries)
# Entry 1: active, type=0xA9 (BSD UFS), start_lba=9, size=rest
# CHS values use 0xFE for "use LBA" (common for small disks)
PART_ENTRY="$BUILD_DIR/mbr_part.bin"
python3 -c "
import struct, sys
# Partition entry 1: active, type 0xA9, start LBA 9, size $UFS_HDD_SECTORS
entry = struct.pack('<BBBBBBBBII',
    0x80,       # status: active
    0xFE, 0xFF, 0xFF,  # CHS first (use LBA)
    0xA9,       # type: BSD UFS
    0xFE, 0xFF, 0xFF,  # CHS last (use LBA)
    $UFS_START_SECTOR,  # start LBA
    $UFS_HDD_SECTORS)   # size in sectors
# Entries 2-4: empty
entry += b'\x00' * 48
# Boot signature
entry += struct.pack('<H', 0xAA55)
sys.stdout.buffer.write(entry)
" > "$PART_ENTRY"
dd if="$PART_ENTRY" of="$IMG_HDD" bs=1 seek=446 conv=notrunc 2>/dev/null

# Write stage2 (sectors 1-8)
dd if="$STAGE2_PAD" of="$IMG_HDD" bs=$SECTOR_SIZE seek=1 conv=notrunc 2>/dev/null

# Write UFS (sectors 9+)
dd if="$UFS_HDD_IMG" of="$IMG_HDD" bs=$SECTOR_SIZE seek=$UFS_START_SECTOR conv=notrunc 2>/dev/null

echo "[mkpcimg] HDD: $IMG_HDD ($(wc -c < "$IMG_HDD") bytes)"
echo "[mkpcimg]   stage1_hdd: $(wc -c < "$STAGE1_HDD") bytes"
echo "[mkpcimg]   UFS:        $(wc -c < "$UFS_HDD_IMG") bytes"

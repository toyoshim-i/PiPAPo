#!/usr/bin/env bash
# =============================================================================
# mkpcimg.sh — Assemble PC/XT bootable floppy image
# =============================================================================
#
# Layout (1.44 MB, 2880 × 512-byte sectors):
#   Sector 0          stage1 boot sector (512 B)
#   Sectors 1-8       stage2 UFS loader  (4 KB)
#   Sectors 9+        UFS partition (contains /boot/kernel)
#
# Usage:
#   ./scripts/mkpcimg.sh [build_dir]
#
# Expects build artifacts in the selected build directory:
#   stage1.bin     — boot sector flat binary (512 B)
#   stage2.bin     — stage2 flat binary (≤4 KB)
#   ppap_pcxt.bin — kernel flat binary
#
# Output: build/pcxt/ppap_pcxt.img (1.44 MB floppy image)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$PROJECT_DIR/build/pcxt}"
MKUFS="$PROJECT_DIR/tools/mkufs/mkufs"

STAGE1="$BUILD_DIR/stage1.bin"
STAGE2="$BUILD_DIR/stage2.bin"
KERNEL="$BUILD_DIR/ppap_pcxt.bin"
IMG="$BUILD_DIR/ppap_pcxt.img"

# Floppy parameters
SECTOR_SIZE=512
FLOPPY_SECTORS=2880  # 1.44 MB
STAGE2_SECTORS=8     # 4 KB for stage2
UFS_START_SECTOR=9   # sector 0 = stage1, 1-8 = stage2

# ── Verify inputs ──────────────────────────────────────────────────────────

for f in "$STAGE1" "$STAGE2" "$KERNEL"; do
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
cp "$KERNEL" "$UFS_STAGING/boot/kernel"

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
USER_APPS=(hello getty init pdb push cat ls ps df top)
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
if [[ -f "$USER_BUILD_DIR/runtests.elf" ]]; then
  cp "$USER_BUILD_DIR/runtests.elf" "$UFS_STAGING/bin/runtests"
fi
if [[ -f "$USER_BUILD_DIR/runtests_ext.elf" ]]; then
  cp "$USER_BUILD_DIR/runtests_ext.elf" "$UFS_STAGING/bin/runtests_ext"
fi
if [[ -f "$USER_BUILD_DIR/test_exec.elf" ]]; then
  cp "$USER_BUILD_DIR/test_exec.elf" "$UFS_STAGING/bin/test_exec"
fi
if [[ -f "$USER_BUILD_DIR/test_vfork.elf" ]]; then
  cp "$USER_BUILD_DIR/test_vfork.elf" "$UFS_STAGING/bin/test_vfork"
fi

# ── Create UFS image ─────────────────────────────────────────────────────

# Available space for UFS: sectors 9..2879 = 2871 sectors × 512 = 1,470,072 B
UFS_SIZE=$(( (FLOPPY_SECTORS - UFS_START_SECTOR) * SECTOR_SIZE ))
UFS_IMG="$BUILD_DIR/ufs_boot.img"

echo "[mkpcimg] Creating UFS image ($(( UFS_SIZE / 1024 )) KB)..."
"$MKUFS" -s "$UFS_SIZE" -p "$UFS_STAGING" "$UFS_IMG"

# ── Assemble floppy image ────────────────────────────────────────────────

echo "[mkpcimg] Assembling floppy image..."

# Start with empty 1.44 MB image
dd if=/dev/zero of="$IMG" bs=$SECTOR_SIZE count=$FLOPPY_SECTORS 2>/dev/null

# Write stage1 (sector 0)
dd if="$STAGE1" of="$IMG" bs=$SECTOR_SIZE conv=notrunc 2>/dev/null

# Write stage2 (sectors 1-8, padded to 4 KB)
STAGE2_PAD="$BUILD_DIR/stage2_padded.bin"
dd if=/dev/zero of="$STAGE2_PAD" bs=$SECTOR_SIZE count=$STAGE2_SECTORS 2>/dev/null
dd if="$STAGE2" of="$STAGE2_PAD" bs=1 conv=notrunc 2>/dev/null
dd if="$STAGE2_PAD" of="$IMG" bs=$SECTOR_SIZE seek=1 conv=notrunc 2>/dev/null

# Write UFS (sectors 9+)
dd if="$UFS_IMG" of="$IMG" bs=$SECTOR_SIZE seek=$UFS_START_SECTOR conv=notrunc 2>/dev/null

echo "[mkpcimg] Output: $IMG ($(wc -c < "$IMG") bytes)"
echo "[mkpcimg]   stage1: $(wc -c < "$STAGE1") bytes"
echo "[mkpcimg]   stage2: $(wc -c < "$STAGE2") bytes"
echo "[mkpcimg]   kernel: $(wc -c < "$KERNEL") bytes"
echo "[mkpcimg]   UFS:    $(wc -c < "$UFS_IMG") bytes"

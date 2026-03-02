#!/bin/bash
# prepare_sd.sh — Format SD card and create UFS image files for PPAP
#
# Usage: ./scripts/prepare_sd.sh /dev/sdX
#
# This script formats the first partition as FAT32 and creates UFS
# images for /usr, /home, and /var.  Requires root for mount operations.
#
# Prerequisites:
#   - Build mkufs: cd build && ninja mkufs
#   - SD card with at least one partition

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 /dev/sdX"
    echo "  Formats partition 1 as FAT32 and creates UFS images"
    exit 1
fi

DEVICE="$1"
PARTITION="${DEVICE}1"
MOUNT="/tmp/ppap_sd"
BUILD_DIR="$(cd "$(dirname "$0")/../build" && pwd)"

if [ ! -b "$PARTITION" ]; then
    echo "Error: $PARTITION is not a block device"
    exit 1
fi

if [ ! -x "$BUILD_DIR/mkufs" ]; then
    echo "Error: mkufs not found at $BUILD_DIR/mkufs"
    echo "Run: cd build && ninja mkufs"
    exit 1
fi

echo "=== Formatting $PARTITION as FAT32 ==="
mkfs.vfat -F 32 "$PARTITION"

mkdir -p "$MOUNT"
mount "$PARTITION" "$MOUNT"

echo "=== Creating UFS images ==="
"$BUILD_DIR/mkufs" -s 64K "$MOUNT/ppap_usr.img"
echo "  Created ppap_usr.img (64 KB)"

sync
umount "$MOUNT"
rmdir "$MOUNT" 2>/dev/null || true

echo "=== SD card ready ==="
echo "Flash ppap.elf and boot to test."

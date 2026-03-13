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
# Creates a raw 2HD .XDF floppy image (1,261,568 bytes):
#   offset 0         (1024 B):  boot sector — IPL signature + loader code
#   offset 1024      (N×1024 B): raw kernel binary (ppap_x68k.bin)
#   offset 1024+N×1024 …:        zero padding to full floppy size
#
# The boot sector:
#   1. Saves IPL IOCS vector (TRAP #15 at 0x0000BC)
#   2. Reads kernel from sectors 1..N to 0x006000 via IOCS _B_READ
#   3. Copies kernel .vectors (1024 B) to 0x000000, then restores TRAP #15
#   4. Jumps to Reset_Handler at 0x006400
#
# Usage with emulators:
#   px68k:    px68k -fd0 build/x68k/ppap_x68k.xdf
#   XEiJ:     FD0 → build/x68k/ppap_x68k.xdf
#   XM6 TypeG: FD0 → build/x68k/ppap_x68k.xdf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

KERNEL_BIN="${1:-$PROJECT_DIR/build/x68k/ppap_x68k.bin}"
OUTPUT_XDF="${2:-$PROJECT_DIR/build/x68k/ppap_x68k.xdf}"

M68K_GCC="$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-gcc"
M68K_OBJCOPY="$PROJECT_DIR/tools/m68k-toolchain/bin/m68k-elf-objcopy"
BOOTLDR_S="$PROJECT_DIR/src/target/x68k/boot/bootldr.S"
BOOTLDR_LD="$PROJECT_DIR/src/target/x68k/boot/bootldr.ld"

# ── Verify prerequisites ──────────────────────────────────────────────────────

if [[ ! -f "$KERNEL_BIN" ]]; then
    echo "[mkx68kimg] Error: kernel binary not found: $KERNEL_BIN"
    echo "            Build first: ./scripts/build.sh x68k"
    exit 1
fi

if [[ ! -x "$M68K_GCC" ]]; then
    echo "[mkx68kimg] Error: m68k-elf-gcc not found: $M68K_GCC"
    echo "            Build toolchain: ./third_party/build_gcc_m68k.sh"
    exit 1
fi

# ── X68000 2HD floppy geometry ────────────────────────────────────────────────

FLOPPY_TRACKS=77
FLOPPY_HEADS=2
FLOPPY_SECTORS_PER_TRACK=8
FLOPPY_SECTOR_SIZE=1024
FLOPPY_TOTAL_SECTORS=$(( FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SECTORS_PER_TRACK ))
FLOPPY_SIZE=$(( FLOPPY_TOTAL_SECTORS * FLOPPY_SECTOR_SIZE ))

# ── Calculate kernel sector count ─────────────────────────────────────────────

KERNEL_SIZE=$(stat -c%s "$KERNEL_BIN")
KERNEL_SECTORS=$(( (KERNEL_SIZE + FLOPPY_SECTOR_SIZE - 1) / FLOPPY_SECTOR_SIZE ))
TOTAL_SECTORS=$(( KERNEL_SECTORS + 1 ))  # +1 for boot sector

echo "[mkx68kimg] Kernel:  $KERNEL_BIN"
echo "[mkx68kimg]          $KERNEL_SIZE bytes = $KERNEL_SECTORS sectors × 1024 B"
echo "[mkx68kimg] Floppy:  $TOTAL_SECTORS / $FLOPPY_TOTAL_SECTORS sectors used"

if [[ $TOTAL_SECTORS -gt $FLOPPY_TOTAL_SECTORS ]]; then
    echo "[mkx68kimg] Error: kernel too large for 2HD floppy"
    echo "            Max kernel size: $(( (FLOPPY_TOTAL_SECTORS - 1) * FLOPPY_SECTOR_SIZE )) bytes"
    exit 1
fi

# ── Assemble boot sector ──────────────────────────────────────────────────────

TMPDIR=$(mktemp -d)
trap "rm -rf '$TMPDIR'" EXIT

# Patch KERNEL_SECTORS placeholder in boot loader source
sed "s/KERNEL_SECTORS/${KERNEL_SECTORS}/" "$BOOTLDR_S" > "$TMPDIR/bootldr.S"

# Compile: assemble with m68000 target
$M68K_GCC -m68000 -nostdlib -ffreestanding \
    -T "$BOOTLDR_LD" \
    -Wl,--build-id=none \
    -o "$TMPDIR/bootldr.elf" \
    "$TMPDIR/bootldr.S"

# Extract raw binary (.text section only; LMA=0 → file offset=0)
$M68K_OBJCOPY -O binary -j .text "$TMPDIR/bootldr.elf" "$TMPDIR/bootldr.bin"

BOOT_SIZE=$(stat -c%s "$TMPDIR/bootldr.bin")
echo "[mkx68kimg] Boot sector: $BOOT_SIZE bytes (limit $FLOPPY_SECTOR_SIZE)"

if [[ $BOOT_SIZE -gt $FLOPPY_SECTOR_SIZE ]]; then
    echo "[mkx68kimg] Error: boot sector too large ($BOOT_SIZE > $FLOPPY_SECTOR_SIZE bytes)"
    exit 1
fi

# ── Create floppy image ───────────────────────────────────────────────────────

mkdir -p "$(dirname "$OUTPUT_XDF")"

# Zero-fill the entire floppy image
dd if=/dev/zero of="$OUTPUT_XDF" bs="$FLOPPY_SECTOR_SIZE" count="$FLOPPY_TOTAL_SECTORS" \
    status=none

# Write boot sector at offset 0 (sector 0); zero-padded to full sector
dd if="$TMPDIR/bootldr.bin" of="$OUTPUT_XDF" bs=1 conv=notrunc status=none

# Write kernel binary starting at offset 1024 (logical sector 1)
dd if="$KERNEL_BIN" of="$OUTPUT_XDF" bs=1 seek="$FLOPPY_SECTOR_SIZE" \
    conv=notrunc status=none

echo "[mkx68kimg] Created: $OUTPUT_XDF"
echo ""
echo "[mkx68kimg] To run with an emulator:"
echo "   px68k:     px68k -fd0 $OUTPUT_XDF"
echo "   XEiJ:      set FD0 to $OUTPUT_XDF"
echo "   XM6 TypeG: set FD0 to $OUTPUT_XDF"

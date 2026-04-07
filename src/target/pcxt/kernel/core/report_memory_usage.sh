#!/bin/sh
# report_memory_usage.sh -- Print PC/XT core/VFS size and segment usage

set -eu

SIZE_TOOL="$1"
NM_TOOL="$2"
CORE_ELF="$3"
VFS_ELF="$4"

#
# Reserved area boundaries (must match pcxt_kernel.ld / pcxt_vfs.ld):
#   0x00000-0x005FF  IVT + BIOS data + mod_info_t
#   0x00600-0x09FFF  Core .text/.rodata/.data/.bss   (39424 B)
#   0x0A000-0x0DFFF  VFS  .rodata/.data/.bss         (16384 B)
#   0x0E000-0x0FFFF  Per-process kernel stacks       ( 8192 B fixed)
#   CS=0x1000+       Core .text far copy             (64 KB segment)
#   CS=????+         VFS  .text                      (64 KB segment)
#
CORE_BASE_HEX=0x0600
CORE_DS0_LIMIT_HEX=0xA000        # Core image must end before VFS data
VFS_DATA_BASE_HEX=0xA000
VFS_DATA_LIMIT_HEX=0xE000        # VFS data must end before kernel stacks
KSTACK_BASE_HEX=0xE000
KSTACK_LIMIT_HEX=0x10000

CORE_DS0_RESERVED=$((CORE_DS0_LIMIT_HEX - CORE_BASE_HEX))      # 39424
VFS_DATA_RESERVED=$((VFS_DATA_LIMIT_HEX - VFS_DATA_BASE_HEX))  # 16384
KSTACK_RESERVED=$((KSTACK_LIMIT_HEX - KSTACK_BASE_HEX))        #  8192
CS_SEGMENT_LIMIT=65536           # 64 KB real-mode segment

lookup_symbol() {
  elf="$1"
  sym="$2"
  "$NM_TOOL" -n "$elf" | awk -v want="$sym" '
    $3 == want { print $1; found = 1; exit }
    END { if (!found) exit 1 }
  '
}

format_pct() {
  used="$1"
  limit="$2"
  awk -v used="$used" -v limit="$limit" '
    BEGIN {
      if (limit == 0) {
        printf "n/a";
      } else {
        printf "%.1f%%", (used * 100.0) / limit;
      }
    }
  '
}

core_cs_end_hex="$(lookup_symbol "$CORE_ELF" "__core_cs_end")"
core_bss_end_hex="$(lookup_symbol "$CORE_ELF" "__bss_end")"
vfs_code_end_hex="$(lookup_symbol "$VFS_ELF" "_code_end")"
vfs_data_start_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_start")"
vfs_data_end_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_end")"

# Core .text + .rodata in the runtime far CS (0x1000+).  Must fit in 64 KB.
core_cs_used=$((0x$core_cs_end_hex - CORE_BASE_HEX))

# Core total image (.text + .rodata + .data + .bss) in DS=0.  Must fit
# between 0x0600 and 0xA000 (the VFS data reservation starts at 0xA000).
core_ds_used=$((0x$core_bss_end_hex - CORE_BASE_HEX))

# VFS .text in its runtime far CS.  Must fit in 64 KB.
vfs_cs_used=$((0x$vfs_code_end_hex))

# VFS .rodata + .data + .bss in DS=0.  Must fit between 0xA000 and 0xE000
# (the kernel stack reservation starts at 0xE000).
vfs_ds_used=$((0x$vfs_data_end_hex - 0x$vfs_data_start_hex))

echo
echo "=== PC/XT Memory Usage ==="
"$SIZE_TOOL" "$CORE_ELF" "$VFS_ELF"

echo
echo "=== PC/XT Segment Usage (vs. linker reservations) ==="
printf "  core CS  (.text+.rodata):  %6d / %5d bytes (%s)   [CS 64 KB]\n" \
  "$core_cs_used" "$CS_SEGMENT_LIMIT" \
  "$(format_pct "$core_cs_used" "$CS_SEGMENT_LIMIT")"
printf "  core image in DS=0:        %6d / %5d bytes (%s)   [0x0600..0xA000]\n" \
  "$core_ds_used" "$CORE_DS0_RESERVED" \
  "$(format_pct "$core_ds_used" "$CORE_DS0_RESERVED")"
printf "  VFS  CS  (.text):          %6d / %5d bytes (%s)   [CS 64 KB]\n" \
  "$vfs_cs_used" "$CS_SEGMENT_LIMIT" \
  "$(format_pct "$vfs_cs_used" "$CS_SEGMENT_LIMIT")"
printf "  VFS  data in DS=0:         %6d / %5d bytes (%s)   [0xA000..0xE000]\n" \
  "$vfs_ds_used" "$VFS_DATA_RESERVED" \
  "$(format_pct "$vfs_ds_used" "$VFS_DATA_RESERVED")"
printf "  kernel stacks (fixed):     %6d         bytes               [0xE000..0xFFFF]\n" \
  "$KSTACK_RESERVED"

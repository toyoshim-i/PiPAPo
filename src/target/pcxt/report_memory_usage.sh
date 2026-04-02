#!/bin/sh
# report_memory_usage.sh -- Print PC/XT core/VFS size and segment usage

set -eu

SIZE_TOOL="$1"
NM_TOOL="$2"
CORE_ELF="$3"
VFS_ELF="$4"

CORE_BASE_HEX=0x0600
CORE_LIMIT_HEX=0xA000
DS0_LIMIT_HEX=0x10000
VFS_CODE_LIMIT_HEX=0x10000
VFS_DATA_LIMIT=$((0x10000 - 0xA000))

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

report_core() {
  core_cs_end_hex="$(lookup_symbol "$CORE_ELF" "__core_cs_end")"
  bss_end_hex="$(lookup_symbol "$CORE_ELF" "__bss_end")"
  stack_bottom_hex="$(lookup_symbol "$CORE_ELF" "__stack_bottom")"
  stack_top_hex="$(lookup_symbol "$CORE_ELF" "__stack_top")"
  page_pool_hex="$(lookup_symbol "$CORE_ELF" "__page_pool_start")"
  data_start_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_start")"
  data_end_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_end")"

  core_cs_used=$((0x$core_cs_end_hex - CORE_BASE_HEX))
  core_ds_used=$((0x$bss_end_hex - 0x$core_cs_end_hex))
  stack_used=$((0x$stack_top_hex - 0x$stack_bottom_hex))
  vfs_ds_used=$((0x$data_end_hex - 0x$data_start_hex))
  ds0_limit=$((DS0_LIMIT_HEX - CORE_BASE_HEX))
  total_ds_used=$((core_ds_used + stack_used + vfs_ds_used))
  page_pool_addr=$((0x$page_pool_hex))
  free_ram=$((0x9FC00 - page_pool_addr))

  printf '%s\n' \
    "$core_cs_used" \
    "$core_ds_used" \
    "$stack_used" \
    "$total_ds_used" \
    "$ds0_limit" \
    "$page_pool_addr" \
    "$free_ram"
}

report_vfs() {
  code_end_hex="$(lookup_symbol "$VFS_ELF" "_code_end")"
  data_start_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_start")"
  data_end_hex="$(lookup_symbol "$VFS_ELF" "__vfs_data_end")"

  code_used=$((0x$code_end_hex))
  code_limit=$((VFS_CODE_LIMIT_HEX))
  data_used=$((0x$data_end_hex - 0x$data_start_hex))

  printf '%s\n' "$code_used" "$code_limit" "$data_used"
}

core_report="$(report_core)"
vfs_report="$(report_vfs)"

core_cs_used="$(printf '%s\n' "$core_report" | sed -n '1p')"
core_ds_used="$(printf '%s\n' "$core_report" | sed -n '2p')"
stack_used="$(printf '%s\n' "$core_report" | sed -n '3p')"
total_ds_used="$(printf '%s\n' "$core_report" | sed -n '4p')"
ds0_limit="$(printf '%s\n' "$core_report" | sed -n '5p')"
page_pool_addr="$(printf '%s\n' "$core_report" | sed -n '6p')"
free_ram="$(printf '%s\n' "$core_report" | sed -n '7p')"

vfs_cs_used="$(printf '%s\n' "$vfs_report" | sed -n '1p')"
vfs_code_limit="$(printf '%s\n' "$vfs_report" | sed -n '2p')"
vfs_ds_used="$(printf '%s\n' "$vfs_report" | sed -n '3p')"

echo
echo "=== PC/XT Memory Usage ==="
"$SIZE_TOOL" "$CORE_ELF" "$VFS_ELF"

echo
echo "=== PC/XT Segment Usage ==="
echo "  core CS usage:        ${core_cs_used} / 65536 bytes ($(format_pct "$core_cs_used" 65536))"
echo "  core DS=0 usage:      ${core_ds_used} bytes"
echo "  kernel stack reserve: ${stack_used} bytes"
echo "  VFS CS usage:         ${vfs_cs_used} / ${vfs_code_limit} bytes ($(format_pct "$vfs_cs_used" "$vfs_code_limit"))"
echo "  VFS DS=0 window use:  ${vfs_ds_used} / ${VFS_DATA_LIMIT} bytes ($(format_pct "$vfs_ds_used" "$VFS_DATA_LIMIT"))"
echo "  total DS=0 use:       ${total_ds_used} / ${ds0_limit} bytes ($(format_pct "$total_ds_used" "$ds0_limit"))"
printf '  first free page:      0x%05x (%u bytes free to 0x9FC00)\n' \
  "$page_pool_addr" "$free_ram"

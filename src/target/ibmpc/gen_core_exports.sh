#!/bin/sh
# gen_core_exports.sh — Generate core_exports.ld from core ELF
#
# Extracts all data/BSS symbol addresses from the core module ELF
# and writes a linker symbol file for the VFS module.
#
# Usage: gen_core_exports.sh <nm-tool> <core-elf> <output-ld>

NM="$1"
ELF="$2"
OUT="$3"

# Export only shared data symbols needed by VFS, excluding:
# - linker-script symbols (__*) — conflict with VFS linker script
# - mod_vfs, mod_exec, vfs_fptrs* — these belong to the core binary
#   and must not override VFS's own definitions
# - mod_core — defined in VFS's core_mod_init.c
"$NM" "$ELF" \
  | grep -E ' [BD] ' \
  | grep -v -E '^.{8} . (__|\.)' \
  | grep -v -E 'mod_vfs|mod_exec|mod_core|vfs_fptrs' \
  | awk '{ print $3 " = 0x" $1 ";" }' \
  > "$OUT"

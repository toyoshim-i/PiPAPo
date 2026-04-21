#!/usr/bin/env bash
# =============================================================================
# check_no_extern_in_c.sh — forbid `extern` declarations in .c files
# =============================================================================
#
# Project rule: every symbol declaration belongs in a header.  An `extern`
# keyword inside a .c file (at file scope or tucked inside a function body)
# hides the real dependency chain from the include list.  For function
# symbols use a plain prototype in a header; for variables use a header-
# declared `extern` that the defining .c includes via own-header-first.
#
# Scope for now: src/arch/ and src/target/ — the audit that produced
# docs/proposals/ongoing_cleanup.md section 1.
#
# Approved exceptions are listed below; adding a new one requires a
# comment explaining why a header can't carry the declaration.
#
# Usage:
#   ./scripts/check_no_extern_in_c.sh
#
# Returns 0 if no forbidden extern is found, 1 otherwise.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# File:line pairs that are allowed to keep an `extern` declaration.
# Keep this list short — every entry is a defeat for the rule.
#
# * target_pcxt.c `extern uint16_t vfs_fptrs[]`:
#     vfs_fptrs[] is an asm-defined array in
#     src/target/pcxt/kernel/common/stubs/vfs_stubs.S that target_pcxt.c
#     patches at boot.  C cannot express it as a function prototype and
#     only target_pcxt.c writes to it, so a shared header would be a
#     ceremonial move of the same extern.
ALLOW_REGEX='src/target/pcxt/kernel/core/target_pcxt\.c:[0-9]+:extern uint16_t vfs_fptrs\[\]'

violations="$(grep -rEn '^[[:space:]]*extern[[:space:]]' \
  "$PPAP_ROOT/src/arch" "$PPAP_ROOT/src/target" \
  --include='*.c' 2>/dev/null \
  | sed "s|$PPAP_ROOT/||" \
  | grep -Ev "$ALLOW_REGEX" \
  || true)"

if [[ -n "$violations" ]]; then
  echo "EXTERN-IN-.C VIOLATION:"
  echo "Every symbol declaration must live in a header.  Use a plain"
  echo "function prototype for code symbols, or declare shared state in"
  echo "a header the defining .c file #includes first."
  echo ""
  echo "$violations"
  exit 1
fi

echo "No extern in .c OK"

#!/usr/bin/env bash
# =============================================================================
# check_common_no_c.sh — Disallow .c files in kernel/common/
# =============================================================================
#
# kernel/common/ is for headers only (.h, .inc).  Implementation files (.c)
# belong in kernel/core/, kernel/vfs/, or target-specific directories.
#
# Returns 0 if clean, 1 if violations are found.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

c_files="$(find "$PPAP_ROOT/src/kernel/common" -name '*.c' 2>/dev/null || true)"

if [[ -n "$c_files" ]]; then
  echo "VIOLATION: .c files found in kernel/common/"
  echo "Move implementations to kernel/core/, kernel/vfs/, or target/."
  echo ""
  echo "$c_files"
  exit 1
fi

echo "kernel/common no-.c check OK"
exit 0

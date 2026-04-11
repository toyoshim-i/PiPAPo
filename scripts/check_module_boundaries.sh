#!/usr/bin/env bash
# =============================================================================
# check_module_boundaries.sh — Enforce kernel module include boundaries
# =============================================================================
#
# Rules:
#   1. kernel/core/ must not #include "vfs/" headers directly
#   2. kernel/vfs/ must not #include "core/" headers directly
#
# Cross-module function calls go through kernel/common/mod/ interfaces.
# kernel/common/ is the bridge layer — it may reference both core/ and vfs/.
#
# Usage:
#   ./scripts/check_module_boundaries.sh
#
# Returns 0 if boundaries are clean, 1 if violations are found.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

rc=0

# ── core/ must not include vfs/ ──────────────────────────────────────────────

core_violations="$(grep -rn '#include.*"kernel/vfs/' \
  "$PPAP_ROOT/src/kernel/core" \
  --include='*.c' --include='*.h' \
  2>/dev/null || true)"

if [[ -n "$core_violations" ]]; then
  echo "MODULE BOUNDARY VIOLATION: core/ includes vfs/ directly"
  echo "Use common/mod/mod_vfs.h for cross-module access."
  echo ""
  echo "$core_violations"
  echo ""
  rc=1
fi

# ── vfs/ must not include core/ ──────────────────────────────────────────────

vfs_violations="$(grep -rn '#include.*"kernel/core/' \
  "$PPAP_ROOT/src/kernel/vfs" \
  --include='*.c' --include='*.h' \
  2>/dev/null || true)"

if [[ -n "$vfs_violations" ]]; then
  echo "MODULE BOUNDARY VIOLATION: vfs/ includes core/ directly"
  echo "Use common/mod/mod_core.h for cross-module access."
  echo ""
  echo "$vfs_violations"
  echo ""
  rc=1
fi

# ── mem_region_ptr_ref must not exist ────────────────────────────────────────

ptr_ref_violations="$(grep -rn 'mem_region_ptr_ref' \
  "$PPAP_ROOT/src/kernel" \
  --include='*.c' --include='*.h' \
  2>/dev/null || true)"

if [[ -n "$ptr_ref_violations" ]]; then
  echo "MEM_REGION VIOLATION: mem_region_ptr_ref still in use"
  echo "Use mem_region_kbuf_to_page for kernel buffers."
  echo ""
  echo "$ptr_ref_violations"
  echo ""
  rc=1
fi

if [[ $rc -eq 0 ]]; then
  echo "Module boundaries OK"
fi

exit $rc

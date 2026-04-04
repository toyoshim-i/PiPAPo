#!/usr/bin/env bash
# =============================================================================
# check_allocator_boundaries.sh — Validate allocator API boundaries
# =============================================================================
#
# Enforces the allocator boundary introduced by the allocator migration:
# files outside src/kernel/core/mm/ must not call the page allocator backend
# directly. New code should use mem_region_* instead.
#
# Usage:
#   ./scripts/check_allocator_boundaries.sh
#
# Returns 0 if allocator boundaries are clean, 1 if violations are found.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if command -v rg >/dev/null 2>&1; then
  SEARCH_TOOL=(rg -n)
else
  SEARCH_TOOL=(grep -RIn)
fi

PATTERN='\\bpage_(alloc|free|alloc_at|alloc_contiguous|free_count|max_contiguous)\\s*\\('

violations="$("${SEARCH_TOOL[@]}" \
  "$PATTERN" \
  "$PPAP_ROOT/src" \
  --glob '!src/kernel/core/mm/**' \
  --glob '*.c' \
  --glob '*.h' \
  2>/dev/null || true)"

if [[ -n "$violations" ]]; then
  echo "ALLOCATOR BOUNDARY VIOLATION:"
  echo "Direct page_* usage is only allowed under src/kernel/core/mm/."
  echo "Use mem_region_* from non-mm code instead."
  echo ""
  echo "$violations"
  exit 1
fi

echo "Allocator boundaries OK"

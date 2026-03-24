#!/usr/bin/env bash
# =============================================================================
# check_module_boundaries.sh — Validate kernel module include boundaries
# =============================================================================
#
# Checks that module directories only communicate via mod/ headers.
#
# Rules:
#   1. Inward:  Files outside vfs/ must not include vfs/vfs.h (use mod/mod_vfs.h)
#   2. Outward: Files inside vfs/ must not include other module internals
#
# Usage:
#   ./scripts/check_module_boundaries.sh
#
# Returns 0 if all boundaries are clean, 1 if violations found.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_DIR="$PPAP_ROOT/src/kernel"

# Modules with enforced boundaries.
# Add new modules here as they are migrated.
MODULES="vfs"

ERRORS=0

for mod in $MODULES; do
  dir="$KERNEL_DIR/${mod}"
  [ -d "$dir" ] || continue

  # Rule 1: no external file includes ANY header from this module dir
  violations=$(grep -rn "\"${mod}/\|\"\.\./${mod}/" \
    "$KERNEL_DIR/" --include="*.c" --include="*.h" 2>/dev/null \
    | grep -v "$KERNEL_DIR/${mod}/" \
    | grep -v "$KERNEL_DIR/mod/" || true)

  if [ -n "$violations" ]; then
    echo "BOUNDARY VIOLATION: files outside ${mod}/ include its internal headers:"
    echo "$violations"
    ERRORS=$((ERRORS + 1))
  fi

  # Rule 2: files inside this module don't include other module internals
  for other in $MODULES; do
    [ "$other" = "$mod" ] && continue
    violations=$(grep -rn "\"${other}/\|\"\.\./${other}/" \
      "$KERNEL_DIR/${mod}/" --include="*.c" --include="*.h" 2>/dev/null \
      | grep -v "mod/mod_" || true)

    if [ -n "$violations" ]; then
      echo "BOUNDARY VIOLATION: ${mod}/ includes ${other}/ internal headers:"
      echo "$violations"
      ERRORS=$((ERRORS + 1))
    fi
  done
done

if [ $ERRORS -gt 0 ]; then
  echo ""
  echo "Found $ERRORS boundary violation(s)"
  exit 1
fi

echo "Module boundaries OK ($MODULES)"

#!/usr/bin/env bash
# =============================================================================
# check_clang_format.sh — Check / fix code formatting with clang-format
# =============================================================================
#
# Modes:
#   ./scripts/check_clang_format.sh          Check only (used by build.sh)
#   ./scripts/check_clang_format.sh --fix    Reformat files in place
#
# Scans all .c and .h files under src/kernel/ and src/arch/*/kernel/.
# Skips user/ directories.
#
# If clang-format is not installed, the check is skipped with a warning.
# If installed and violations are found, the check fails with exit 1.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FIX=0
if [[ "${1:-}" == "--fix" ]]; then
  FIX=1
fi

if ! command -v clang-format &>/dev/null; then
  echo "clang-format check: skipped (not installed)"
  echo "       Run: ./scripts/setup.sh host"
  exit 0
fi

# Collect kernel source files (skip user/ dirs)
files=()
while IFS= read -r -d '' f; do
  files+=("$f")
done < <(find "$PPAP_ROOT/src/kernel" "$PPAP_ROOT/src/arch" \
  \( -name '*.c' -o -name '*.h' \) \
  -not -path '*/user/*' \
  -print0 2>/dev/null)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "clang-format check: no files found"
  exit 0
fi

if [[ $FIX -eq 1 ]]; then
  # Fix mode: reformat in place
  fixed=0
  for f in "${files[@]}"; do
    if ! diff -q <(clang-format --style=file "$f") "$f" &>/dev/null; then
      clang-format -i --style=file "$f"
      echo "  formatted: ${f#"$PPAP_ROOT/"}"
      fixed=$((fixed + 1))
    fi
  done
  echo "clang-format: $fixed files reformatted"
  exit 0
fi

# Check mode: dry-run
bad=()
for f in "${files[@]}"; do
  if ! diff -q <(clang-format --style=file "$f") "$f" &>/dev/null; then
    bad+=("${f#"$PPAP_ROOT/"}")
  fi
done

if [[ ${#bad[@]} -gt 0 ]]; then
  echo "CLANG-FORMAT VIOLATION: ${#bad[@]} files need formatting"
  echo "Run: ./scripts/check_clang_format.sh --fix"
  echo ""
  for f in "${bad[@]}"; do
    echo "  $f"
  done
  echo ""
  exit 1
fi

echo "clang-format OK"

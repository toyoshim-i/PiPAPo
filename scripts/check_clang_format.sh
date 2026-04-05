#!/usr/bin/env bash
# =============================================================================
# check_clang_format.sh — Check code formatting with clang-format
# =============================================================================
#
# Checks all .c and .h files under src/kernel/ and src/arch/*/kernel/ against
# the project .clang-format config.  Skips user/ directories.
#
# If clang-format is not installed, the check is skipped with a warning.
# If installed and violations are found, the build fails.
#
# Usage:
#   ./scripts/check_clang_format.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if ! command -v clang-format &>/dev/null; then
  echo "clang-format check: skipped (not installed)"
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

# Check formatting (dry-run)
bad=()
for f in "${files[@]}"; do
  if ! diff -q <(clang-format --style=file "$f") "$f" &>/dev/null; then
    bad+=("${f#"$PPAP_ROOT/"}")
  fi
done

if [[ ${#bad[@]} -gt 0 ]]; then
  echo "CLANG-FORMAT VIOLATION: ${#bad[@]} files need formatting"
  echo "Run: clang-format -i <file> to fix"
  echo ""
  for f in "${bad[@]}"; do
    echo "  $f"
  done
  echo ""
  exit 1
fi

echo "clang-format OK"

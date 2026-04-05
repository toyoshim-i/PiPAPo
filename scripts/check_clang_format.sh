#!/usr/bin/env bash
# =============================================================================
# check_clang_format.sh — Check / fix code formatting with clang-format
# =============================================================================
#
# Modes:
#   ./scripts/check_clang_format.sh          Check dirty files only (default)
#   ./scripts/check_clang_format.sh --all    Check all kernel source files
#   ./scripts/check_clang_format.sh --fix    Reformat dirty files in place
#   ./scripts/check_clang_format.sh --fix --all  Reformat all files
#
# "Dirty files" = files with uncommitted changes (staged + unstaged).
# Scans .c and .h under src/kernel/ and src/arch/*/kernel/, skips user/.
#
# If clang-format is not installed, the check is skipped with a warning.
# If installed and violations are found, the check fails with exit 1.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FIX=0
ALL=0
for arg in "$@"; do
  case "$arg" in
    --fix) FIX=1 ;;
    --all) ALL=1 ;;
  esac
done

if ! command -v clang-format &>/dev/null; then
  echo "clang-format check: skipped (not installed)"
  echo "       Run: ./scripts/setup.sh host"
  exit 0
fi

# Collect files to check
files=()
if [[ $ALL -eq 1 ]]; then
  while IFS= read -r -d '' f; do
    files+=("$f")
  done < <(find "$PPAP_ROOT/src/kernel" "$PPAP_ROOT/src/arch" \
    \( -name '*.c' -o -name '*.h' \) \
    -not -path '*/user/*' \
    -print0 2>/dev/null)
else
  # Dirty files only (staged + unstaged changes)
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    # Only kernel/arch .c/.h files, skip user/
    case "$f" in
      src/kernel/*.c|src/kernel/*.h|src/arch/*.c|src/arch/*.h)
        [[ "$f" == */user/* ]] && continue
        [[ -f "$PPAP_ROOT/$f" ]] && files+=("$PPAP_ROOT/$f")
        ;;
    esac
  done < <(cd "$PPAP_ROOT" && git diff --name-only HEAD 2>/dev/null; \
           cd "$PPAP_ROOT" && git diff --name-only --cached 2>/dev/null)
  # Deduplicate
  if [[ ${#files[@]} -gt 0 ]]; then
    mapfile -t files < <(printf '%s\n' "${files[@]}" | sort -u)
  fi
fi

if [[ ${#files[@]} -eq 0 ]]; then
  echo "clang-format OK (no files to check)"
  exit 0
fi

if [[ $FIX -eq 1 ]]; then
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

# Check mode
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

echo "clang-format OK (${#files[@]} files checked)"

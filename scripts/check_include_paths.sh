#!/usr/bin/env bash
# =============================================================================
# check_include_paths.sh — Enforce full-path #include convention
# =============================================================================
#
# Rules:
#   All #include "..." in kernel and arch code must use full paths
#   rooted at src/.  Valid prefixes:
#     "kernel/"   — kernel headers
#     "common/"   — user-kernel shared types
#     "target/"   — target abstraction
#
# Bare includes like "foo.h" are not allowed — they hide dependencies
# and break the overlay resolution chain.
#
# Exceptions (whitelisted):
#   - ESP-IDF headers (esp_*, sdkconfig.h, freertos/*, hal/*)
#   - ktest.h (test framework, conditionally included)
#
# Usage:
#   ./scripts/check_include_paths.sh
#
# Returns 0 if clean, 1 if violations are found.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Scan kernel/ and arch/*/kernel/ for bare includes (skip user/ dirs)
violations="$(grep -rn '#include "' \
  "$PPAP_ROOT/src/kernel" \
  "$PPAP_ROOT/src/arch" \
  --include='*.c' --include='*.h' \
  2>/dev/null \
  | grep '/kernel/' \
  | grep -v '"kernel/' \
  | grep -v '"common/' \
  | grep -v '"target/' \
  | grep -v '"esp_' \
  | grep -v '"sdkconfig' \
  | grep -v '"freertos/' \
  | grep -v '"hal/' \
  | grep -v '"ktest.h"' \
  | grep -v '"xtensa_api.h"' \
  || true)"

if [[ -n "$violations" ]]; then
  echo "INCLUDE PATH VIOLATION: bare #include found (use full path from src/)"
  echo "Valid prefixes: \"kernel/...\", \"common/...\", \"target/...\""
  echo ""
  echo "$violations"
  echo ""
  echo ""
  exit 1
fi

echo "Include paths OK"
exit 0

#!/usr/bin/env bash
# qemu-test.sh — Run on-target test suite under QEMU and check for PASS
#
# Usage (from project root):
#   ./scripts/qemu-test.sh            # run tests
#   ./scripts/qemu-test.sh --build    # rebuild first, then run tests
#
# Exit code:
#   0 — all on-target tests passed
#   1 — one or more tests failed, or QEMU timed out

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ELF="$PROJECT_DIR/build/ppap_qemu_arm.elf"
TIMEOUT=30

# ── Optional rebuild ──────────────────────────────────────────────────────────
if [[ "${1:-}" == "--build" ]]; then
    echo "[test] Building ppap_qemu_arm..."
    cmake --build "$PROJECT_DIR/build" --target ppap_qemu_arm
fi

# ── Pre-flight checks ─────────────────────────────────────────────────────────
if ! command -v qemu-system-arm &>/dev/null; then
    echo "[test] Error: qemu-system-arm not found."
    exit 1
fi

if [[ ! -f "$ELF" ]]; then
    echo "[test] Error: $ELF not found. Run: cmake --build build --target ppap_qemu_arm"
    exit 1
fi

# ── Run QEMU with timeout, capture output ─────────────────────────────────────
echo "[test] Running on-target tests (timeout ${TIMEOUT}s)..."
OUTPUT=$(timeout "$TIMEOUT" qemu-system-arm \
    -M mps2-an500 \
    -nographic \
    -serial mon:stdio \
    -kernel "$ELF" 2>&1 || true)

echo "$OUTPUT"

# ── Check results ─────────────────────────────────────────────────────────────
if echo "$OUTPUT" | grep -q "ALL.*TESTS PASSED"; then
    echo ""
    echo "[test] PASS — all on-target tests passed"
    exit 0
else
    echo ""
    echo "[test] FAIL — tests did not all pass (or QEMU timed out)"
    exit 1
fi

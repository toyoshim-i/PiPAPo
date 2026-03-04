#!/usr/bin/env bash
# test_all_targets.sh — Build all targets and run QEMU automated tests
#
# Usage (from project root):
#   ./scripts/test_all_targets.sh
#
# Steps:
#   1. Build all three targets (production, PPAP_TESTS=OFF)
#   2. Build QEMU target with tests (PPAP_TESTS=ON)
#   3. Run QEMU automated test suite
#   4. Print binary sizes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

echo "=== Building all targets (production) ==="
cmake -B build -DPPAP_TESTS=OFF
cmake --build build -- ppap_qemu_arm ppap_pico1 ppap_pico1calc

echo ""
echo "=== Building QEMU with tests ==="
cmake -B build_test -DPPAP_TESTS=ON
cmake --build build_test -- ppap_qemu_arm

echo ""
echo "=== QEMU automated test ==="
ELF="$PROJECT_DIR/build_test/ppap_qemu_arm.elf"
TIMEOUT=30
OUTPUT=$(timeout "$TIMEOUT" qemu-system-arm \
    -M mps2-an500 \
    -nographic \
    -serial mon:stdio \
    -kernel "$ELF" 2>&1 || true)

echo "$OUTPUT"

if echo "$OUTPUT" | grep -q "ALL.*TESTS PASSED"; then
    echo ""
    echo "[test] PASS — all on-target tests passed"
else
    echo ""
    echo "[test] FAIL — tests did not all pass (or QEMU timed out)"
    exit 1
fi

echo ""
echo "=== Build sizes (production) ==="
arm-none-eabi-size build/ppap_qemu_arm.elf \
                   build/ppap_pico1.elf \
                   build/ppap_pico1calc.elf 2>/dev/null || true

echo ""
echo "=== All targets OK ==="

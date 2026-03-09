#!/usr/bin/env bash
# test_all_targets.sh — Build all targets and run QEMU automated tests
#
# Usage (from project root):
#   ./scripts/test_all_targets.sh
#
# Steps:
#   1. Build all targets (production, PPAP_TESTS=OFF)
#   2. Print binary sizes
#   3. Build & run QEMU test suite (PPAP_TESTS=ON via run.sh --test)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

echo "=== Building all targets (production) ==="
cmake -B build/arm_m -DPPAP_TESTS=OFF
cmake --build build/arm_m -- ppap_qemu_arm ppap_pico1 ppap_pico1calc

echo ""
echo "=== Build sizes (production) ==="
arm-none-eabi-size build/arm_m/ppap_qemu_arm.elf \
                   build/arm_m/ppap_pico1.elf \
                   build/arm_m/ppap_pico1calc.elf 2>/dev/null || true

echo ""
echo "=== QEMU automated test (ARM) ==="
"$SCRIPT_DIR/run.sh" --test qemu_arm

echo ""
echo "=== All targets OK ==="

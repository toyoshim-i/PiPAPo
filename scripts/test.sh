#!/bin/sh
# scripts/test.sh — Build and run the PPAP unit-test suite (host native build)
#
# Usage:
#   ./scripts/test.sh            # run from the repo root
#   ./scripts/test.sh --verbose  # show all test output, not just failures
#
# The test suite uses a separate CMake project (tests/CMakeLists.txt) that
# compiles kernel source files with the host gcc.  No ARM toolchain needed.
#
# Prerequisites:
#   - cmake >= 3.13
#   - host gcc / clang

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build_tests"
VERBOSE="${1:-}"

echo "=== PPAP unit tests ==="
echo "Source: ${REPO_ROOT}/tests"
echo "Build:  ${BUILD_DIR}"
echo ""

# Configure (quiet unless something goes wrong)
cmake -S "${REPO_ROOT}/tests" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc \
    > /dev/null

# Build
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo ""

# Run
if [ "${VERBOSE}" = "--verbose" ]; then
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -V
else
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

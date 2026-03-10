# arm_m.cmake — Common setup for ARM Cortex-M targets
#
# Include AFTER project() in each ARM target's CMakeLists.txt.
# Expects PPAP_ROOT to be set by the caller.
#
# Provides:
#   - ppap_arm_target_common(target): common include dirs + test support
#   - All ppap_userland.cmake functions and variables

include_guard(GLOBAL)

# Shared build directory for userland artifacts (musl, busybox, etc.).
# All ARM targets share one build to avoid redundant musl/busybox rebuilds.
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/arm_m")

# Common build config (kernel sources, userland, romfs)
include(${CMAKE_CURRENT_LIST_DIR}/ppap_userland.cmake)

# ppap_arm_target_common(target)
#
# Adds standard include dirs and PPAP_TESTS support to an ARM target.
function(ppap_arm_target_common target)
    target_include_directories(${target} PRIVATE
        ${PPAP_ROOT}/src
        ${PPAP_ROOT}/src/kernel
    )
    if(PPAP_TESTS)
        target_sources(${target} PRIVATE ${PPAP_ROOT}/tests/kernel/ktest.c)
        target_include_directories(${target} PRIVATE ${PPAP_ROOT}/tests/kernel)
        target_compile_definitions(${target} PRIVATE PPAP_TESTS=1)
    endif()
endfunction()

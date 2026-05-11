# arm_m.cmake — Common setup for ARM Cortex-M targets
#
# Include AFTER project() in each ARM target's CMakeLists.txt.
# Expects PPAP_ROOT to be set by the caller.
#
# Provides:
#   - ppap_arm_target_common(target): common include dirs + test support
#   - All user.cmake functions and variables

include_guard(GLOBAL)

# Shared build directory for userland artifacts (rogue, etc.).
# All ARM targets share one build to avoid redundant musl/rogue rebuilds.
if(PPAP_ARM_HARDFLOAT)
    set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/arm_m33")
else()
    set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/arm_m")
endif()

# Userland build config (rogue, romfs pipeline)
# Must be included before kernel.cmake to define PPAP_ENABLE_* options
include(${CMAKE_CURRENT_LIST_DIR}/user.cmake)

# Kernel source lists (KERNEL_COMMON_SOURCES, KERNEL_BLKDEV_SOURCES, etc.)
# Depends on PPAP_ENABLE_* options defined in user.cmake
include(${CMAKE_CURRENT_LIST_DIR}/kernel.cmake)

# ppap_arm_target_common(target)
#
# Adds standard include dirs and PPAP_TESTS support to an ARM target.
# Also applies subsystem/eCPU build flags.
function(ppap_arm_target_common target)
    # Include order: target-specific dirs are added by per-target
    # CMakeLists BEFORE this call, so the resolution chain is
    #   target overlay  ->  arch overlay  ->  primary src/
    # (first-match-wins; target overrides arch overrides primary).
    target_include_directories(${target} PRIVATE
        ${PPAP_ROOT}/src/arch/arm_m
        ${PPAP_ROOT}/src
    )
    
    # Warnings: treat as errors for project code (third-party is built externally)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

    # Core kernel definitions
    target_compile_definitions(${target} PRIVATE
        PPAP_KERNEL=1
        # TODO: reduce after CP/M/deep subsystem stack paths are optimized.
        PROC_KSTACK_SIZE=2048u
    )
    
    # Subsystem and eCPU build flags
    if(PPAP_ENABLE_HUMAN68K)
        target_compile_definitions(${target} PRIVATE PPAP_ENABLE_HUMAN68K=1)
    endif()
    if(PPAP_ENABLE_CPM)
        target_compile_definitions(${target} PRIVATE PPAP_ENABLE_CPM=1)
    endif()
    if(PPAP_ENABLE_SOS)
        target_compile_definitions(${target} PRIVATE PPAP_ENABLE_SOS=1)
    endif()
    if(PPAP_ENABLE_ECPU_M68K)
        target_compile_definitions(${target} PRIVATE PPAP_ENABLE_ECPU_M68K=1)
    endif()
    if(PPAP_ENABLE_ECPU_Z80)
        target_compile_definitions(${target} PRIVATE PPAP_ENABLE_ECPU_Z80=1)
    endif()
    if(PPAP_ENABLE_HUMAN68K OR PPAP_ENABLE_CPM OR PPAP_ENABLE_SOS OR PPAP_ENABLE_ECPU_M68K OR PPAP_ENABLE_ECPU_Z80)
        target_compile_definitions(${target} PRIVATE PPAP_HAS_SUBSYS=1)
    endif()

    # Test suite
    if(PPAP_TESTS)
        target_sources(${target} PRIVATE ${PPAP_ROOT}/tests/kernel/ktest.c)
        target_include_directories(${target} PRIVATE ${PPAP_ROOT}/tests/kernel)
        target_compile_definitions(${target} PRIVATE PPAP_TESTS=1)
    endif()
    if(PPAP_TESTS_EXTENDED)
        target_compile_definitions(${target} PRIVATE PPAP_TESTS_EXTENDED=1)
    endif()

    if(H68K_DEBUG)
        target_compile_definitions(${target} PRIVATE PPAP_DEBUG_LOG=1)
    endif()

    # Semihosting: replace hardware UART with bkpt 0xAB I/O
    if(PPAP_SEMIHOST)
        target_compile_definitions(${target} PRIVATE PPAP_SEMIHOST=1)
    endif()
endfunction()

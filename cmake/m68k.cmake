# m68k.cmake — Common setup for Motorola 68000 targets
#
# Include AFTER project() in each m68k target's CMakeLists.txt.
# Expects PPAP_ROOT to be set by the caller.
#
# Provides:
#   - ppap_m68k_target_common(target): common include dirs + test support
#   - All user.cmake functions and variables

include_guard(GLOBAL)

# Shared build directory for userland artifacts (rogue, etc.).
# All m68k targets share one build to avoid redundant musl/rogue rebuilds.
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/m68k")

# Userland build config (rogue, romfs pipeline)
# Must be included before kernel.cmake to define PPAP_ENABLE_* options
include(${CMAKE_CURRENT_LIST_DIR}/user.cmake)

# Kernel source lists (ARCH_M68K_SOURCES, KERNEL_SHARED_SOURCES, etc.)
# Depends on PPAP_ENABLE_* options defined in user.cmake
include(${CMAKE_CURRENT_LIST_DIR}/kernel.cmake)

# ppap_m68k_target_common(target)
#
# Adds standard include dirs and PPAP_TESTS support to an m68k target.
# Also applies subsystem/eCPU build flags.
function(ppap_m68k_target_common target)
    target_include_directories(${target} PRIVATE
        ${PPAP_ROOT}/src/arch/m68k/include
        ${PPAP_ROOT}/src
        ${PPAP_ROOT}/src/arch/m68k
    )
    
    # Warnings: treat as errors for project code (third-party is built externally)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

    # Core kernel definitions
    target_compile_definitions(${target} PRIVATE
        PPAP_KERNEL=1
        # TODO: shrink to 1 KB after fixed-kstack high-water measurements.
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
    
    # Human68k debug support
    if(H68K_DEBUG)
        target_compile_definitions(${target} PRIVATE PPAP_DEBUG_LOG=1)
    endif()
endfunction()

# m68k.cmake — Common setup for Motorola 68000 targets
#
# Include AFTER project() in each m68k target's CMakeLists.txt.
# Expects PPAP_ROOT to be set by the caller.
#
# Provides:
#   - ppap_m68k_target_common(target): common include dirs + test support
#   - All user.cmake functions and variables

include_guard(GLOBAL)

# Kernel source lists (ARCH_M68K_SOURCES, KERNEL_SHARED_SOURCES, etc.)
include(${CMAKE_CURRENT_LIST_DIR}/kernel.cmake)

# Shared build directory for userland artifacts (musl, busybox, etc.).
# All m68k targets share one build to avoid redundant musl/busybox rebuilds.
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/m68k")

# Userland build config (musl, busybox, romfs pipeline)
include(${CMAKE_CURRENT_LIST_DIR}/user.cmake)

# ppap_m68k_target_common(target)
#
# Adds standard include dirs and PPAP_TESTS support to an m68k target.
function(ppap_m68k_target_common target)
    target_include_directories(${target} PRIVATE
        ${PPAP_ROOT}/src/arch/m68k/include
        ${PPAP_ROOT}/src
        ${PPAP_ROOT}/src/kernel
    )
    if(PPAP_TESTS)
        target_sources(${target} PRIVATE ${PPAP_ROOT}/tests/kernel/ktest.c)
        target_include_directories(${target} PRIVATE ${PPAP_ROOT}/tests/kernel)
        target_compile_definitions(${target} PRIVATE PPAP_TESTS=1)
    endif()
endfunction()

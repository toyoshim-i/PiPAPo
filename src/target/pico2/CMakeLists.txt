# CMakeLists.txt — Raspberry Pi Pico 2 target (standalone build)
#
# Build:
#   cmake -S src/target/pico2 -B build/pico2
#   cmake --build build/pico2

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# RP2350 ARM Secure world
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD pico2)

# Pico SDK must be set up before project()
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        set(PICO_SDK_PATH "${PPAP_ROOT}/third_party/pico-sdk")
    endif()
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ppap_pico2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)
option(PPAP_SEMIHOST "Use ARM semihosting for serial I/O" OFF)

# User-space binaries use hardware FPU with softfp ABI (compatible with
# soft-float musl/busybox — float args still passed in integer regs).
set(PPAP_ARM_HARDFLOAT ON)

include(${PPAP_ROOT}/cmake/arm_m.cmake)

add_executable(ppap_pico2
    ${KERNEL_COMMON_SOURCES}
    ${PPAP_ROOT}/src/arch/arm_m/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/boot/picobin_block.S
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/target_pico2.c
    ${PPAP_ROOT}/src/arch/arm_m/kernel/core/driver/clock_rpico.c
)

if(PPAP_SEMIHOST)
    target_sources(ppap_pico2 PRIVATE
        ${PPAP_ROOT}/src/arch/arm_m/kernel/vfs/driver/semihost.c
    )
    target_compile_definitions(ppap_pico2 PRIVATE PPAP_SEMIHOST=1)
else()
    target_sources(ppap_pico2 PRIVATE
        ${PPAP_ROOT}/src/arch/arm_m/kernel/vfs/driver/uart_rpico.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/pico2_logger.c
    )
endif()

ppap_arm_target_common(ppap_pico2)

target_include_directories(ppap_pico2 PRIVATE
    ${PPAP_ROOT}/src/arch/arm_m
    ${PPAP_ROOT}/src/target/pico2
)

target_compile_definitions(ppap_pico2 PRIVATE
    PPAP_HAS_PROCFS=1
    PPAP_SYS_HZ=150000000u
    PPAP_PLL_FBDIV=125u
    PPAP_PLL_PD1=5u
    PPAP_PLL_PD2=2u
    PAGE_POOL_BASE=0x20007000u
    PAGE_COUNT_MAX=115u
)

target_link_libraries(ppap_pico2 pico_base_headers hardware_regs)
# No boot2 needed — RP2350 ROM handles QSPI/XIP setup natively

target_link_options(ppap_pico2 PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/pico2.ld
    -nostartfiles
    -Wl,--gc-sections
)

pico_add_extra_outputs(ppap_pico2)

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_pico2
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

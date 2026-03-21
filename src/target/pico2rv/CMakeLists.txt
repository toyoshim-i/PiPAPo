# CMakeLists.txt — Raspberry Pi Pico 2 RISC-V target (standalone build)
#
# Build:
#   cmake -S src/target/pico2rv -B build/pico2rv
#   cmake --build build/pico2rv

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# RP2350 RISC-V mode
set(PICO_PLATFORM rp2350-riscv)
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

project(ppap_pico2rv C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

include(${PPAP_ROOT}/cmake/riscv.cmake)

add_executable(ppap_pico2rv
    ${KERNEL_RISCV_COMMON_SOURCES}
    ${PPAP_ROOT}/src/arch/riscv/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/picobin_block.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico2rv.c
    ${PPAP_ROOT}/src/drivers/arch/arm_m/uart_rpico.c
    ${PPAP_ROOT}/src/drivers/arch/arm_m/clock_rpico.c
)

ppap_riscv_target_common(ppap_pico2rv)

target_compile_definitions(ppap_pico2rv PRIVATE
    PPAP_SYS_HZ=150000000u
    PPAP_PLL_FBDIV=125u
    PPAP_PLL_PD1=5u
    PPAP_PLL_PD2=2u
    PAGE_POOL_BASE=0x20006000u
    PAGE_COUNT_MAX=116u
)

target_link_libraries(ppap_pico2rv pico_base_headers hardware_regs)
# No boot2 needed — RP2350 ROM handles QSPI/XIP setup natively

target_link_options(ppap_pico2rv PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/pico2rv.ld
    -nostartfiles
    -Wl,--gc-sections
)

pico_add_extra_outputs(ppap_pico2rv)

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_pico2rv
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

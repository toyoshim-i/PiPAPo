# CMakeLists.txt — Raspberry Pi Pico target (standalone build)
#
# Build:
#   cmake -S src/target/pico1 -B build/pico1
#   cmake --build build/pico1

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Pico SDK must be set up before project()
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        set(PICO_SDK_PATH "${PPAP_ROOT}/third_party/pico-sdk")
    endif()
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ppap_pico1 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

include(${PPAP_ROOT}/cmake/arm_m.cmake)

add_executable(ppap_pico1
    ${KERNEL_COMMON_SOURCES}
    ${PPAP_ROOT}/src/arch/arm_m/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/target_pico1.c
    ${PPAP_ROOT}/src/arch/arm_m/kernel/core/driver/uart_rpico.c
    ${PPAP_ROOT}/src/arch/arm_m/kernel/core/driver/clock_rpico.c
)

ppap_arm_target_common(ppap_pico1)

target_include_directories(ppap_pico1 PRIVATE
    ${PPAP_ROOT}/src/arch/arm_m
    ${PPAP_ROOT}/src/target/pico1
)

target_link_libraries(ppap_pico1 pico_base_headers hardware_regs)
target_sources(ppap_pico1 PRIVATE $<TARGET_OBJECTS:bs2_default_library>)

target_link_options(ppap_pico1 PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/pico1.ld
    -nostartfiles
    -Wl,--gc-sections
)

pico_add_extra_outputs(ppap_pico1)

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_pico1
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

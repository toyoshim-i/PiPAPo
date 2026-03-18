# CMakeLists.txt — ClockworkPi PicoCalc target (standalone build)
#
# Build:
#   cmake -S src/target/pico1calc -B build/pico1calc
#   cmake --build build/pico1calc

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

project(ppap_pico1calc C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

include(${PPAP_ROOT}/cmake/arm_m.cmake)

add_executable(ppap_pico1calc
    ${KERNEL_COMMON_SOURCES}
    ${KERNEL_BLKDEV_SOURCES}
    ${PPAP_ROOT}/src/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico1calc.c
    ${PPAP_ROOT}/src/drivers/arch/arm_m/uart_rp2040.c
    ${PPAP_ROOT}/src/drivers/clock.c
    ${PPAP_ROOT}/src/drivers/spi.c
    ${PPAP_ROOT}/src/drivers/sd.c
    ${PPAP_ROOT}/src/drivers/i2c.c
    ${PPAP_ROOT}/src/drivers/spi_lcd.c
    ${PPAP_ROOT}/src/drivers/lcd.c
    ${PPAP_ROOT}/src/drivers/kbd.c
    ${PPAP_ROOT}/src/drivers/fbcon.c
)

ppap_arm_target_common(ppap_pico1calc)

target_compile_definitions(ppap_pico1calc PRIVATE
    PPAP_HAS_BLKDEV=1
    SRAM_KERNEL_SIZE=24576u
    PAGE_POOL_BASE=0x20006000u
    PAGE_COUNT_MAX=50u
)
target_link_libraries(ppap_pico1calc pico_base_headers hardware_regs)
target_sources(ppap_pico1calc PRIVATE $<TARGET_OBJECTS:bs2_default_library>)

target_link_options(ppap_pico1calc PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/pico1calc.ld
    -nostartfiles
    -Wl,--gc-sections
)

pico_add_extra_outputs(ppap_pico1calc)

# UF2 sanitization for PicoCalc bootloader
add_custom_command(TARGET ppap_pico1calc POST_BUILD
    COMMAND python3 ${PPAP_ROOT}/tools/uf2sanitize.py
        ${CMAKE_CURRENT_BINARY_DIR}/ppap_pico1calc.uf2
        ${CMAKE_CURRENT_BINARY_DIR}/ppap_pico1calc.uf2
        --start 0x10000100
    VERBATIM
)

# =============================================================================
# Font generation pipeline (LCD console)
# =============================================================================

find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/font8x16.c
    COMMAND ${Python3_EXECUTABLE} ${PPAP_ROOT}/tools/bdf2c.py
            --input ${PPAP_ROOT}/third_party/fonts/8x16.bdf
            --output ${CMAKE_BINARY_DIR}/generated/font8x16.c
            --name font8x16 --width 8 --height 16
    DEPENDS ${PPAP_ROOT}/third_party/fonts/8x16.bdf
            ${PPAP_ROOT}/tools/bdf2c.py
    COMMENT "Generating font8x16.c from 8x16.bdf"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/font8x8.c
    COMMAND ${Python3_EXECUTABLE} ${PPAP_ROOT}/tools/json2c.py
            --input ${PPAP_ROOT}/third_party/fonts/ichigojam-font.json
            --output ${CMAKE_BINARY_DIR}/generated/font8x8.c
            --name font8x8
    DEPENDS ${PPAP_ROOT}/third_party/fonts/ichigojam-font.json
            ${PPAP_ROOT}/tools/json2c.py
    COMMENT "Generating font8x8.c from IchigoJam font"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/font4x8.c
    COMMAND ${Python3_EXECUTABLE} ${PPAP_ROOT}/tools/bdf2c.py
            --input ${PPAP_ROOT}/third_party/fonts/4x6.bdf
            --output ${CMAKE_BINARY_DIR}/generated/font4x8.c
            --name font4x8 --width 4 --height 8
    DEPENDS ${PPAP_ROOT}/third_party/fonts/4x6.bdf
            ${PPAP_ROOT}/tools/bdf2c.py
    COMMENT "Generating font4x8.c from 4x6.bdf"
)

add_custom_target(generate_fonts
    DEPENDS ${CMAKE_BINARY_DIR}/generated/font8x16.c
            ${CMAKE_BINARY_DIR}/generated/font8x8.c
            ${CMAKE_BINARY_DIR}/generated/font4x8.c
)
add_dependencies(ppap_pico1calc generate_fonts)

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/generated/font8x16.c
    ${CMAKE_BINARY_DIR}/generated/font8x8.c
    ${CMAKE_BINARY_DIR}/generated/font4x8.c
    PROPERTIES GENERATED TRUE
)
target_sources(ppap_pico1calc PRIVATE
    ${CMAKE_BINARY_DIR}/generated/font8x16.c
    ${CMAKE_BINARY_DIR}/generated/font8x8.c
    ${CMAKE_BINARY_DIR}/generated/font4x8.c
)

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_pico1calc
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

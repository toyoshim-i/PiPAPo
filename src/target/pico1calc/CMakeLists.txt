add_executable(ppap_pico1calc
    ${KERNEL_COMMON_SOURCES}
    ${KERNEL_BLKDEV_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico1calc.c
    ${CMAKE_SOURCE_DIR}/src/drivers/uart.c
    ${CMAKE_SOURCE_DIR}/src/drivers/clock.c
    ${CMAKE_SOURCE_DIR}/src/drivers/spi.c
    ${CMAKE_SOURCE_DIR}/src/drivers/sd.c
    ${CMAKE_SOURCE_DIR}/src/drivers/i2c.c
    ${CMAKE_SOURCE_DIR}/src/drivers/spi_lcd.c
    ${CMAKE_SOURCE_DIR}/src/drivers/lcd.c
    ${CMAKE_SOURCE_DIR}/src/drivers/kbd.c
    ${CMAKE_SOURCE_DIR}/src/drivers/fbcon.c
)

target_include_directories(ppap_pico1calc PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/kernel
)
target_compile_definitions(ppap_pico1calc PRIVATE PPAP_HAS_BLKDEV=1)
target_link_libraries(ppap_pico1calc pico_base_headers hardware_regs)
target_sources(ppap_pico1calc PRIVATE $<TARGET_OBJECTS:bs2_default_library>)

target_link_options(ppap_pico1calc PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/pico1calc.ld
    -nostartfiles
    -Wl,--gc-sections
)

# Produce ppap_pico1calc.bin, ppap_pico1calc.uf2 alongside ppap_pico1calc.elf
pico_add_extra_outputs(ppap_pico1calc)

# Post-process UF2 for PicoCalc's third-party UF2 bootloader (pelrun/uf2loader).
# The bootloader requires contiguous blocks and rejects blocks targeting its
# own reserved first 16KB (0x10000000–0x10003FFF).  Overwrite the SDK-generated
# UF2 in place so the output filename stays ppap_pico1calc.uf2.
add_custom_command(TARGET ppap_pico1calc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Sanitizing UF2 for PicoCalc bootloader..."
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/uf2sanitize.py
        ${CMAKE_CURRENT_BINARY_DIR}/ppap_pico1calc.uf2
        ${CMAKE_CURRENT_BINARY_DIR}/ppap_pico1calc.uf2
        --start 0x10000100
    VERBATIM
)

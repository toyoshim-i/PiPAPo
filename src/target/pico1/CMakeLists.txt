add_executable(ppap_pico1
    ${KERNEL_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/boot/stage1.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_pico1.c
    ${CMAKE_SOURCE_DIR}/src/drivers/uart.c
    ${CMAKE_SOURCE_DIR}/src/drivers/clock.c
)

target_include_directories(ppap_pico1 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/kernel
)
target_link_libraries(ppap_pico1 pico_base_headers hardware_regs)
target_sources(ppap_pico1 PRIVATE $<TARGET_OBJECTS:bs2_default_library>)

target_link_options(ppap_pico1 PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/pico1.ld
    -nostartfiles
    -Wl,--gc-sections
)

# Produce ppap_pico1.bin, ppap_pico1.uf2 alongside ppap_pico1.elf
pico_add_extra_outputs(ppap_pico1)

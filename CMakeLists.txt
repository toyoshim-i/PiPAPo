cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(ppap C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

add_executable(ppap
    # src/boot/startup.S  -- added in Step 4 (replaces pico_crt0)
    src/kernel/main.c
    src/drivers/uart.c
)

target_include_directories(ppap PRIVATE src)

# hardware_regs: register definitions only, no runtime library dependency
target_link_libraries(ppap
    pico_stdlib
    hardware_uart
    hardware_clocks
    hardware_gpio
)

# Custom linker script — activated in Step 4 (replaces pico_crt0 default script)
# pico_set_linker_script(ppap ${CMAKE_SOURCE_DIR}/ldscripts/ppap.ld)

# Produce ppap.bin and ppap.uf2 alongside ppap.elf
pico_add_extra_outputs(ppap)

# Route stdout/stderr to UART (not USB) — we implement our own UART driver
# but pico_stdlib needs a console target set
pico_enable_stdio_uart(ppap 0)
pico_enable_stdio_usb(ppap 0)

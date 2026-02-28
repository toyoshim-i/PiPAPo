cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(ppap C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

add_executable(ppap
    src/boot/startup.S      # Vector table + Reset_Handler (Step 4)
    src/kernel/main.c
    src/drivers/uart.c
)

target_include_directories(ppap PRIVATE src)

# Bare SDK headers only — no pico_stdlib / pico_runtime / pico_crt0.
# We provide our own startup.S entry point.
# hardware_* implementation libraries will be added in Steps 5+ (UART, clocks).
target_link_libraries(ppap
    pico_base_headers   # Platform / board definitions (INTERFACE, headers only)
    hardware_regs       # RP2040 register address definitions (INTERFACE, headers only)
    bs2_default_library # Pico SDK boot stage 2 (.boot2 section); replaced in Step 8
)

# Custom linker script (ppap.ld) + bare-metal link flags:
#   -nostartfiles    do not link default crt0 / startup files
#   --gc-sections    discard unused sections to keep binary small
target_link_options(ppap PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/ppap.ld
    -nostartfiles
    -Wl,--gc-sections
)

# Produce ppap.bin and ppap.uf2 alongside ppap.elf
pico_add_extra_outputs(ppap)

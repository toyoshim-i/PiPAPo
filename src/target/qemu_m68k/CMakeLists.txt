# CMakeLists.txt — QEMU m68k target (standalone build)
#
# This target builds independently from the ARM/Pico SDK targets.
#
# Build:
#   mkdir -p build-m68k && cd build-m68k
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-m68k.cmake \
#         -S ../src/target/qemu_m68k -B .
#   make
#
# Run:
#   qemu-system-m68k -machine virt -cpu m68000 -nographic \
#         -kernel ppap_qemu_m68k.elf

cmake_minimum_required(VERSION 3.13)

project(ppap_qemu_m68k C ASM)
set(CMAKE_C_STANDARD 11)

# Project root (three levels up from src/target/qemu_m68k/)
set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Phase B: minimal bringup — only the target provides kmain().
# Phase C will add shared kernel sources (proc, vfs, etc.) as they are ported.

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_qemu_m68k
    ${PPAP_ROOT}/src/arch/m68k/boot.S
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu_m68k.c
)

target_include_directories(ppap_qemu_m68k PRIVATE
    ${PPAP_ROOT}/src
    ${PPAP_ROOT}/src/kernel
)

target_compile_definitions(ppap_qemu_m68k PRIVATE
    PPAP_QEMU=1
    PPAP_M68K=1
)

target_compile_options(ppap_qemu_m68k PRIVATE
    -m68000
    -Wall -Wextra -Wno-unused-parameter
    -Os -g
    -ffreestanding -fno-builtin
)

target_link_options(ppap_qemu_m68k PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/qemu_m68k.ld
    -nostartfiles -nostdlib -nodefaultlibs
    -Wl,--gc-sections
    -Wl,--build-id=none
)

# Link libgcc for 68000 helper functions (__udivsi3, __umodsi3, etc.)
# The 68000 has no hardware divide — GCC emits calls into libgcc.
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -m68000 -print-libgcc-file-name
    OUTPUT_VARIABLE M68K_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
target_link_libraries(ppap_qemu_m68k PRIVATE ${M68K_LIBGCC})

# Generate binary and size report
add_custom_command(TARGET ppap_qemu_m68k POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ppap_qemu_m68k.elf ppap_qemu_m68k.bin
    COMMAND ${CMAKE_SIZE} ppap_qemu_m68k.elf
    COMMENT "Generating ppap_qemu_m68k.bin"
)

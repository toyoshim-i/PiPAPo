# CMakeLists.txt — QEMU m68k target (standalone build)
#
# This target builds independently from the ARM/Pico SDK targets.
#
# Build:
#   mkdir -p build/m68k && cd build/m68k
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-m68k.cmake \
#         -S ../../src/target/qemu_m68k -B .
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

# Phase C: shared kernel sources compiled for m68k.
# ARM-only files (smp.c, mpu.c, xip.c) are excluded.

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_qemu_m68k
    # Architecture
    ${PPAP_ROOT}/src/arch/m68k/boot.S
    ${PPAP_ROOT}/src/arch/m68k/m68k_common.c
    ${PPAP_ROOT}/src/arch/m68k/string.c
    ${PPAP_ROOT}/src/arch/m68k/divmod.c
    ${PPAP_ROOT}/src/arch/m68k/switch.S
    ${PPAP_ROOT}/src/arch/m68k/trap.S

    # Target
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_qemu_m68k.c

    # Shared kernel — core subsystems
    ${PPAP_ROOT}/src/kernel/klog.c
    ${PPAP_ROOT}/src/kernel/mm/page.c
    ${PPAP_ROOT}/src/kernel/mm/kmem.c
    ${PPAP_ROOT}/src/kernel/proc/proc.c
    ${PPAP_ROOT}/src/kernel/proc/sched.c
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

# Link libgcc for 68000 helper functions (__mulsi3, etc.).
# Division helpers (__udivsi3, __umodsi3, __divsi3, __modsi3) are
# provided by src/arch/m68k/divmod.c because the system libgcc uses
# 68020+ instructions (BSR.L) that fault on the 68000.  Our versions
# override the libgcc ones (linker picks first definition).
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

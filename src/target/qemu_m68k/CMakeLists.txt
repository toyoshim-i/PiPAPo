# CMakeLists.txt — QEMU m68k target (standalone build)
#
# Build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake \
#         -S src/target/qemu_m68k -B build/qemu_m68k
#   cmake --build build/qemu_m68k

cmake_minimum_required(VERSION 3.13)

project(ppap_qemu_m68k C ASM)
set(CMAKE_C_STANDARD 11)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

option(PPAP_TESTS "Enable test suite" OFF)

include(${PPAP_ROOT}/cmake/m68k.cmake)

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_qemu_m68k
    ${ARCH_M68K_SOURCES}
    ${KERNEL_SHARED_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_qemu_m68k.c
)

ppap_m68k_target_common(ppap_qemu_m68k)

target_compile_definitions(ppap_qemu_m68k PRIVATE
    PPAP_QEMU=1
    PPAP_M68K=1
    PAGE_COUNT_MAX=4096u
    USER_PAGES_MAX=512u
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

# Integer math helpers (__mulsi3, __udivsi3, __umodsi3) are provided by
# src/arch/m68k/math.S instead of libgcc.  The toolchain's libgcc is built
# with -msep-data -fPIC (GOT-relative via a5) for user-space; the kernel
# does not set up a5 as GOT base, so it must not link that libgcc.

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_qemu_m68k BIG_ENDIAN
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

# Generate binary and size report
add_custom_command(TARGET ppap_qemu_m68k POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ppap_qemu_m68k.elf ppap_qemu_m68k.bin
    COMMAND ${CMAKE_SIZE} ppap_qemu_m68k.elf
    COMMENT "Generating ppap_qemu_m68k.bin"
)

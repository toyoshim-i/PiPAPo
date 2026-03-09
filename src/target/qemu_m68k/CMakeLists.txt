# CMakeLists.txt — QEMU m68k target (standalone build)
#
# This target builds independently from the ARM/Pico SDK targets.
# Uses the unified main.c boot path with target hooks.
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

# Common build configuration (kernel sources, userland, romfs functions).
include(${PPAP_ROOT}/cmake/ppap-userland.cmake)

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_qemu_m68k
    # Architecture + shared kernel (from kernel_sources.cmake)
    ${ARCH_M68K_SOURCES}
    ${KERNEL_SHARED_SOURCES}

    # Target-specific drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_qemu_m68k.c
)

target_include_directories(ppap_qemu_m68k PRIVATE
    ${PPAP_ROOT}/src/arch/m68k/include
    ${PPAP_ROOT}/src
    ${PPAP_ROOT}/src/kernel
)

option(PPAP_TESTS "Enable test suite" OFF)

target_compile_definitions(ppap_qemu_m68k PRIVATE
    PPAP_QEMU=1
    PPAP_M68K=1
    PAGE_COUNT_MAX=4096u
    # SYSCALL_DEBUG=1
    $<$<BOOL:${PPAP_TESTS}>:PPAP_TESTS=1>
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
# User-space programs + romfs image pipeline
# =============================================================================

ppap_add_musl()
ppap_add_busybox()
ppap_add_rogue()
ppap_build_user_programs(M68K_USER_ELFS)

ppap_add_mkromfs()
ppap_generate_romfs(ppap_qemu_m68k
    ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
    ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/inittab
    BIG_ENDIAN
    USER_ELFS ${M68K_USER_ELFS}
)

# Generate binary and size report
add_custom_command(TARGET ppap_qemu_m68k POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ppap_qemu_m68k.elf ppap_qemu_m68k.bin
    COMMAND ${CMAKE_SIZE} ppap_qemu_m68k.elf
    COMMENT "Generating ppap_qemu_m68k.bin"
)

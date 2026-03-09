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

# Common userland build configuration (compiler flags, build functions).
# Also includes busybox_applets.cmake and user_programs.cmake.
include(${PPAP_ROOT}/cmake/ppap-userland.cmake)

# Shared kernel sources compiled for m68k.
# ARM-only files (xip.c, mpu.c) are excluded.
# smp.c, signal.c, elf.c have m68k guards.

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_qemu_m68k
    # Architecture
    ${PPAP_ROOT}/src/arch/m68k/boot.S
    ${PPAP_ROOT}/src/arch/m68k/m68k_common.c
    ${PPAP_ROOT}/src/arch/m68k/probe_ram.S
    ${PPAP_ROOT}/src/arch/m68k/string.c
    ${PPAP_ROOT}/src/arch/m68k/switch.S
    ${PPAP_ROOT}/src/arch/m68k/trap.S
    ${PPAP_ROOT}/src/arch/m68k/math.S
    ${PPAP_ROOT}/src/arch/m68k/signal_m68k.S

    # Target
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_qemu_m68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_qemu_m68k.c

    # Unified kernel entry point
    ${PPAP_ROOT}/src/kernel/main.c

    # Shared kernel — core subsystems
    ${PPAP_ROOT}/src/kernel/klog.c
    ${PPAP_ROOT}/src/kernel/mm/page.c
    ${PPAP_ROOT}/src/kernel/mm/kmem.c
    ${PPAP_ROOT}/src/kernel/proc/proc.c
    ${PPAP_ROOT}/src/kernel/proc/sched.c
    ${PPAP_ROOT}/src/kernel/smp.c

    # Syscall dispatch + implementations
    ${PPAP_ROOT}/src/kernel/syscall/syscall.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_proc.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_io.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_time.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_mem.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_poll.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_fs.c

    # File descriptors + TTY + pipe
    ${PPAP_ROOT}/src/kernel/fd/fd.c
    ${PPAP_ROOT}/src/kernel/fd/tty.c
    ${PPAP_ROOT}/src/kernel/fd/pipe.c

    # Signal handling
    ${PPAP_ROOT}/src/kernel/signal/signal.c

    # VFS + filesystems
    ${PPAP_ROOT}/src/kernel/vfs/vfs.c
    ${PPAP_ROOT}/src/kernel/vfs/namei.c
    ${PPAP_ROOT}/src/kernel/fs/romfs.c
    ${PPAP_ROOT}/src/kernel/fs/devfs.c
    ${PPAP_ROOT}/src/kernel/fs/procfs.c
    ${PPAP_ROOT}/src/kernel/fs/tmpfs.c
    ${PPAP_ROOT}/src/kernel/fs/fstab.c

    # ELF loader + exec
    ${PPAP_ROOT}/src/kernel/exec/elf.c
    ${PPAP_ROOT}/src/kernel/exec/exec.c
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

# --- Third-party builds (musl, busybox, rogue) ---
ppap_add_musl()
ppap_add_busybox()
ppap_add_rogue()

# --- User-space binaries ---
ppap_build_user_programs(M68K_USER_ELFS)

# --- Build mkromfs host tool ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkromfs
    COMMAND cc -O2 -I ${PPAP_ROOT}/src/kernel/fs
            -o ${CMAKE_BINARY_DIR}/mkromfs
            ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
    DEPENDS ${PPAP_ROOT}/tools/mkromfs/mkromfs.c
            ${PPAP_ROOT}/src/kernel/fs/romfs_format.h
    COMMENT "Building mkromfs host tool"
)

# --- Stage romfs directory and generate big-endian romfs.bin ---
set(ROMFS_STAGING ${CMAKE_BINARY_DIR}/romfs)
set(ROMFS_BIN     ${CMAKE_BINARY_DIR}/romfs_m68k.bin)
set(ROMFS_ASM     ${CMAKE_BINARY_DIR}/romfs_data_m68k.S)

add_custom_command(
    OUTPUT ${ROMFS_BIN}
    COMMAND ${PPAP_ROOT}/cmake/stage-romfs.sh
            ${ROMFS_STAGING} ${PPAP_ROOT}
            --user-elfs ${M68K_USER_ELFS}
            --bb-dir ${PPAP_BB_DIR}
            --bb-applets ${BB_APPLETS}
            --bb-shell-applets ${BB_SHELL_APPLETS}
            --bb-sbin-applets ${BB_SBIN_APPLETS}
            --rogue ${PPAP_ROGUE_DIR}/rogue
            --etc-dir ${PPAP_ROOT}/src/etc
            --fstab ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
            --inittab ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/inittab
    # Build big-endian romfs image
    COMMAND ${CMAKE_BINARY_DIR}/mkromfs -b
            ${ROMFS_STAGING} ${ROMFS_BIN}
    DEPENDS ${CMAKE_BINARY_DIR}/mkromfs
            ${M68K_USER_ELFS}
            ${PPAP_BB_DIR}/busybox
            ${PPAP_ROGUE_DIR}/rogue
            ${PPAP_ROOT}/src/etc/fstab
            ${PPAP_ROOT}/src/etc/inittab
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/inittab
            ${PPAP_ROOT}/cmake/stage-romfs.sh
    COMMENT "Generating big-endian romfs for m68k"
)

# Generate .S file that includes the romfs binary
file(WRITE ${ROMFS_ASM}
    "    .section .romfs, \"a\"\n    .incbin \"${ROMFS_BIN}\"\n")
set_source_files_properties(${ROMFS_ASM}
    PROPERTIES OBJECT_DEPENDS ${ROMFS_BIN})

add_custom_target(romfs_image_m68k DEPENDS ${ROMFS_BIN})
add_dependencies(ppap_qemu_m68k romfs_image_m68k)
target_sources(ppap_qemu_m68k PRIVATE ${ROMFS_ASM})

# Generate binary and size report
add_custom_command(TARGET ppap_qemu_m68k POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ppap_qemu_m68k.elf ppap_qemu_m68k.bin
    COMMAND ${CMAKE_SIZE} ppap_qemu_m68k.elf
    COMMENT "Generating ppap_qemu_m68k.bin"
)

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

# Shared kernel sources compiled for m68k.
# ARM-only files (xip.c, mpu.c) are excluded.
# smp.c, signal.c, elf.c have m68k guards.

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

# =============================================================================
# User-space programs + romfs image pipeline
# =============================================================================

set(M68K_USER_DIR ${PPAP_ROOT}/user/arch/m68k)
set(M68K_USER_LD  ${M68K_USER_DIR}/user.ld)

# --- Build m68k user-space binaries ---
# Common objects: crt0.o + syscall.o (linked into every user program)
set(M68K_CFLAGS  -m68000 -ffreestanding -nostdlib -Os -g -Wall
                 -msep-data
                 -I${PPAP_ROOT}/user/arch/m68k -I${PPAP_ROOT}/user)
set(M68K_ASFLAGS -m68000)
set(M68K_LDFLAGS -nostdlib -T ${M68K_USER_LD}
                 -Wl,--gc-sections -Wl,--build-id=none)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/crt0.o
    COMMAND ${CMAKE_C_COMPILER} ${M68K_ASFLAGS} -c
            -o ${CMAKE_BINARY_DIR}/crt0.o ${M68K_USER_DIR}/crt0.S
    DEPENDS ${M68K_USER_DIR}/crt0.S
    COMMENT "Assembling m68k crt0.o"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/syscall.o
    COMMAND ${CMAKE_C_COMPILER} ${M68K_ASFLAGS} -c
            -o ${CMAKE_BINARY_DIR}/syscall.o ${M68K_USER_DIR}/syscall.S
    DEPENDS ${M68K_USER_DIR}/syscall.S
    COMMENT "Assembling m68k syscall.o"
)

# hello.elf: compiled from shared user/hello.c with m68k crt0+syscall
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/hello.elf
    COMMAND ${CMAKE_C_COMPILER} ${M68K_CFLAGS} -c
            -o ${CMAKE_BINARY_DIR}/hello.o ${PPAP_ROOT}/user/hello.c
    COMMAND ${CMAKE_C_COMPILER} -m68000 ${M68K_LDFLAGS}
            -o ${CMAKE_BINARY_DIR}/hello.elf
            ${CMAKE_BINARY_DIR}/crt0.o
            ${CMAKE_BINARY_DIR}/syscall.o
            ${CMAKE_BINARY_DIR}/hello.o
    DEPENDS ${PPAP_ROOT}/user/hello.c
            ${CMAKE_BINARY_DIR}/crt0.o
            ${CMAKE_BINARY_DIR}/syscall.o
            ${M68K_USER_LD}
            ${PPAP_ROOT}/user/syscall.h
    COMMENT "Building m68k hello.elf (from user/hello.c)"
)

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
    # Create staging directory structure
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ROMFS_STAGING}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS_STAGING}/etc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS_STAGING}/bin
    # Copy fstab from source tree
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
            ${ROMFS_STAGING}/etc/fstab
    # Copy hello.elf to /bin/hello
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/hello.elf
            ${ROMFS_STAGING}/bin/hello
    # Build big-endian romfs image
    COMMAND ${CMAKE_BINARY_DIR}/mkromfs -b
            ${ROMFS_STAGING} ${ROMFS_BIN}
    DEPENDS ${CMAKE_BINARY_DIR}/mkromfs
            ${CMAKE_BINARY_DIR}/hello.elf
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
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

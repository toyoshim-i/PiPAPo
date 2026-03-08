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
    ${PPAP_ROOT}/src/arch/m68k/divmod64.c
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
    PAGE_COUNT=256u
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

set(M68K_COMMON_OBJS ${CMAKE_BINARY_DIR}/crt0.o ${CMAKE_BINARY_DIR}/syscall.o)
set(M68K_COMMON_DEPS ${M68K_COMMON_OBJS} ${M68K_USER_LD} ${PPAP_ROOT}/user/syscall.h)

# Build a user program from shared C source
function(m68k_user_program name source)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/${name}.elf
        COMMAND ${CMAKE_C_COMPILER} ${M68K_CFLAGS} -c
                -o ${CMAKE_BINARY_DIR}/${name}.o ${source}
        COMMAND ${CMAKE_C_COMPILER} -m68000 ${M68K_LDFLAGS}
                -o ${CMAKE_BINARY_DIR}/${name}.elf
                ${M68K_COMMON_OBJS}
                ${CMAKE_BINARY_DIR}/${name}.o
        DEPENDS ${source} ${M68K_COMMON_DEPS}
        COMMENT "Building m68k ${name}.elf"
    )
endfunction()

m68k_user_program(hello  ${PPAP_ROOT}/user/hello.c)
m68k_user_program(init   ${PPAP_ROOT}/user/init.c)
m68k_user_program(getty  ${PPAP_ROOT}/user/getty.c)
m68k_user_program(ttyctl ${PPAP_ROOT}/user/ttyctl.c)

set(M68K_USER_ELFS
    ${CMAKE_BINARY_DIR}/hello.elf
    ${CMAKE_BINARY_DIR}/init.elf
    ${CMAKE_BINARY_DIR}/getty.elf
    ${CMAKE_BINARY_DIR}/ttyctl.elf
)

# --- Busybox (pre-built by build-busybox.sh --m68k) ---
set(M68K_BB_DIR  ${PPAP_ROOT}/build/m68k/busybox)

# --- rogue (built automatically, matching ARM approach) ---
set(M68K_ROGUE ${CMAKE_BINARY_DIR}/rogue/rogue)
file(GLOB ROGUE_PATCHES ${PPAP_ROOT}/third_party/patches/rogue/*)
add_custom_command(
    OUTPUT ${M68K_ROGUE}
    COMMAND ${PPAP_ROOT}/third_party/build-rogue.sh --m68k
    DEPENDS ${PPAP_ROOT}/third_party/build-rogue.sh
            ${ROGUE_PATCHES}
            ${PPAP_ROOT}/third_party/configs/busybox-m68k.ld
    COMMENT "Building rogue (PIC, m68k)"
)

# Busybox applet symlinks (same as ARM install-busybox.sh)
set(BB_APPLETS cat chmod cp df echo grep head kill ln ls mkdir mv
               printf ps rm rmdir sed sleep sort tail top uname vi wc)
set(BB_SHELL_APPLETS sh hush)
set(BB_SBIN_APPLETS mount umount)

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

# Convert CMake lists to space-separated strings for shell script
string(REPLACE ";" " " M68K_USER_ELFS_STR "${M68K_USER_ELFS}")
string(REPLACE ";" " " BB_APPLETS_STR "${BB_APPLETS}")
string(REPLACE ";" " " BB_SHELL_APPLETS_STR "${BB_SHELL_APPLETS}")
string(REPLACE ";" " " BB_SBIN_APPLETS_STR "${BB_SBIN_APPLETS}")

# Build a shell script to assemble the romfs staging directory.
# This handles symlink creation which cmake -E can't do portably.
set(ROMFS_SCRIPT ${CMAKE_BINARY_DIR}/stage_romfs.sh)
file(WRITE ${ROMFS_SCRIPT}
"#!/bin/bash
set -e
STAGING=$1
BB_DIR=$2
ROGUE=$3

# Create directory structure
rm -rf $STAGING
mkdir -p $STAGING/bin $STAGING/sbin $STAGING/etc $STAGING/dev
mkdir -p $STAGING/proc $STAGING/tmp $STAGING/usr/bin

# Install user programs
for elf in ${M68K_USER_ELFS_STR}; do
    name=$(basename $elf .elf)
    case $name in
        init)   cp $elf $STAGING/sbin/$name ;;
        ttyctl) cp $elf $STAGING/usr/bin/$name ;;
        *)      cp $elf $STAGING/bin/$name ;;
    esac
done

# Install busybox (if built)
if [ -f $BB_DIR/busybox ]; then
    cp $BB_DIR/busybox $STAGING/bin/busybox
    cp $BB_DIR/busybox.sh $STAGING/bin/busybox.sh
    # Applet symlinks
    for a in ${BB_APPLETS_STR}; do ln -sf busybox $STAGING/bin/$a; done
    for a in ${BB_SHELL_APPLETS_STR}; do ln -sf busybox.sh $STAGING/bin/$a; done
    for a in ${BB_SBIN_APPLETS_STR}; do ln -sf ../bin/busybox $STAGING/sbin/$a; done
fi

# Install rogue (if built)
if [ -f $ROGUE ]; then
    cp $ROGUE $STAGING/bin/rogue
fi
")

add_custom_command(
    OUTPUT ${ROMFS_BIN}
    # Stage romfs
    COMMAND bash ${ROMFS_SCRIPT}
            ${ROMFS_STAGING} ${M68K_BB_DIR} ${M68K_ROGUE}
    # Copy shared /etc files, then override with target-specific config
    COMMAND cp -a ${PPAP_ROOT}/src/etc/. ${ROMFS_STAGING}/etc/
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
            ${ROMFS_STAGING}/etc/fstab
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/inittab
            ${ROMFS_STAGING}/etc/inittab
    # Build big-endian romfs image
    COMMAND ${CMAKE_BINARY_DIR}/mkromfs -b
            ${ROMFS_STAGING} ${ROMFS_BIN}
    DEPENDS ${CMAKE_BINARY_DIR}/mkromfs
            ${M68K_USER_ELFS}
            ${M68K_ROGUE}
            ${PPAP_ROOT}/src/etc/fstab
            ${PPAP_ROOT}/src/etc/inittab
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/fstab
            ${CMAKE_CURRENT_SOURCE_DIR}/romfs/etc/inittab
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

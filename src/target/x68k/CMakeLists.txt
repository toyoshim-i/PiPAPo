# CMakeLists.txt — X68000 target (standalone build)
#
# Build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_m68k.cmake \
#         -S src/target/x68k -B build/x68k
#   cmake --build build/x68k
#
# Rootfs lives in a single 44bsd UFS on the floppy starting at sector 4.
# Stage2 reads /boot/kernel from it, and the kernel mounts the same UFS
# as the rootfs via iocs_blk ("fd0").  ppap_generate_romfs() stages the
# userland so that mkx68kimg.sh can package it into the UFS.
#
# There is no QEMU/emulator run target for x68k.  Use
#   scripts/run.sh --build x68k  to produce ppap_x68k.xdf.

cmake_minimum_required(VERSION 3.13)

project(ppap_x68k C ASM)
set(CMAKE_C_STANDARD 11)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

include(${PPAP_ROOT}/cmake/m68k.cmake)

set(CMAKE_EXECUTABLE_SUFFIX .elf)

add_executable(ppap_x68k
    ${ARCH_M68K_SOURCES}
    ${KERNEL_SHARED_SOURCES}
    ${KERNEL_BLKDEV_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/target_x68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/iocs_blk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/scsi_blk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/serial_x68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/tvram_x68k.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/x68k_iocs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/x68k_logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/driver/timer_x68k.c
)

# X68000 IOCS ROM calls run on the current process's kernel stack, and the
# ROM's SCC/MFP/FDC interrupt frames nest on it while the IOCS guard holds
# IPL=0; preemption adds the timer ISR frame on top.  The shared m68k 2 KB
# default overruns into the adjacent kstack slot under IOCS-heavy workloads
# (verified by kstack canary panics); 4 KB holds with margin.  Set before
# the common setup applies PROC_KSTACK_SIZE, and keep in sync with the
# .kstack region in x68k.ld = PROC_KSTACK_IDLE_SIZE +
# (PROC_MAX-1) * PROC_KSTACK_SIZE.
set(PPAP_M68K_KSTACK_SIZE 4096u)
ppap_m68k_target_common(ppap_x68k)

target_include_directories(ppap_x68k PRIVATE
    ${PPAP_ROOT}/src/arch/m68k
    ${PPAP_ROOT}/src/target/x68k
)

target_compile_definitions(ppap_x68k PRIVATE
    PPAP_X68K=1
    PPAP_M68K=1
    PPAP_HAS_BLKDEV=1
    PPAP_HAS_PROCFS=1
    # PAGE_COUNT_MAX: supports up to 12 MB expanded X68000 RAM.
    # mm_init() uses m68k_probe_ram() to detect actual physical RAM at runtime.
    PAGE_COUNT_MAX=3072u
    USER_PAGES_MAX=512u
    # RAM_END: X68000 VRAM starts at 0xC00000; stop probing before that.
    # This caps the page pool to main RAM (1–12 MB depending on expansion).
    RAM_END=0xC00000u
)

target_compile_options(ppap_x68k PRIVATE
    -m68000
    -Os -g
    -ffreestanding -fno-builtin
)

target_link_options(ppap_x68k PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/x68k.ld
    -nostartfiles -nostdlib -nodefaultlibs
    -Wl,--gc-sections
    -Wl,--build-id=none
)

# Integer math helpers (__mulsi3, __udivsi3, __umodsi3) are provided by
# src/arch/m68k/math.S — do not link libgcc (same as qemu_m68k).

# =============================================================================
# Romfs staging (Phase X-3)
#
# ppap_generate_romfs() stages the m68k userland to
# build/x68k/romfs_ppap_x68k/ so that mkx68kimg.sh can package it into
# the inner rootfs.ufs.  The generated .romfs section is discarded by the
# linker (x68k.ld: /DISCARD/ *(.romfs)) so the kernel binary stays lean.
# =============================================================================

ppap_generate_romfs(ppap_x68k BIG_ENDIAN NO_ROGUE
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs
    EXCLUDE_APPS hello trace pdb)

# Generate binary and size report
add_custom_command(TARGET ppap_x68k POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ppap_x68k.elf ppap_x68k.bin
    COMMAND ${CMAKE_SIZE} ppap_x68k.elf
    COMMENT "Generating ppap_x68k.bin"
)

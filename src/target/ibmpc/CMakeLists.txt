# =============================================================================
# IBM PC target (i16, 8086 real mode) -- Phase P-4b
# =============================================================================
#
# Five-part build:
#   1. stage1 — 512 B boot sector
#   2. stage2 — UFS loader (up to 4 KB), loads core + VFS modules
#   3. core   — core PPAP kernel module (scheduler, mm, syscall, blkdev)
#   4. vfs    — VFS module (filesystems, fd, tty, pipe)
#   5. hello  — test user program (.COM flat binary)
#
# Each kernel module is a separate small-model binary in its own 64 KB
# segment (CS=DS). Cross-module calls use far-call stubs with DS switching.
# Medium model is broken in ia16-elf-ld 2.39.
#
# Toolchain: ia16-elf-gcc (16-bit x86 cross-compiler)
# Output: ppap_ibmpc.img (1.44 MB bootable floppy)
# =============================================================================

cmake_minimum_required(VERSION 3.13)

# -- Locate ia16-elf-gcc ---------------------------------------------------

set(PPAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

if(EXISTS "/opt/ppap/ia16-elf/bin/ia16-elf-gcc")
  set(IA16_TC "/opt/ppap/ia16-elf")
elseif(EXISTS "${PPAP_ROOT}/tools/ia16-build/prefix/bin/ia16-elf-gcc")
  set(IA16_TC "${PPAP_ROOT}/tools/ia16-build/prefix")
elseif(EXISTS "${PPAP_ROOT}/tools/ia16-toolchain/usr/bin/ia16-elf-gcc")
  set(IA16_TC "${PPAP_ROOT}/tools/ia16-toolchain/usr")
else()
  set(IA16_TC "")
endif()

if(IA16_TC)
  set(CMAKE_C_COMPILER "${IA16_TC}/bin/ia16-elf-gcc")
  set(CMAKE_ASM_COMPILER "${IA16_TC}/bin/ia16-elf-gcc")
  set(CMAKE_OBJCOPY "${IA16_TC}/bin/ia16-elf-objcopy")
else()
  set(CMAKE_C_COMPILER "ia16-elf-gcc")
  set(CMAKE_ASM_COMPILER "ia16-elf-gcc")
  set(CMAKE_OBJCOPY "ia16-elf-objcopy")
endif()

set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i16)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(ppap_ibmpc C ASM)

set(COMMON_FLAGS
  -march=i8086
  -mtune=i8086
  -ffreestanding
  -nostdlib
  -Os
  -Wall -Wextra -Werror
  -Wno-unused-parameter
  -Wno-unused-function
  -Wno-pointer-to-int-cast
  -Wno-int-to-pointer-cast
)

# Each module is small model (CS=DS within one 64 KB segment).
# Medium model is broken in ia16-elf-ld 2.39 (R_386_16 overflow).
# Modules are separate binaries with isolated segments instead.
set(KERNEL_FLAGS ${COMMON_FLAGS})

set(PPAP_INCLUDES
  ${PPAP_ROOT}/src
  ${PPAP_ROOT}/src/kernel
  ${PPAP_ROOT}/src/target/ibmpc
)

# ==========================================================================
# Target 1: stage1 (boot sector, 512 bytes)
# ==========================================================================

add_executable(stage1
  ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage1.S
)
target_compile_options(stage1 PRIVATE -march=i8086 -ffreestanding)
target_link_options(stage1 PRIVATE
  -nostartfiles -nostdlib
  -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage1.ld
)
set_target_properties(stage1 PROPERTIES SUFFIX ".elf")

add_custom_command(TARGET stage1 POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:stage1>
    ${CMAKE_CURRENT_BINARY_DIR}/stage1.bin
  COMMENT "Creating stage1.bin (512 B boot sector)"
)

# ==========================================================================
# Target 2: stage2 (UFS loader, up to 4 KB)
# ==========================================================================

add_executable(stage2
  ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage2_entry.S
  ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage2.c
)
target_compile_options(stage2 PRIVATE ${COMMON_FLAGS})
target_link_options(stage2 PRIVATE
  -nostartfiles
  -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage2.ld
)
target_link_libraries(stage2 PRIVATE gcc)
set_target_properties(stage2 PROPERTIES SUFFIX ".elf")

add_custom_command(TARGET stage2 POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:stage2>
    ${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
  COMMENT "Creating stage2.bin"
)

# ==========================================================================
# Target 3: core module (scheduler, memory, syscall, blkdev)
# ==========================================================================

set(ARCH_I16_SOURCES
  ${PPAP_ROOT}/src/arch/i16/boot.S
  ${PPAP_ROOT}/src/arch/i16/switch.S
  ${PPAP_ROOT}/src/arch/i16/trap.S
  ${PPAP_ROOT}/src/arch/i16/i16_common.c
  ${PPAP_ROOT}/src/arch/i16/smp.c
  ${PPAP_ROOT}/src/arch/i16/string.c
)

set(KERNEL_CORE_SOURCES
  ${PPAP_ROOT}/src/kernel/main.c
  ${PPAP_ROOT}/src/kernel/klog.c
  ${PPAP_ROOT}/src/kernel/common/mod/mod_core.c
  ${PPAP_ROOT}/src/kernel/common/seg.c
  ${PPAP_ROOT}/src/kernel/mm/page.c
  ${PPAP_ROOT}/src/kernel/mm/kmem.c
  ${PPAP_ROOT}/src/kernel/mm/mem_region.c
  ${PPAP_ROOT}/src/kernel/proc/proc.c
  ${PPAP_ROOT}/src/kernel/proc/sched.c
  ${PPAP_ROOT}/src/kernel/signal/signal.c
  ${PPAP_ROOT}/src/kernel/cpu/cpu.c
  ${PPAP_ROOT}/src/kernel/cpu/cpu_native.c
  ${PPAP_ROOT}/src/kernel/blkdev/blkdev.c
  ${PPAP_ROOT}/src/kernel/exec/exec.c
  ${PPAP_ROOT}/src/kernel/exec/elf16_loader.c
  ${PPAP_ROOT}/src/kernel/exec/flat_loader.c
  ${PPAP_ROOT}/src/kernel/exec/loader.c
  ${PPAP_ROOT}/src/kernel/subsys/subsys.c
  ${PPAP_ROOT}/src/kernel/syscall/syscall.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_proc.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_io.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_time.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_mem.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_poll.c
  ${PPAP_ROOT}/src/kernel/syscall/sys_fs.c
  ${PPAP_ROOT}/src/target/target_default.c
)

set(TARGET_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/target_ibmpc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_com.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/floppy_blk.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bios_con.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_pit.c
  # Core-side stubs: caller stubs for VFS, entry stubs for core
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/vfs_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/core_entries.S
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/vfs_mod_init.c
)

add_executable(ppap_ibmpc
  ${ARCH_I16_SOURCES}
  ${KERNEL_CORE_SOURCES}
  ${TARGET_SOURCES}
)

target_include_directories(ppap_ibmpc PRIVATE ${PPAP_INCLUDES})
# 640 KB conventional RAM: pages from __page_pool_start to 0x9FC00.
# free_stack[] uses page_id_t (16-bit index), not void*, so pages
# beyond 64 KB are accessible.  ~150 pages typical (depends on
# kernel size and VFS data reservation).
target_compile_definitions(ppap_ibmpc PRIVATE PAGE_COUNT_MAX=150u)
target_compile_options(ppap_ibmpc PRIVATE ${KERNEL_FLAGS})

target_link_options(ppap_ibmpc PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/ibmpc_kernel.ld
  -nostartfiles
  -Wl,--gc-sections
  # TODO: replace with stub objects once far-call stubs are implemented
  -Wl,--unresolved-symbols=ignore-all
)
target_link_libraries(ppap_ibmpc PRIVATE gcc)

set_target_properties(ppap_ibmpc PROPERTIES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ibmpc_kernel.ld"
)

add_custom_command(TARGET ppap_ibmpc POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:ppap_ibmpc>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc.bin
  COMMENT "Creating core module binary ppap_ibmpc.bin"
)

# ==========================================================================
# Target 4: VFS module (separate segment)
# ==========================================================================

set(KERNEL_VFS_SOURCES
  ${PPAP_ROOT}/src/kernel/vfs/vfs.c
  ${PPAP_ROOT}/src/kernel/vfs/namei.c
  ${PPAP_ROOT}/src/kernel/fd/fd.c
  ${PPAP_ROOT}/src/kernel/fd/tty.c
  ${PPAP_ROOT}/src/kernel/fd/pipe.c
  ${PPAP_ROOT}/src/kernel/fs/romfs.c
  ${PPAP_ROOT}/src/kernel/fs/tmpfs.c
  ${PPAP_ROOT}/src/kernel/fs/devfs.c
  ${PPAP_ROOT}/src/kernel/fs/procfs.c
  ${PPAP_ROOT}/src/kernel/fs/fstab.c
  ${PPAP_ROOT}/src/kernel/fs/ufs.c
  ${PPAP_ROOT}/src/arch/i16/string.c
)

add_executable(ppap_ibmpc_vfs
  # Header must be first — contains entry point table at offset 0
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/vfs_header.S
  ${KERNEL_VFS_SOURCES}
  # VFS-side stubs: entry stubs for VFS, caller stubs for core
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/vfs_entries.S
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/core_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/core_mod_init.c
)

target_include_directories(ppap_ibmpc_vfs PRIVATE ${PPAP_INCLUDES})
target_compile_options(ppap_ibmpc_vfs PRIVATE ${KERNEL_FLAGS})

target_link_options(ppap_ibmpc_vfs PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/ibmpc_vfs.ld
  -nostartfiles
  # TODO: replace with stub objects once far-call stubs are implemented
  -Wl,--unresolved-symbols=ignore-all
)
target_link_libraries(ppap_ibmpc_vfs PRIVATE gcc)

set_target_properties(ppap_ibmpc_vfs PROPERTIES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ibmpc_vfs.ld"
)

# Extract VFS code (.text + .rodata) and data (.data) as separate binaries.
# Code goes to VFS CS segment, data goes to DS:0xA000.
add_custom_command(TARGET ppap_ibmpc_vfs POST_BUILD
  COMMAND ${CMAKE_OBJCOPY}
    -j .text -j .text.header
    -O binary
    $<TARGET_FILE:ppap_ibmpc_vfs>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc_vfs.bin
  COMMAND ${CMAKE_OBJCOPY}
    -j .rodata -j .data
    -O binary
    $<TARGET_FILE:ppap_ibmpc_vfs>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc_vfs_data.bin
  COMMENT "Creating VFS code + data binaries"
)

add_dependencies(ppap_ibmpc ppap_ibmpc_vfs)

# ==========================================================================
# Target 5: hello.com (test user program for exec)
# ==========================================================================

add_executable(hello_com
  ${CMAKE_CURRENT_SOURCE_DIR}/user/hello.S
)
target_compile_options(hello_com PRIVATE -march=i8086 -ffreestanding)
target_link_options(hello_com PRIVATE
  -nostartfiles -nostdlib
  -T ${CMAKE_CURRENT_SOURCE_DIR}/user/flat.ld
)
set_target_properties(hello_com PROPERTIES
  SUFFIX ".elf"
  OUTPUT_NAME "hello"
)

add_custom_command(TARGET hello_com POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:hello_com>
    ${CMAKE_CURRENT_BINARY_DIR}/hello.com
  COMMENT "Creating hello.com (test user program)"
)

add_dependencies(ppap_ibmpc hello_com)

# ==========================================================================
# Post-build: assemble floppy image via mkpcimg.sh
# ==========================================================================

add_custom_command(TARGET ppap_ibmpc POST_BUILD
  COMMAND ${PPAP_ROOT}/scripts/mkpcimg.sh
  DEPENDS stage1 stage2
  COMMENT "Assembling floppy image ppap_ibmpc.img"
)

add_dependencies(ppap_ibmpc stage1 stage2)

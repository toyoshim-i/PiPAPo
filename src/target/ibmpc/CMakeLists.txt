# =============================================================================
# IBM PC target (i16, 8086 real mode) -- Phase P-3b
# =============================================================================
#
# Three-part build:
#   1. stage1 — 512 B boot sector
#   2. stage2 — UFS loader (up to 4 KB)
#   3. kernel — core PPAP kernel (page alloc, proc, sched, klog)
#
# Does NOT include: ELF loader, eCPU emulators, subsystem bridges,
# block devices. Those will be separate modules (see kernel_modules.md).
#
# Toolchain: ia16-elf-gcc (16-bit x86 cross-compiler)
# Output: ppap_ibmpc.img (1.44 MB bootable floppy)
# =============================================================================

cmake_minimum_required(VERSION 3.13)

# -- Locate ia16-elf-gcc ---------------------------------------------------

set(PPAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

if(EXISTS "/opt/ppap/ia16-elf/bin/ia16-elf-gcc")
  set(IA16_TC "/opt/ppap/ia16-elf")
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
# Target 3: kernel (core PPAP kernel)
# ==========================================================================

set(ARCH_I16_SOURCES
  ${PPAP_ROOT}/src/arch/i16/boot.S
  ${PPAP_ROOT}/src/arch/i16/switch.S
  ${PPAP_ROOT}/src/arch/i16/i16_common.c
  ${PPAP_ROOT}/src/arch/i16/smp.c
  ${PPAP_ROOT}/src/arch/i16/string.c
)

# Core kernel sources — minimal set for boot-to-idle
set(KERNEL_CORE_SOURCES
  ${PPAP_ROOT}/src/kernel/main.c
  ${PPAP_ROOT}/src/kernel/klog.c
  ${PPAP_ROOT}/src/kernel/mm/page.c
  ${PPAP_ROOT}/src/kernel/mm/kmem.c
  ${PPAP_ROOT}/src/kernel/proc/proc.c
  ${PPAP_ROOT}/src/kernel/proc/sched.c
  ${PPAP_ROOT}/src/kernel/signal/signal.c
  ${PPAP_ROOT}/src/kernel/cpu/cpu.c
  ${PPAP_ROOT}/src/kernel/cpu/cpu_native.c
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
  ${PPAP_ROOT}/src/kernel/exec/exec.c
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
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bios_con.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_pit.c
)

add_executable(ppap_ibmpc
  ${ARCH_I16_SOURCES}
  ${KERNEL_CORE_SOURCES}
  ${TARGET_SOURCES}
)

target_include_directories(ppap_ibmpc PRIVATE
  ${PPAP_ROOT}/src
  ${PPAP_ROOT}/src/kernel
  ${PPAP_ROOT}/src/target/ibmpc
)

target_compile_options(ppap_ibmpc PRIVATE ${COMMON_FLAGS})

target_link_options(ppap_ibmpc PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/ibmpc_kernel.ld
  -nostartfiles
  -Wl,--gc-sections
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
  COMMENT "Creating kernel binary ppap_ibmpc.bin"
)

# ==========================================================================
# Post-build: assemble floppy image via mkpcimg.sh
# ==========================================================================

add_custom_command(TARGET ppap_ibmpc POST_BUILD
  COMMAND ${PPAP_ROOT}/scripts/mkpcimg.sh
  DEPENDS stage1 stage2
  COMMENT "Assembling floppy image ppap_ibmpc.img"
)

add_dependencies(ppap_ibmpc stage1 stage2)

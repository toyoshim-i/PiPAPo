# =============================================================================
# PC/XT target (i16, 8086 real mode) -- Phase P-4b
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
# Output: ppap_pcxt.img (1.44 MB bootable floppy)
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
  set(CMAKE_NM "${IA16_TC}/bin/ia16-elf-nm")
  set(CMAKE_SIZE_UTIL "${IA16_TC}/bin/ia16-elf-size")
else()
  set(CMAKE_C_COMPILER "ia16-elf-gcc")
  set(CMAKE_ASM_COMPILER "ia16-elf-gcc")
  set(CMAKE_OBJCOPY "ia16-elf-objcopy")
  set(CMAKE_NM "ia16-elf-nm")
  set(CMAKE_SIZE_UTIL "ia16-elf-size")
endif()

set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i16)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(ppap_pcxt C ASM)

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

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
  ${PPAP_ROOT}/src/arch/i16
  ${PPAP_ROOT}/src/target/pcxt
)

set(PCXT_USER_DIR ${PPAP_ROOT}/src/user)
set(PCXT_USER_ARCH_DIR ${PPAP_ROOT}/src/arch/i16/user)
set(PCXT_TEST_DIR ${PPAP_ROOT}/tests/user)
set(PCXT_USER_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/user)
file(MAKE_DIRECTORY ${PCXT_USER_BUILD_DIR})

execute_process(
  COMMAND ${CMAKE_C_COMPILER} -march=i8086 -print-libgcc-file-name
  OUTPUT_VARIABLE IA16_LIBGCC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(PCXT_USER_CFLAGS
  -march=i8086
  -mtune=i8086
  -ffreestanding
  -nostdlib
  -Os
  -Wall -Wextra -Werror
  -Wno-unused-parameter
  -I${PPAP_ROOT}/src
  -I${PCXT_USER_DIR}
)

set(PCXT_USER_ASFLAGS
  -march=i8086
  -mtune=i8086
)

set(PCXT_USER_LDFLAGS
  -march=i8086
  -mtune=i8086
  -nostdlib
  -T ${PCXT_USER_ARCH_DIR}/user.ld
  -Wl,--gc-sections
  -Wl,--build-id=none
)

add_custom_command(
  OUTPUT ${PCXT_USER_BUILD_DIR}/crt0.o
  COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_ASFLAGS} -c
          -o ${PCXT_USER_BUILD_DIR}/crt0.o ${PCXT_USER_ARCH_DIR}/crt0.S
  DEPENDS ${PCXT_USER_ARCH_DIR}/crt0.S
  COMMENT "Assembling ia16 crt0.o"
)

add_custom_command(
  OUTPUT ${PCXT_USER_BUILD_DIR}/syscall.o
  COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_ASFLAGS} -c
          -o ${PCXT_USER_BUILD_DIR}/syscall.o ${PCXT_USER_ARCH_DIR}/syscall.S
  DEPENDS ${PCXT_USER_ARCH_DIR}/syscall.S
  COMMENT "Assembling ia16 syscall.o"
)

add_custom_command(
  OUTPUT ${PCXT_USER_BUILD_DIR}/runtime.o
  COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_CFLAGS} -c
          -o ${PCXT_USER_BUILD_DIR}/runtime.o ${PCXT_USER_ARCH_DIR}/runtime.c
  DEPENDS ${PCXT_USER_ARCH_DIR}/runtime.c
  COMMENT "Compiling ia16 runtime.o"
)

add_custom_command(
  OUTPUT ${PCXT_USER_BUILD_DIR}/uclib.o
  COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_CFLAGS}
    -ffunction-sections -fdata-sections -c
    -o ${PCXT_USER_BUILD_DIR}/uclib.o ${PPAP_ROOT}/src/user/lib/uclib.c
  DEPENDS ${PPAP_ROOT}/src/user/lib/uclib.c
    ${PPAP_ROOT}/src/user/lib/uclib.h
  COMMENT "Compiling ia16 uclib.o"
)

set(PCXT_CRT_OBJS
  ${PCXT_USER_BUILD_DIR}/crt0.o
  ${PCXT_USER_BUILD_DIR}/syscall.o
  ${PCXT_USER_BUILD_DIR}/runtime.o
  ${PCXT_USER_BUILD_DIR}/uclib.o
)

function(pcxt_user_program name source)
  cmake_parse_arguments(ARG "" "OUTPUT_NAME" "EXTRA_CFLAGS;EXTRA_SOURCES" ${ARGN})

  if(ARG_OUTPUT_NAME)
    set(_out_name ${ARG_OUTPUT_NAME})
  else()
    set(_out_name ${name})
  endif()

  get_filename_component(_src_base ${source} NAME_WE)
  get_filename_component(_src_ext ${source} LAST_EXT)
  set(_obj ${PCXT_USER_BUILD_DIR}/${name}_${_src_base}.o)
  set(_elf ${PCXT_USER_BUILD_DIR}/${_out_name}.elf)
  set(_objs ${_obj})

  if(_src_ext STREQUAL ".S" OR _src_ext STREQUAL ".s")
    add_custom_command(
      OUTPUT ${_obj}
      COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_ASFLAGS} -c -o ${_obj} ${source}
      DEPENDS ${source}
      COMMENT "Assembling ia16 ${name}.o"
    )
  else()
    add_custom_command(
      OUTPUT ${_obj}
      COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_CFLAGS} ${ARG_EXTRA_CFLAGS}
              -c -o ${_obj} ${source}
      DEPENDS ${source} ${PPAP_ROOT}/src/user/syscall.h
      COMMENT "Compiling ia16 ${name}.o"
    )
  endif()

  foreach(_extra_source ${ARG_EXTRA_SOURCES})
    get_filename_component(_extra_base ${_extra_source} NAME_WE)
    get_filename_component(_extra_ext ${_extra_source} LAST_EXT)
    set(_extra_obj ${PCXT_USER_BUILD_DIR}/${name}_${_extra_base}.o)
    list(APPEND _objs ${_extra_obj})

    if(_extra_ext STREQUAL ".S" OR _extra_ext STREQUAL ".s")
      add_custom_command(
        OUTPUT ${_extra_obj}
        COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_ASFLAGS} -c -o ${_extra_obj}
                ${_extra_source}
        DEPENDS ${_extra_source}
        COMMENT "Assembling ia16 ${name}_${_extra_base}.o"
      )
    else()
      add_custom_command(
        OUTPUT ${_extra_obj}
        COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_CFLAGS} ${ARG_EXTRA_CFLAGS}
                -c -o ${_extra_obj} ${_extra_source}
        DEPENDS ${_extra_source} ${PPAP_ROOT}/src/user/syscall.h
        COMMENT "Compiling ia16 ${name}_${_extra_base}.o"
      )
    endif()
  endforeach()

  add_custom_command(
    OUTPUT ${_elf}
    COMMAND ${CMAKE_C_COMPILER} ${PCXT_USER_LDFLAGS}
            -o ${_elf} ${PCXT_CRT_OBJS} ${_objs} ${IA16_LIBGCC}
    DEPENDS ${PCXT_CRT_OBJS} ${_objs} ${PCXT_USER_ARCH_DIR}/user.ld ${IA16_LIBGCC}
    COMMENT "Linking ia16 ${_out_name}.elf"
  )

  set_property(GLOBAL APPEND PROPERTY PCXT_USER_ELFS ${_elf})
endfunction()

# ==========================================================================
# Target 1: stage1 (boot sector, 512 bytes)
# ==========================================================================

add_executable(stage1
  ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage1.S
)
target_compile_options(stage1 PRIVATE -march=i8086 -ffreestanding
  -Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/boot)
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

# Target 1b: stage1_hdd (MBR boot sector, 512 bytes)
# ==========================================================================

add_executable(stage1_hdd
  ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage1_hdd.S
)
target_compile_options(stage1_hdd PRIVATE -march=i8086 -ffreestanding
  -Wa,-I${CMAKE_CURRENT_SOURCE_DIR}/boot)
target_link_options(stage1_hdd PRIVATE
  -nostartfiles -nostdlib
  -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/stage1.ld
)
set_target_properties(stage1_hdd PROPERTIES SUFFIX ".elf")

add_custom_command(TARGET stage1_hdd POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:stage1_hdd>
    ${CMAKE_CURRENT_BINARY_DIR}/stage1_hdd.bin
  COMMENT "Creating stage1_hdd.bin (512 B MBR boot sector)"
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
  ${PPAP_ROOT}/src/arch/i16/boot/boot.S
  ${PPAP_ROOT}/src/arch/i16/kernel/core/switch.S
  ${PPAP_ROOT}/src/arch/i16/kernel/core/trap.S
  ${PPAP_ROOT}/src/arch/i16/kernel/core/i16_common.c
  ${PPAP_ROOT}/src/arch/i16/kernel/core/smp.c
  ${PPAP_ROOT}/src/arch/i16/kernel/core/string.c
  ${PPAP_ROOT}/src/arch/i16/kernel/core/seg.c
)

set(KERNEL_CORE_SOURCES
  ${PPAP_ROOT}/src/kernel/core/main.c
  ${PPAP_ROOT}/src/kernel/core/core.c
  ${PPAP_ROOT}/src/kernel/core/mm/page.c
  ${PPAP_ROOT}/src/kernel/core/mm/kmem.c
  ${PPAP_ROOT}/src/kernel/core/mm/mem_region.c
  ${PPAP_ROOT}/src/kernel/core/proc/proc.c
  ${PPAP_ROOT}/src/kernel/core/proc/sched.c
  ${PPAP_ROOT}/src/kernel/core/signal/signal.c
  ${PPAP_ROOT}/src/kernel/core/cpu/cpu.c
  ${PPAP_ROOT}/src/kernel/core/cpu/cpu_native.c
  ${PPAP_ROOT}/src/kernel/core/exec/exec.c
  ${PPAP_ROOT}/src/kernel/core/exec/elf16_loader.c
  ${PPAP_ROOT}/src/kernel/core/exec/flat_loader.c
  ${PPAP_ROOT}/src/kernel/core/exec/loader.c
  ${PPAP_ROOT}/src/kernel/core/subsys/subsys.c
  ${PPAP_ROOT}/src/kernel/core/syscall/syscall.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_proc.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_io.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_time.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_mem.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_poll.c
  ${PPAP_ROOT}/src/kernel/core/syscall/sys_fs.c
  ${PPAP_ROOT}/src/target/target_default.c
)

set(TARGET_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/target_pcxt.c
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/driver/timer_pit.c
  # Core-side stubs: caller stubs for VFS, entry stubs for core
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/core_entries.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_mod_init.c
)

# .S stub files use the C preprocessor to expand the function index list
# from kernel/common/mod/mod_*.inc.  CMake's automatic dependency scanner
# does not track #include for .S files (no .S.d depfiles are generated),
# so changes to the .inc would not trigger a rebuild on incremental
# builds — we'd silently link OLD index slots against NEW C-side struct
# layouts.  Declare the deps explicitly here.  See docs/notes/pcxt_crash.md.
set_property(SOURCE
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_entries.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_header.S
  APPEND PROPERTY OBJECT_DEPENDS
  ${PPAP_ROOT}/src/kernel/common/mod/mod_vfs.inc)
set_property(SOURCE
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/core_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/core_entries.S
  APPEND PROPERTY OBJECT_DEPENDS
  ${PPAP_ROOT}/src/kernel/common/mod/mod_core.inc)

add_executable(ppap_pcxt
  ${ARCH_I16_SOURCES}
  ${KERNEL_CORE_SOURCES}
  ${TARGET_SOURCES}
)

target_include_directories(ppap_pcxt PRIVATE ${PPAP_INCLUDES})
# 640 KB conventional RAM: pages from __page_pool_start to 0x9FC00.
# free_stack[] uses page_id_t (16-bit index), not void*, so pages
# beyond 64 KB are accessible.  ~150 pages typical (depends on
# kernel size and VFS data reservation).
target_compile_definitions(ppap_pcxt PRIVATE PAGE_COUNT_MAX=150u PPAP_HAS_BLKDEV=1 PPAP_TICK_HZ=18u)
if(PPAP_TESTS)
  target_compile_definitions(ppap_pcxt PRIVATE PPAP_TESTS=1)
endif()
if(PPAP_TESTS_EXTENDED)
  target_compile_definitions(ppap_pcxt PRIVATE PPAP_TESTS_EXTENDED=1)
endif()
target_compile_options(ppap_pcxt PRIVATE ${KERNEL_FLAGS})

target_link_options(ppap_pcxt PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/pcxt_kernel.ld
  -nostartfiles
  -Wl,--gc-sections
)
target_link_libraries(ppap_pcxt PRIVATE gcc)

set_target_properties(ppap_pcxt PROPERTIES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/pcxt_kernel.ld"
)

add_custom_command(TARGET ppap_pcxt POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:ppap_pcxt>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt.bin
  COMMENT "Creating core module binary"
)

# ==========================================================================
# Target 4: VFS module (separate segment)
# ==========================================================================

set(KERNEL_VFS_SOURCES
  ${PPAP_ROOT}/src/kernel/vfs/klog.c
  ${PPAP_ROOT}/src/kernel/vfs/vfs.c
  ${PPAP_ROOT}/src/kernel/vfs/namei.c
  ${PPAP_ROOT}/src/kernel/vfs/fd.c
  ${PPAP_ROOT}/src/kernel/vfs/tty.c
  ${PPAP_ROOT}/src/kernel/vfs/pipe.c
  ${PPAP_ROOT}/src/kernel/vfs/romfs.c
  ${PPAP_ROOT}/src/kernel/vfs/tmpfs.c
  ${PPAP_ROOT}/src/kernel/vfs/devfs.c
  ${PPAP_ROOT}/src/kernel/vfs/procfs.c
  ${PPAP_ROOT}/src/kernel/vfs/fstab.c
  ${PPAP_ROOT}/src/kernel/vfs/ufs.c
  ${PPAP_ROOT}/src/arch/i16/kernel/core/string.c
)

add_executable(ppap_pcxt_vfs
  # Header must be first — contains entry point table at offset 0
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_header.S
  ${KERNEL_VFS_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/uart_com.c
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/bios_con.c
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/pcxt_logger.c
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/pcxt_vfs_init.c
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/bios_blk.c
  ${PPAP_ROOT}/src/kernel/vfs/driver/blkdev.c
  # VFS-side stubs: entry stubs for VFS, caller stubs for core
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/vfs_entries.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/common/stubs/core_stubs.S
  ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/core.c
)

target_include_directories(ppap_pcxt_vfs PRIVATE ${PPAP_INCLUDES})
target_compile_definitions(ppap_pcxt_vfs PRIVATE PPAP_HAS_UFS=1 PPAP_TICK_HZ=18u)
target_compile_options(ppap_pcxt_vfs PRIVATE ${KERNEL_FLAGS})

# Two-pass link: VFS needs core_exports.ld from core ELF.
# Use a custom target to break the circular dependency:
#   ppap_pcxt → (POST_BUILD) → core_exports_gen → ppap_pcxt_vfs
# CMake doesn't allow cycles between executables, so core_exports_gen
# is an intermediate custom target.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld"
    "/* auto-generated — will be replaced after core links */\n")
endif()
add_custom_target(core_exports_gen
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/gen_core_exports.sh
    ${CMAKE_NM} ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt.elf
    ${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld
  DEPENDS ppap_pcxt
  COMMENT "Generating core_exports.ld from core ELF"
)
add_dependencies(ppap_pcxt_vfs core_exports_gen)

target_link_options(ppap_pcxt_vfs PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/pcxt_vfs.ld
  -T ${CMAKE_CURRENT_BINARY_DIR}/core_exports.ld
  -nostartfiles
)
target_link_libraries(ppap_pcxt_vfs PRIVATE gcc)

set_target_properties(ppap_pcxt_vfs PROPERTIES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/pcxt_vfs.ld"
)

# Extract VFS code (.text + .rodata) and data (.data) as separate binaries.
# Code goes to VFS CS segment, data goes to DS:0xAC00.
add_custom_command(TARGET ppap_pcxt_vfs POST_BUILD
  COMMAND ${CMAKE_OBJCOPY}
    -j .text -j .text.header
    -O binary
    $<TARGET_FILE:ppap_pcxt_vfs>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt_vfs.bin
  COMMAND ${CMAKE_OBJCOPY}
    -j .rodata -j .data
    -O binary
    $<TARGET_FILE:ppap_pcxt_vfs>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt_vfs_data.bin
  COMMENT "Creating VFS code + data binaries"
)

add_custom_target(pcxt_memory_report ALL
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/report_memory_usage.sh
    ${CMAKE_SIZE_UTIL}
    ${CMAKE_NM}
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt.elf
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_pcxt_vfs.elf
  DEPENDS ppap_pcxt ppap_pcxt_vfs
  COMMENT "Reporting PC/XT core and VFS memory usage"
)

# ==========================================================================
# Target 5: ia16 user ELFs (first-party src/user only)
# ==========================================================================

set(PCXT_USER_APPS hello getty init pdb push cat ls ps df top)

foreach(_app ${PCXT_USER_APPS})
  if(_app STREQUAL "push")
    pcxt_user_program(push ${PPAP_ROOT}/src/user/push.c
      EXTRA_SOURCES
        ${PPAP_ROOT}/src/user/push_line.c)
  elseif(_app STREQUAL "pdb")
    pcxt_user_program(pdb ${PPAP_ROOT}/src/user/pdb/pdb.c
      EXTRA_SOURCES
        ${PPAP_ROOT}/src/user/pdb/pdb_util.c
        ${PPAP_ROOT}/src/user/pdb/pdb_trace_util.c
        ${PPAP_ROOT}/src/user/pdb/pdb_cmd.c
        ${PPAP_ROOT}/src/user/pdb/pdb_regs.c
        ${PPAP_ROOT}/src/user/pdb/pdb_target.c
        ${PPAP_ROOT}/src/user/pdb/pdb_inspect.c
        ${PPAP_ROOT}/src/user/pdb/pdb_break.c)
  else()
    pcxt_user_program(${_app} ${PPAP_ROOT}/src/user/${_app}.c)
  endif()
endforeach()

if(PPAP_TESTS)
  pcxt_user_program(runtests ${PPAP_ROOT}/tests/user/runtests.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_exec ${PPAP_ROOT}/tests/user/test_exec.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_vfork ${PPAP_ROOT}/tests/user/test_vfork.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_fs ${PPAP_ROOT}/tests/user/test_fs.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_rw ${PPAP_ROOT}/tests/user/test_rw.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_tmpfs ${PPAP_ROOT}/tests/user/test_tmpfs.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
  pcxt_user_program(test_ufs ${PPAP_ROOT}/tests/user/test_ufs.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
endif()

if(PPAP_TESTS_EXTENDED)
  pcxt_user_program(runtests_ext ${PPAP_ROOT}/tests/user/runtests_ext.c
    EXTRA_CFLAGS -I${PCXT_TEST_DIR})
endif()

get_property(PCXT_USER_ELFS GLOBAL PROPERTY PCXT_USER_ELFS)
add_custom_target(pcxt_user_elfs DEPENDS ${PCXT_USER_ELFS})

# ==========================================================================
# Post-build: assemble floppy image via mkpcimg.sh
# ==========================================================================

# Floppy image depends on both core and VFS binaries + bootloader.
# This breaks what was previously a circular dependency:
#   old: ppap_pcxt -(POST_BUILD)-> needs VFS binary -(dep)-> ppap_pcxt
#   new: ppap_pcxt_img depends on both independently
add_custom_target(ppap_pcxt_img ALL
  COMMAND ${PPAP_ROOT}/scripts/mkpcimg.sh ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ppap_pcxt ppap_pcxt_vfs stage1 stage1_hdd stage2 pcxt_user_elfs
  COMMENT "Assembling floppy and HDD images"
)
add_dependencies(ppap_pcxt_img ppap_pcxt ppap_pcxt_vfs stage1 stage1_hdd stage2 pcxt_user_elfs)

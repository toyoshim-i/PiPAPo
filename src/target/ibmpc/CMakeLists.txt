# =============================================================================
# IBM PC target (i16, 8086 real mode) -- Phase P-3a
# =============================================================================
#
# Three-part build:
#   1. stage1 — 512 B boot sector (stage1.S)
#   2. stage2 — UFS loader (stage2_entry.S + stage2.c), up to 4 KB
#   3. kernel — P-2 standalone scheduler (boot.S + switch.S + target code)
#
# Post-build: mkpcimg.sh assembles the floppy image.
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
# Target 3: kernel (P-2 standalone scheduler)
# ==========================================================================

set(KERNEL_SOURCES
  ${PPAP_ROOT}/src/arch/i16/boot.S
  ${PPAP_ROOT}/src/arch/i16/switch.S
  ${PPAP_ROOT}/src/arch/i16/i16_common.c
  ${PPAP_ROOT}/src/arch/i16/smp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/target_ibmpc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_com.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bios_con.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/timer_pit.c
)

add_executable(ppap_ibmpc ${KERNEL_SOURCES})

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

# Ensure stage1 and stage2 are built before the kernel post-build step
add_dependencies(ppap_ibmpc stage1 stage2)

# =============================================================================
# IBM PC target (i16, 8086 real mode) -- Phase P-1
# =============================================================================
#
# Minimal build: boot.S → kmain() → print banner → halt.
# Does NOT link the full kernel yet (P-2+).
#
# Toolchain: ia16-elf-gcc (16-bit x86 cross-compiler)
# Output: flat binary ppap_ibmpc.img suitable for QEMU -kernel or floppy boot
# =============================================================================

cmake_minimum_required(VERSION 3.13)

# -- Locate ia16-elf-gcc ---------------------------------------------------

set(PPAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# Check Docker path first, then local tools path, then system PATH
if(EXISTS "/opt/ppap/ia16-elf/bin/ia16-elf-gcc")
  set(IA16_TC "/opt/ppap/ia16-elf")
elseif(EXISTS "${PPAP_ROOT}/tools/ia16-toolchain/usr/bin/ia16-elf-gcc")
  set(IA16_TC "${PPAP_ROOT}/tools/ia16-toolchain/usr")
else()
  # Assume ia16-elf-gcc is on PATH
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

# -- Compiler flags ---------------------------------------------------------

set(COMMON_FLAGS
  -march=i8086
  -mtune=i8086
  -ffreestanding
  -nostdlib
  -Os
  -Wall -Wextra -Werror
  -Wno-unused-parameter
)

# -- Sources ----------------------------------------------------------------

set(ARCH_I16_SOURCES
  ${PPAP_ROOT}/src/arch/i16/boot.S
  ${PPAP_ROOT}/src/arch/i16/i16_common.c
  ${PPAP_ROOT}/src/arch/i16/smp.c
)

set(TARGET_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/target_ibmpc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_com.c
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bios_con.c
)

# -- Build target -----------------------------------------------------------

add_executable(ppap_ibmpc
  ${ARCH_I16_SOURCES}
  ${TARGET_SOURCES}
)

target_include_directories(ppap_ibmpc PRIVATE
  ${PPAP_ROOT}/src
  ${PPAP_ROOT}/src/kernel
  ${PPAP_ROOT}/src/target/ibmpc
)

target_compile_options(ppap_ibmpc PRIVATE ${COMMON_FLAGS})

target_link_options(ppap_ibmpc PRIVATE
  -T ${CMAKE_CURRENT_SOURCE_DIR}/ibmpc.ld
  -nostartfiles
  -Wl,--gc-sections
)

set_target_properties(ppap_ibmpc PROPERTIES
  SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ibmpc.ld"
)

# -- Flat binary output (for QEMU -kernel or floppy boot) ------------------

add_custom_command(TARGET ppap_ibmpc POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary
    $<TARGET_FILE:ppap_ibmpc>
    ${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc.bin
  # Pad to 1.44 MB floppy image with binary at sector 0
  COMMAND dd if=/dev/zero of=${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc.img
    bs=512 count=2880 2>/dev/null
  COMMAND dd if=${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc.bin
    of=${CMAKE_CURRENT_BINARY_DIR}/ppap_ibmpc.img
    conv=notrunc 2>/dev/null
  COMMENT "Creating 1.44 MB floppy image ppap_ibmpc.img"
)

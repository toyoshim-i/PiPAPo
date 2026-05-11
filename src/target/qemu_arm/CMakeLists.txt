# CMakeLists.txt — QEMU ARM target (standalone build)
#
# Build:
#   cmake -S src/target/qemu_arm -B build/qemu_arm -DPPAP_TESTS=ON
#   cmake --build build/qemu_arm

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

project(ppap_qemu_arm C CXX ASM)
set(CMAKE_C_STANDARD 11)

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)
option(PPAP_SEMIHOST "Use ARM semihosting for serial I/O" OFF)

include(${PPAP_ROOT}/cmake/arm_m.cmake)

set(CMAKE_EXECUTABLE_SUFFIX .elf)

# Select UART driver: semihosting (bkpt 0xAB) or CMSDK UART hardware
if(PPAP_SEMIHOST)
    set(UART_DRIVER ${PPAP_ROOT}/src/arch/arm_m/kernel/vfs/driver/semihost.c)
else()
    set(UART_DRIVER ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/uart_qemu.c)
endif()

add_executable(ppap_qemu_arm
    ${KERNEL_COMMON_SOURCES}
    ${KERNEL_BLKDEV_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/target_qemu_arm.c
    ${UART_DRIVER}
    ${PPAP_ROOT}/src/kernel/vfs/driver/uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/vfs/driver/qemu_arm_logger.c
    ${PPAP_ROOT}/src/kernel/vfs/driver/ramblk.c
)

# Target dir is added before ppap_arm_target_common so the overlay
# search order ends up target -> arch -> primary src/.
target_include_directories(ppap_qemu_arm PRIVATE
    ${PPAP_ROOT}/src/target/qemu_arm
)

ppap_arm_target_common(ppap_qemu_arm)

target_compile_definitions(ppap_qemu_arm PRIVATE
    PPAP_QEMU=1
    PPAP_HAS_BLKDEV=1
    PPAP_HAS_PROCFS=1
)

target_link_options(ppap_qemu_arm PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel/core/qemu.ld
    -nostartfiles
    -Wl,--gc-sections
)

# =============================================================================
# Romfs
# =============================================================================

ppap_generate_romfs(ppap_qemu_arm
    OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/romfs)

# =============================================================================
# FAT32 test image pipeline (QEMU only)
# =============================================================================

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkfatimg
    COMMAND cc -O2 -o ${CMAKE_BINARY_DIR}/mkfatimg
            ${PPAP_ROOT}/tools/mkfatimg/mkfatimg.c
    DEPENDS ${PPAP_ROOT}/tools/mkfatimg/mkfatimg.c
    COMMENT "Building mkfatimg host tool"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkufs
    COMMAND cc -O2 -o ${CMAKE_BINARY_DIR}/mkufs
            ${PPAP_ROOT}/tools/mkufs/mkufs.c
    DEPENDS ${PPAP_ROOT}/tools/mkufs/mkufs.c
    COMMENT "Building mkufs host tool"
)

file(GLOB UFS_TESTDATA ${PPAP_ROOT}/tools/ufs_testdata/*)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ufstest.img
    COMMAND ${CMAKE_BINARY_DIR}/mkufs -s 64K
            -p ${PPAP_ROOT}/tools/ufs_testdata
            ${CMAKE_BINARY_DIR}/ufstest.img
    DEPENDS ${CMAKE_BINARY_DIR}/mkufs ${UFS_TESTDATA}
    COMMENT "Generating UFS test image"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fatimg.bin
    COMMAND ${CMAKE_BINARY_DIR}/mkfatimg ${CMAKE_BINARY_DIR}/fatimg.bin
            ${CMAKE_BINARY_DIR}/ufstest.img
    DEPENDS ${CMAKE_BINARY_DIR}/mkfatimg ${CMAKE_BINARY_DIR}/ufstest.img
    COMMENT "Generating FAT32 test image (with UFS)"
)

file(WRITE ${CMAKE_BINARY_DIR}/fatimg_data.S
"    .section .fatimg, \"a\", %progbits\n    .incbin \"${CMAKE_BINARY_DIR}/fatimg.bin\"\n")

add_custom_target(fatimg_image DEPENDS ${CMAKE_BINARY_DIR}/fatimg.bin)
add_dependencies(ppap_qemu_arm fatimg_image)

set_source_files_properties(${CMAKE_BINARY_DIR}/fatimg_data.S
    PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/fatimg.bin)
target_sources(ppap_qemu_arm PRIVATE ${CMAKE_BINARY_DIR}/fatimg_data.S)

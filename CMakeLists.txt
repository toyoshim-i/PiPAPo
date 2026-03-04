cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(ppap C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Build option: enable kernel integration tests + userland test suite
option(PPAP_TESTS "Enable kernel integration tests and userland test suite" OFF)

# Shared kernel source list (used by each target's CMakeLists.txt)
include(cmake/kernel_sources.cmake)

# =============================================================================
# Per-target build definitions
# =============================================================================
add_subdirectory(src/target/qemu_arm)
add_subdirectory(src/target/pico1)
add_subdirectory(src/target/pico1calc)

set(PPAP_ALL_TARGETS ppap_qemu_arm ppap_pico1 ppap_pico1calc)

# =============================================================================
# Test support (conditional on PPAP_TESTS)
# =============================================================================
if(PPAP_TESTS)
    foreach(tgt ${PPAP_ALL_TARGETS})
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/test/ktest.c)
        target_compile_definitions(${tgt} PRIVATE PPAP_TESTS=1)
    endforeach()
endif()

# =============================================================================
# User-space build + romfs image pipeline
# =============================================================================
# 1. Build musl libc + busybox
# 2. Build user-space ELF binaries (user/Makefile)
# 3. Install into romfs/
# 4. Build mkromfs, generate romfs.bin
# 5. Link romfs_data.S (.incbin romfs.bin) into all targets
# =============================================================================

# --- musl libc (Phase 6 Step 1) ---
set(MUSL_SYSROOT ${CMAKE_BINARY_DIR}/musl-sysroot)
set(MUSL_LIBC ${MUSL_SYSROOT}/lib/libc.a)

file(GLOB_RECURSE MUSL_OVERLAY ${CMAKE_SOURCE_DIR}/third_party/patches/musl/overlay/*)

add_custom_command(
    OUTPUT ${MUSL_LIBC}
    COMMAND ${CMAKE_SOURCE_DIR}/third_party/build-musl.sh
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/build-musl.sh
            ${MUSL_OVERLAY}
    COMMENT "Building musl libc (PIC, Cortex-M0+)"
)

# --- busybox (Phase 6 Step 4) ---
set(BUSYBOX_BIN ${CMAKE_BINARY_DIR}/busybox/busybox)

add_custom_command(
    OUTPUT ${BUSYBOX_BIN}
    COMMAND ${CMAKE_SOURCE_DIR}/third_party/build-busybox.sh
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/build-busybox.sh
            ${CMAKE_SOURCE_DIR}/third_party/configs/busybox_ppap.fragment
            ${CMAKE_SOURCE_DIR}/third_party/configs/busybox.ld
            ${MUSL_LIBC}
    COMMENT "Building busybox (PIC, Cortex-M0+)"
)

# --- busybox install (Phase 6 Step 6) ---
set(BUSYBOX_STAMP ${CMAKE_BINARY_DIR}/busybox_install.stamp)

add_custom_command(
    OUTPUT ${BUSYBOX_STAMP}
    COMMAND ${CMAKE_SOURCE_DIR}/third_party/install-busybox.sh
    COMMAND ${CMAKE_COMMAND} -E touch ${BUSYBOX_STAMP}
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/install-busybox.sh
            ${BUSYBOX_BIN}
    COMMENT "Installing busybox + applet symlinks into romfs/"
)

add_custom_target(busybox_install DEPENDS ${BUSYBOX_STAMP})

# --- User-space binaries (Phase 3 Step 1) ---
set(USER_SRC_DIR ${CMAKE_SOURCE_DIR}/user)
set(USER_STAMP ${CMAKE_BINARY_DIR}/user_binaries.stamp)

file(GLOB USER_SOURCES ${USER_SRC_DIR}/*.c ${USER_SRC_DIR}/*.S
                       ${USER_SRC_DIR}/*.h ${USER_SRC_DIR}/*.ld
                       ${USER_SRC_DIR}/Makefile)

find_program(MAKE_CMD make REQUIRED)

add_custom_command(
    OUTPUT ${USER_STAMP}
    COMMAND ${MAKE_CMD} -C ${USER_SRC_DIR} clean
    COMMAND ${MAKE_CMD} -C ${USER_SRC_DIR} install
            ROMFS_BIN=${CMAKE_SOURCE_DIR}/romfs/bin
    COMMAND ${CMAKE_COMMAND} -E touch ${USER_STAMP}
    DEPENDS ${USER_SOURCES}
    COMMENT "Building user-space binaries and installing to romfs/bin/"
)

add_custom_target(user_binaries DEPENDS ${USER_STAMP})

# --- mkromfs host tool ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkromfs
    COMMAND cc -O2 -I ${CMAKE_SOURCE_DIR}/src/kernel/fs
            -o ${CMAKE_BINARY_DIR}/mkromfs
            ${CMAKE_SOURCE_DIR}/tools/mkromfs/mkromfs.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/mkromfs/mkromfs.c
            ${CMAKE_SOURCE_DIR}/src/kernel/fs/romfs_format.h
    COMMENT "Building mkromfs host tool"
)

# --- Generate romfs.bin from romfs/ directory ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/romfs.bin
    COMMAND ${CMAKE_BINARY_DIR}/mkromfs
            ${CMAKE_SOURCE_DIR}/romfs
            ${CMAKE_BINARY_DIR}/romfs.bin
    DEPENDS ${CMAKE_BINARY_DIR}/mkromfs ${USER_STAMP} ${BUSYBOX_STAMP}
    COMMENT "Generating romfs image from romfs/"
)

# --- romfs_data.S: .incbin the romfs image into a .romfs section ---
file(WRITE ${CMAKE_BINARY_DIR}/romfs_data.S
"    .section .romfs, \"a\", %progbits\n    .incbin \"${CMAKE_BINARY_DIR}/romfs.bin\"\n")

add_custom_target(romfs_image DEPENDS ${CMAKE_BINARY_DIR}/romfs.bin)

set_source_files_properties(${CMAKE_BINARY_DIR}/romfs_data.S
    PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/romfs.bin)

# --- Link romfs into all targets ---
foreach(tgt ${PPAP_ALL_TARGETS})
    add_dependencies(${tgt} romfs_image)
    target_sources(${tgt} PRIVATE ${CMAKE_BINARY_DIR}/romfs_data.S)
endforeach()

# =============================================================================
# FAT32 test image pipeline (QEMU only)
# =============================================================================

# --- mkfatimg host tool ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkfatimg
    COMMAND cc -O2 -o ${CMAKE_BINARY_DIR}/mkfatimg
            ${CMAKE_SOURCE_DIR}/tools/mkfatimg/mkfatimg.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/mkfatimg/mkfatimg.c
    COMMENT "Building mkfatimg host tool"
)

# --- mkufs host tool ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkufs
    COMMAND cc -O2 -o ${CMAKE_BINARY_DIR}/mkufs
            ${CMAKE_SOURCE_DIR}/tools/mkufs/mkufs.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/mkufs/mkufs.c
    COMMENT "Building mkufs host tool"
)

# --- Generate UFS test image (populated from tools/ufs_testdata/) ---
file(GLOB UFS_TESTDATA ${CMAKE_SOURCE_DIR}/tools/ufs_testdata/*)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ufstest.img
    COMMAND ${CMAKE_BINARY_DIR}/mkufs -s 64K
            -p ${CMAKE_SOURCE_DIR}/tools/ufs_testdata
            ${CMAKE_BINARY_DIR}/ufstest.img
    DEPENDS ${CMAKE_BINARY_DIR}/mkufs ${UFS_TESTDATA}
    COMMENT "Generating UFS test image"
)

# --- Generate fatimg.bin (with embedded UFS image) ---
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/fatimg.bin
    COMMAND ${CMAKE_BINARY_DIR}/mkfatimg ${CMAKE_BINARY_DIR}/fatimg.bin
            ${CMAKE_BINARY_DIR}/ufstest.img
    DEPENDS ${CMAKE_BINARY_DIR}/mkfatimg ${CMAKE_BINARY_DIR}/ufstest.img
    COMMENT "Generating FAT32 test image (with UFS)"
)

# --- fatimg_data.S: .incbin the FAT32 image into a .fatimg section ---
file(WRITE ${CMAKE_BINARY_DIR}/fatimg_data.S
"    .section .fatimg, \"a\", %progbits\n    .incbin \"${CMAKE_BINARY_DIR}/fatimg.bin\"\n")

add_custom_target(fatimg_image DEPENDS ${CMAKE_BINARY_DIR}/fatimg.bin)
add_dependencies(ppap_qemu_arm fatimg_image)

set_source_files_properties(${CMAKE_BINARY_DIR}/fatimg_data.S
    PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/fatimg.bin)

# Only link FAT32 image into ppap_qemu_arm (hardware uses real SD card)
target_sources(ppap_qemu_arm PRIVATE ${CMAKE_BINARY_DIR}/fatimg_data.S)


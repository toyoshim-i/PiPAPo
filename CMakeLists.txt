cmake_minimum_required(VERSION 3.13)

# Default to the in-tree Pico SDK submodule; allow env override
if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
endif()

# Pull in Pico SDK (must be before project())
include(cmake/pico_sdk_import.cmake)

project(ppap C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Build option: enable kernel integration tests + userland test suite
option(PPAP_TESTS "Enable kernel integration tests and userland test suite" OFF)

# Shared kernel source list (used by each target's CMakeLists.txt)
include(cmake/kernel_sources.cmake)

# Common userland build configuration (compiler flags, build functions)
# Also includes busybox_applets.cmake and user_programs.cmake.
include(cmake/ppap-userland.cmake)

# =============================================================================
# Per-target build definitions
# =============================================================================
add_subdirectory(src/target/qemu_arm)
add_subdirectory(src/target/pico1)
add_subdirectory(src/target/pico1calc)

set(PPAP_ALL_TARGETS ppap_qemu_arm ppap_pico1 ppap_pico1calc)

# Place ELF outputs in the build root (scripts expect build/arm_m/ppap_*.elf)
foreach(tgt ${PPAP_ALL_TARGETS})
    set_target_properties(${tgt} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

# =============================================================================
# Test support (conditional on PPAP_TESTS)
# =============================================================================
if(PPAP_TESTS)
    foreach(tgt ${PPAP_ALL_TARGETS})
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/tests/kernel/ktest.c)
        target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/tests/kernel)
        target_compile_definitions(${tgt} PRIVATE PPAP_TESTS=1)
    endforeach()
endif()

# =============================================================================
# User-space build + romfs image pipeline
# =============================================================================
# 1. Build musl libc, busybox, rogue (via ppap-userland.cmake functions)
# 2. Build user-space ELF binaries (via ppap-userland.cmake functions)
# 3. Stage into per-target romfs directories
# 4. Build mkromfs, generate romfs.bin
# 5. Link romfs_data.S (.incbin romfs.bin) into all targets
# =============================================================================

# --- Third-party builds (musl, busybox, rogue) ---
ppap_add_musl()
ppap_add_busybox()
ppap_add_rogue()

# --- User-space binaries ---
ppap_build_user_programs(ARM_USER_ELF_PATHS)

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

# --- Per-target romfs generation ---
# Each target gets its own romfs.bin with an appropriate /etc/fstab.
# Targets with PPAP_HAS_BLKDEV get the mmcblk0 vfat mount entry appended.
set(PPAP_BLKDEV_TARGETS ppap_qemu_arm ppap_pico1calc)

# Generate per-target fstab files at configure time
set(FSTAB_BLKDEV_LINE "mmcblk0          /mnt/sd     vfat    rw\n")
foreach(tgt ${PPAP_ALL_TARGETS})
    file(READ ${CMAKE_SOURCE_DIR}/src/etc/fstab _fstab_base)
    list(FIND PPAP_BLKDEV_TARGETS ${tgt} _blkdev_idx)
    if(_blkdev_idx GREATER_EQUAL 0)
        set(_fstab_content "${_fstab_base}${FSTAB_BLKDEV_LINE}")
    else()
        set(_fstab_content "${_fstab_base}")
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/fstab_${tgt} "${_fstab_content}")
endforeach()

# Generate per-target inittab files at configure time
# PicoCalc uses our tiny /bin/getty on both ttyS0 (UART) and tty1 (LCD+kbd);
# other targets keep the default single-TTY respawn.
# /bin/getty: setsid, open TTY, TIOCSCTTY, "Press Enter" prompt, exec /bin/sh.
set(INITTAB_SERIAL_ONLY "ttyS0::respawn:/bin/getty ttyS0\n")
set(INITTAB_DUAL_TTY "ttyS0::respawn:/bin/getty ttyS0\ntty1::respawn:/bin/getty tty1\n")
foreach(tgt ${PPAP_ALL_TARGETS})
    if(tgt STREQUAL "ppap_pico1calc")
        file(WRITE ${CMAKE_BINARY_DIR}/inittab_${tgt} "${INITTAB_DUAL_TTY}")
    else()
        file(WRITE ${CMAKE_BINARY_DIR}/inittab_${tgt} "${INITTAB_SERIAL_ONLY}")
    endif()
endforeach()

foreach(tgt ${PPAP_ALL_TARGETS})
    set(ROMFS_STAGING ${CMAKE_BINARY_DIR}/romfs_${tgt})
    set(ROMFS_BIN     ${CMAKE_BINARY_DIR}/romfs_${tgt}.bin)
    set(ROMFS_ASM     ${CMAKE_BINARY_DIR}/romfs_data_${tgt}.S)

    add_custom_command(
        OUTPUT ${ROMFS_BIN}
        COMMAND ${CMAKE_SOURCE_DIR}/cmake/stage-romfs.sh
                ${ROMFS_STAGING} ${CMAKE_SOURCE_DIR}
                --user-elfs ${ARM_USER_ELF_PATHS}
                --bb-dir ${PPAP_BB_DIR}
                --bb-applets ${BB_APPLETS}
                --bb-shell-applets ${BB_SHELL_APPLETS}
                --bb-sbin-applets ${BB_SBIN_APPLETS}
                --rogue ${PPAP_ROGUE_DIR}/rogue
                --etc-dir ${CMAKE_SOURCE_DIR}/src/etc
                --fstab ${CMAKE_BINARY_DIR}/fstab_${tgt}
                --inittab ${CMAKE_BINARY_DIR}/inittab_${tgt}
        COMMAND ${CMAKE_BINARY_DIR}/mkromfs
                ${ROMFS_STAGING} ${ROMFS_BIN}
        DEPENDS ${CMAKE_BINARY_DIR}/mkromfs
                ${ARM_USER_ELF_PATHS}
                ${PPAP_BB_DIR}/busybox
                ${PPAP_ROGUE_DIR}/rogue
                ${CMAKE_SOURCE_DIR}/src/etc/fstab
                ${CMAKE_SOURCE_DIR}/src/etc/inittab
                ${CMAKE_BINARY_DIR}/fstab_${tgt}
                ${CMAKE_BINARY_DIR}/inittab_${tgt}
                ${CMAKE_SOURCE_DIR}/cmake/stage-romfs.sh
        COMMENT "Generating romfs for ${tgt}"
    )

    file(WRITE ${ROMFS_ASM}
        "    .section .romfs, \"a\", %progbits\n    .incbin \"${ROMFS_BIN}\"\n")

    set_source_files_properties(${ROMFS_ASM}
        PROPERTIES OBJECT_DEPENDS ${ROMFS_BIN})

    add_custom_target(romfs_image_${tgt} DEPENDS ${ROMFS_BIN})
    add_dependencies(${tgt} romfs_image_${tgt})
    target_sources(${tgt} PRIVATE ${ROMFS_ASM})
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

# =============================================================================
# Font generation pipeline (PicoCalc LCD console)
# =============================================================================
find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/font8x16.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
            --input ${CMAKE_SOURCE_DIR}/third_party/fonts/8x16.bdf
            --output ${CMAKE_BINARY_DIR}/generated/font8x16.c
            --name font8x16 --width 8 --height 16
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/8x16.bdf
            ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
    COMMENT "Generating font8x16.c from 8x16.bdf"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/font4x8.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
            --input ${CMAKE_SOURCE_DIR}/third_party/fonts/4x6.bdf
            --output ${CMAKE_BINARY_DIR}/generated/font4x8.c
            --name font4x8 --width 4 --height 8
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/4x6.bdf
            ${CMAKE_SOURCE_DIR}/tools/bdf2c.py
    COMMENT "Generating font4x8.c from 4x6.bdf"
)

add_custom_target(generate_fonts
    DEPENDS ${CMAKE_BINARY_DIR}/generated/font8x16.c
            ${CMAKE_BINARY_DIR}/generated/font4x8.c
)

add_dependencies(ppap_pico1calc generate_fonts)

set_source_files_properties(
    ${CMAKE_BINARY_DIR}/generated/font8x16.c
    ${CMAKE_BINARY_DIR}/generated/font4x8.c
    TARGET_DIRECTORY ppap_pico1calc
    PROPERTIES GENERATED TRUE
)

target_sources(ppap_pico1calc PRIVATE
    ${CMAKE_BINARY_DIR}/generated/font8x16.c
    ${CMAKE_BINARY_DIR}/generated/font4x8.c
)

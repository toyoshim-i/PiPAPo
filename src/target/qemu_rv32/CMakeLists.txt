# CMakeLists.txt — QEMU RISC-V virt machine target
#
# Build:
#   cmake -S src/target/qemu_rv32 -B build/qemu_rv32
#   cmake --build build/qemu_rv32

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# RISC-V bare-metal toolchain (same as pico2rv kernel)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# Find the toolchain
set(RISCV_TC "${PPAP_ROOT}/tools/riscv-toolchain")
set(CMAKE_C_COMPILER "${RISCV_TC}/bin/riscv32-unknown-elf-gcc")
set(CMAKE_ASM_COMPILER "${RISCV_TC}/bin/riscv32-unknown-elf-gcc")
set(CMAKE_OBJCOPY "${RISCV_TC}/bin/riscv32-unknown-elf-objcopy")
set(CMAKE_SIZE_UTIL "${RISCV_TC}/bin/riscv32-unknown-elf-size")

# Must set these before project() to avoid CMake's compiler test
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(ppap_qemu_rv32 C ASM)
set(CMAKE_C_STANDARD 11)

option(PPAP_TESTS "Enable test suite" OFF)
option(PPAP_TESTS_EXTENDED "Use extended test init runner" OFF)

# RISC-V flags (rv32imac, no float).
# Explicitly disable Zcb (compressed byte/halfword load/store) — the Pico SDK
# toolchain enables it by default (Hazard3 supports it), but QEMU 8.x doesn't.
set(PPAP_RV32_MARCH "rv32imac_zicsr_zifencei")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=${PPAP_RV32_MARCH} -mabi=ilp32 -mcmodel=medany -mno-strict-align")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -march=${PPAP_RV32_MARCH} -mabi=ilp32")

# Shared build directory for userland artifacts
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/riscv")
# Pretend we're hazard3 so user.cmake detects RISC-V
set(CMAKE_SYSTEM_PROCESSOR hazard3)

include(${PPAP_ROOT}/cmake/riscv.cmake)

add_executable(ppap_qemu_rv32
    ${KERNEL_RISCV_COMMON_SOURCES}
    ${KERNEL_BLKDEV_SOURCES}
    ${PPAP_ROOT}/src/kernel/blkdev/flatblk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_rv32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_ns16550.c
    ${CMAKE_CURRENT_SOURCE_DIR}/string.c
)

# Common includes, warnings, definitions
target_include_directories(ppap_qemu_rv32 PRIVATE
    ${PPAP_ROOT}/src
    ${PPAP_ROOT}/src/kernel
)
target_compile_options(ppap_qemu_rv32 PRIVATE
    -Wall -Wextra -Werror -Wno-unused-parameter)
target_compile_definitions(ppap_qemu_rv32 PRIVATE
    PPAP_KERNEL=1
    PPAP_QEMU=1
    PPAP_HAS_BLKDEV=1
    PPAP_SYS_HZ=10000000u
    PAGE_POOL_BASE=0x80809000u
    PAGE_COUNT_MAX=107u
)

# Subsystem/eCPU definitions
if(PPAP_ENABLE_HUMAN68K)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_ENABLE_HUMAN68K=1)
endif()
if(PPAP_ENABLE_CPM)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_ENABLE_CPM=1)
endif()
if(PPAP_ENABLE_SOS)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_ENABLE_SOS=1)
endif()
if(PPAP_ENABLE_ECPU_M68K)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_ENABLE_ECPU_M68K=1)
endif()
if(PPAP_ENABLE_ECPU_Z80)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_ENABLE_ECPU_Z80=1)
endif()

# Test support
if(PPAP_TESTS)
    target_sources(ppap_qemu_rv32 PRIVATE ${PPAP_ROOT}/tests/kernel/ktest.c)
    target_include_directories(ppap_qemu_rv32 PRIVATE ${PPAP_ROOT}/tests/kernel)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_TESTS=1)
endif()
if(PPAP_TESTS_EXTENDED)
    target_compile_definitions(ppap_qemu_rv32 PRIVATE PPAP_TESTS_EXTENDED=1)
endif()

target_link_options(ppap_qemu_rv32 PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/qemu_rv32.ld
    -nostartfiles
    -Wl,--gc-sections
)

# Output with .elf extension (build.sh expects it)
set_target_properties(ppap_qemu_rv32 PROPERTIES SUFFIX ".elf")

# Romfs (exclude musl-linked tests — they go into UFS image below)
ppap_generate_romfs(ppap_qemu_rv32 EXCLUDE_APPS test_musl_child)

# ── UFS test image (musl-linked binaries too large for romfs) ──────────────
# Build mkufs host tool
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/mkufs
    COMMAND cc -O2 -o ${CMAKE_BINARY_DIR}/mkufs
            ${PPAP_ROOT}/tools/mkufs/mkufs.c
    DEPENDS ${PPAP_ROOT}/tools/mkufs/mkufs.c
    COMMENT "Building mkufs host tool"
)

# Stage musl-linked test binaries into a directory for mkufs
set(_ufs_staging ${CMAKE_BINARY_DIR}/ufs_musl_staging)
set(_ufs_img ${CMAKE_BINARY_DIR}/ufs_musl.img)

# Collect musl test ELFs (built by user.cmake)
set(_musl_elf_deps "")
foreach(tst ${USER_MUSL_TESTS})
    list(APPEND _musl_elf_deps ${PPAP_SHARED_BUILD}/${tst}.elf)
endforeach()

add_custom_command(
    OUTPUT ${_ufs_img}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${_ufs_staging}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_ufs_staging}/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${_musl_elf_deps} ${_ufs_staging}/bin/
    # Rename .elf → plain name, strip unneeded symbols (keep relocations)
    COMMAND ${CMAKE_COMMAND} -E rename
            ${_ufs_staging}/bin/test_musl_child.elf
            ${_ufs_staging}/bin/test_musl_child
    COMMAND ${CMAKE_STRIP} --strip-unneeded ${_ufs_staging}/bin/test_musl_child
    COMMAND ${CMAKE_BINARY_DIR}/mkufs -s 2M
            -p ${_ufs_staging} ${_ufs_img}
    DEPENDS ${CMAKE_BINARY_DIR}/mkufs ${_musl_elf_deps}
    COMMENT "Generating UFS test image (musl binaries)"
)

# Embed UFS image as a linker section
file(WRITE ${CMAKE_BINARY_DIR}/ufsimg_data.S
"    .section .ufsimg, \"a\", %progbits\n    .incbin \"${_ufs_img}\"\n")

add_custom_target(ufsimg_image DEPENDS ${_ufs_img})
add_dependencies(ppap_qemu_rv32 ufsimg_image)

set_source_files_properties(${CMAKE_BINARY_DIR}/ufsimg_data.S
    PROPERTIES OBJECT_DEPENDS ${_ufs_img})
target_sources(ppap_qemu_rv32 PRIVATE ${CMAKE_BINARY_DIR}/ufsimg_data.S)

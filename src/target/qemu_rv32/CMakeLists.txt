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

# RISC-V flags (rv32imac, no float, no relaxation for kernel)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv32imac_zicsr -mabi=ilp32 -mcmodel=medany")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -march=rv32imac_zicsr -mabi=ilp32")

# Shared build directory for userland artifacts
set(PPAP_SHARED_BUILD "${PPAP_ROOT}/build/riscv")
# Pretend we're hazard3 so user.cmake detects RISC-V
set(CMAKE_SYSTEM_PROCESSOR hazard3)

include(${PPAP_ROOT}/cmake/riscv.cmake)

add_executable(ppap_qemu_rv32
    ${KERNEL_RISCV_COMMON_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/target_qemu_rv32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/uart_ns16550.c
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
    PPAP_SYS_HZ=10000000u
    PAGE_POOL_BASE=0x80804000u
    PAGE_COUNT_MAX=112u
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

# Romfs
ppap_generate_romfs(ppap_qemu_rv32)

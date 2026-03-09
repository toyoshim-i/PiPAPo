# toolchain-m68k.cmake — Cross-compilation toolchain for Motorola 68000
#
# Usage:
#   mkdir -p build/m68k && cd build/m68k
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-m68k.cmake ..
#
# Uses a custom m68k-elf-gcc built by third_party/build-gcc-m68k.sh.
# Targets the 68000 ISA (-m68000) with a 68000-safe libgcc.
# No standard C library — bare-metal with -nostdlib.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

# Custom m68k-elf toolchain (built by third_party/build-gcc-m68k.sh)
set(PPAP_M68K_TOOLCHAIN ${CMAKE_CURRENT_LIST_DIR}/../tools/m68k-toolchain)

# Cross-compiler
set(CMAKE_C_COMPILER   ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-gcc)
set(CMAKE_ASM_COMPILER ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-gcc)
set(CMAKE_OBJCOPY      ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-objcopy)
set(CMAKE_OBJDUMP      ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-objdump)
set(CMAKE_SIZE         ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-size)
set(CMAKE_AR           ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-ar)
set(CMAKE_RANLIB       ${PPAP_M68K_TOOLCHAIN}/bin/m68k-elf-ranlib)

# Discover GCC's built-in include path (stdint.h, stddef.h, etc.)
# Needed because -nostdinc strips all default search paths.
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE M68K_GCC_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Compiler flags
set(CMAKE_C_FLAGS_INIT   "-m68000 -mno-strict-align -ffreestanding -fno-builtin -nostdinc -isystem ${M68K_GCC_INCLUDE}")
set(CMAKE_ASM_FLAGS_INIT "-m68000")

# Disable compiler tests (no standard runtime available)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

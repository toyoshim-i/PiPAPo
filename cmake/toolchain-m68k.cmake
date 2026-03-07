# toolchain-m68k.cmake — Cross-compilation toolchain for Motorola 68000
#
# Usage:
#   mkdir build-m68k && cd build-m68k
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-m68k.cmake ..
#
# Uses m68k-linux-gnu-gcc from Debian/Ubuntu (apt install gcc-m68k-linux-gnu).
# Targets the 68000 ISA (-m68000) for X68000 hardware compatibility.
# No standard C library — bare-metal with -nostdlib.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)

# Cross-compiler
set(CMAKE_C_COMPILER   m68k-linux-gnu-gcc)
set(CMAKE_ASM_COMPILER m68k-linux-gnu-gcc)
set(CMAKE_OBJCOPY      m68k-linux-gnu-objcopy)
set(CMAKE_OBJDUMP      m68k-linux-gnu-objdump)
set(CMAKE_SIZE         m68k-linux-gnu-size)
set(CMAKE_AR           m68k-linux-gnu-ar)
set(CMAKE_RANLIB       m68k-linux-gnu-ranlib)

# Compiler flags
set(CMAKE_C_FLAGS_INIT   "-m68000 -mno-strict-align -ffreestanding -fno-builtin -nostdinc -isystem /usr/lib/gcc-cross/m68k-linux-gnu/13/include")
set(CMAKE_ASM_FLAGS_INIT "-m68000")

# Disable compiler tests (no standard runtime available)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

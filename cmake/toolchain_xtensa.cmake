# toolchain_xtensa.cmake — Xtensa LX7 (ESP32-S3) cross-compilation toolchain
#
# Used when building outside ESP-IDF's build system (e.g., standalone CMake).
# When building via idf.py, ESP-IDF sets up the toolchain automatically.
#
# All code uses Call0 ABI (-mabi=call0): no register windowing.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

set(CMAKE_C_COMPILER xtensa-esp-elf-gcc)
set(CMAKE_CXX_COMPILER xtensa-esp-elf-g++)
set(CMAKE_ASM_COMPILER xtensa-esp-elf-gcc)
set(CMAKE_OBJCOPY xtensa-esp-elf-objcopy)
set(CMAKE_SIZE xtensa-esp-elf-size)

set(CMAKE_C_FLAGS_INIT "-mabi=call0 -mcpu=esp32s3")
set(CMAKE_ASM_FLAGS_INIT "-mabi=call0 -mcpu=esp32s3")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE_INIT "-g -O2 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE_INIT "-g -O2 -DNDEBUG")

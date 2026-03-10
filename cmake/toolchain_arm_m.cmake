# toolchain_arm_m.cmake — ARM Cortex-M0+ cross-compilation toolchain
#
# Used by ARM targets that don't use the Pico SDK (e.g., qemu_arm).
# Pico targets (pico1, pico1calc) get their toolchain from pico_sdk_init().

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_SIZE arm-none-eabi-size)

set(CMAKE_C_FLAGS_INIT "-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft")
set(CMAKE_ASM_FLAGS_INIT "-mthumb -mcpu=cortex-m0plus -march=armv6s-m -mfloat-abi=soft")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Match Pico SDK defaults: Release build, debug-friendly optimization
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE_INIT "-g -O3 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE_INIT "-g -O3 -DNDEBUG")

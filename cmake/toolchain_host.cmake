# toolchain_host.cmake — Native host (Linux/macOS) build for user-space apps
#
# Unlike the cross-compiled targets, the host build produces native ELFs
# (or Mach-O on macOS) that run directly on the developer's machine.  No
# kernel, no romfs, no QEMU — just a thin POSIX shim layer that makes
# push and pi usable outside of PPAP.

set(CMAKE_SYSTEM_NAME ${CMAKE_HOST_SYSTEM_NAME})

# Let CMake pick up the system gcc/clang.  No cross-compile prefix.
if(NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES cc gcc clang)
endif()

# _GNU_SOURCE unlocks ppoll, getdents, sigaction struct, etc. on glibc.
# PPAP_HOST_BUILD is the source-level switch consumed by posix_shim.h.
add_compile_definitions(PPAP_HOST_BUILD=1 _GNU_SOURCE=1)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

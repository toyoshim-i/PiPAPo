# CMakeLists.txt — Host (native Linux/macOS) build for selected user apps
#
# Builds push and pi as native binaries runnable outside of PPAP.  Kernel,
# romfs, and QEMU are all bypassed.  A shadow "syscall.h" under include/
# redirects PPAP's syscall surface to libc, with posix_shim.c providing
# adapters for the five calls whose signatures diverge.
#
# Build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain_host.cmake \
#         -S src/target/host -B build/host
#   cmake --build build/host

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

project(ppap_host C)
set(CMAKE_C_STANDARD 11)

# Put target/host/include/ FIRST so "syscall.h" and "common/termios.h"
# resolve to the host shadows before the PPAP originals.
include_directories(BEFORE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
include_directories(
    ${PPAP_ROOT}/src
    ${PPAP_ROOT}/src/user
)

# Silence warnings from app code that targets freestanding embedded C;
# they compile cleanly but glibc has different conventions (e.g. no
# prototypes for "const char *" literals matching pointer args).
add_compile_options(-Wall -Wno-unused-parameter -Wno-unused-function
                    -Wno-unused-result)

# Force-include the shadow syscall.h on every app translation unit so
#   #include "syscall.h"
# resolves to the host version first.  The quoted-include same-directory
# rule would otherwise pick src/user/syscall.h before our -I path is
# tried; the shadow defines PPAP_USER_SYSCALL_H, so subsequent app-side
# includes are short-circuited by the original's guard.
#
# posix_shim.c must NOT receive the forced include — it talks to libc
# directly — so the force-include is attached per-source, not per-target.
set(SHIM_H ${CMAKE_CURRENT_SOURCE_DIR}/include/syscall.h)
set(APP_SRCS
    ${PPAP_ROOT}/src/user/hello.c
    ${PPAP_ROOT}/src/user/push.c
    ${PPAP_ROOT}/src/user/push_line.c
)
set_source_files_properties(${APP_SRCS}
    PROPERTIES COMPILE_FLAGS "-include ${SHIM_H}")

# hello: sanity check that needs no shim code.
add_executable(hello ${PPAP_ROOT}/src/user/hello.c)

# push: link the shim TU for getdents, brk, ppoll, sigaction, getcwd.
add_executable(push
    ${PPAP_ROOT}/src/user/push.c
    ${PPAP_ROOT}/src/user/push_line.c
    ${CMAKE_CURRENT_SOURCE_DIR}/posix_shim.c
)
# A host's inherited environ is far larger than the embedded 1.5 KB pool;
# bump the cap so env_init doesn't silently drop entries (which in turn
# would cause later `VAR=value` assignments to silently fail).
target_compile_definitions(push PRIVATE
    ENV_POOL_SIZE=16384
    ENV_MAX=256
)

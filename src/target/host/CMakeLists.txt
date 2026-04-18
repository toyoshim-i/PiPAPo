# CMakeLists.txt — Host (native Linux/macOS) build for selected user apps
#
# Builds push and pi as native binaries runnable outside of PPAP.  Kernel,
# romfs, and QEMU are all bypassed.  A thin posix_shim layer converts
# between PPAP's syscall ABI (syscall.h, common/*.h types) and host libc.
#
# Build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../../cmake/toolchain_host.cmake \
#         -S src/target/host -B build/host
#   cmake --build build/host
#
# Skeleton milestone: just hello (no shim needed — signatures match libc).
# push and pi land in the follow-up increments.

cmake_minimum_required(VERSION 3.13)

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

project(ppap_host C)
set(CMAKE_C_STANDARD 11)

# The user sources include "syscall.h" which brings in common/*.h for ABI
# types.  On host these resolve against libc by matching signatures.
include_directories(
    ${PPAP_ROOT}/src
    ${PPAP_ROOT}/src/user
)

# hello: one write() + return.  Serves as the plumbing sanity check.
add_executable(hello ${PPAP_ROOT}/src/user/hello.c)

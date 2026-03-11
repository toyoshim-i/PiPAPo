cmake_minimum_required(VERSION 3.13)

# ── Native host build — uses the system compiler (gcc/clang), NOT arm-none-eabi
project(ppap_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

# Treat warnings as errors so test code stays clean
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# ── Include path for kernel headers ──────────────────────────────────────────
# All kernel source files use paths relative to src/ (e.g. "config.h",
# "kernel/mm/page.h").  Point the host compiler at the same root.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# ── test_kmem ─────────────────────────────────────────────────────────────────
# kmem.c has no hardware dependencies — compiles and tests cleanly on the host.
add_executable(test_kmem
    ${SRC}/kernel/mm/kmem.c
    test_kmem.c
)
target_include_directories(test_kmem PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME kmem COMMAND test_kmem)

# ── test_h68k_path ──────────────────────────────────────────────────────────
# Human68k path translation and error code mapping — compiles the real
# h68k_util.c from kernel source.  Pure C, no stubs needed.
add_executable(test_h68k_path
    ${SRC}/kernel/subsys/h68k_util.c
    test_h68k_path.c
)
target_include_directories(test_h68k_path PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME h68k_path COMMAND test_h68k_path)

# ── test_ecpu_z80 ─────────────────────────────────────────────────────────
# Z80 eCPU core — pure C interpreter, no hardware dependencies.
add_executable(test_ecpu_z80
    ${SRC}/kernel/ecpu/ecpu_z80.c
    ${SRC}/kernel/ecpu/ecpu_z80_alu.c
    test_ecpu_z80.c
)
target_include_directories(test_ecpu_z80 PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ecpu_z80 COMMAND test_ecpu_z80)

# ── test_ecpu_m68k ────────────────────────────────────────────────────────
# m68k eCPU core — pure C interpreter, no hardware dependencies.
add_executable(test_ecpu_m68k
    ${SRC}/kernel/ecpu/ecpu_m68k.c
    ${SRC}/kernel/ecpu/ecpu_m68k_alu.c
    test_ecpu_m68k.c
)
target_include_directories(test_ecpu_m68k PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ecpu_m68k COMMAND test_ecpu_m68k)

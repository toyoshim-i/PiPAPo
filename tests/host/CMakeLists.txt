cmake_minimum_required(VERSION 3.13)

# ── Native host build — uses the system compiler (gcc/clang), NOT arm-none-eabi
project(ppap_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

# Treat warnings as errors so test code stays clean
add_compile_options(-Wall -Wextra -Werror)
add_compile_definitions(PPAP_HOST_TEST)

enable_testing()

# ── Include path for kernel headers ──────────────────────────────────────────
# All kernel source files use paths relative to src/ and src/kernel/
# (e.g. "common/config.h", "core/mm/page.h").  Point the host compiler
# at the same roots.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# ── test_kmem ─────────────────────────────────────────────────────────────────
# kmem.c has no hardware dependencies — compiles and tests cleanly on the host.
add_executable(test_kmem
    ${SRC}/kernel/core/mm/kmem.c
    test_kmem.c
)
target_include_directories(test_kmem PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME kmem COMMAND test_kmem)

# ── test_uc_heap ──────────────────────────────────────────────────────────
# uc_heap.c operates purely on caller-supplied memory — no syscalls,
# no arch dependencies — so it compiles cleanly on the host.
add_executable(test_uc_heap
    ${SRC}/user/lib/uc_heap.c
    test_uc_heap.c
)
target_include_directories(test_uc_heap PRIVATE
    ${SRC} ${SRC}/user ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME uc_heap COMMAND test_uc_heap)

# ── test_page_alloc ────────────────────────────────────────────────────────
# page_alloc.c is the pure allocator core — no locking, klogf, or hardware
# probes.  Factored out of page.c so it can be unit-tested on the host.
add_executable(test_page_alloc
    ${SRC}/kernel/core/mm/page_alloc.c
    test_page_alloc.c
)
target_include_directories(test_page_alloc PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME page_alloc COMMAND test_page_alloc)

# ── test_h68k_path ──────────────────────────────────────────────────────────
# Disabled: test references h68k_util.h / h68k_translate_path symbols that
# were renamed when the Human68k subsystem moved under human68k/.  Needs
# a full update (includes + function names).  TODO: revive.
# add_executable(test_h68k_path
#     ${SRC}/kernel/core/subsys/human68k/human68k_util.c
#     test_h68k_path.c
# )
# target_include_directories(test_h68k_path PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
# add_test(NAME h68k_path COMMAND test_h68k_path)

# ── test_ecpu_z80 ─────────────────────────────────────────────────────────
# Z80 eCPU core — pure C interpreter, no hardware dependencies.
add_executable(test_ecpu_z80
    ${SRC}/kernel/core/cpu/ecpu_z80.c
    ${SRC}/kernel/core/cpu/ecpu_z80_alu.c
    test_mem_region_stubs.c
    test_ecpu_z80.c
)
target_include_directories(test_ecpu_z80 PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ecpu_z80 COMMAND test_ecpu_z80)

# ── test_ecpu_m68k ────────────────────────────────────────────────────────
# m68k eCPU core — pure C interpreter, no hardware dependencies.
add_executable(test_ecpu_m68k
    ${SRC}/kernel/core/cpu/ecpu_m68k.c
    ${SRC}/kernel/core/cpu/ecpu_m68k_alu.c
    test_mem_region_stubs.c
    test_ecpu_m68k.c
)
target_include_directories(test_ecpu_m68k PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ecpu_m68k COMMAND test_ecpu_m68k)

# ── test_zexall ──────────────────────────────────────────────────────────
# ZEXALL — Z80 instruction exerciser (all flags including undocumented F3/F5).
# Superset of ZEXDOC: tests documented + undocumented flag behaviour (~4 min).
set(ZEXALL_COM ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/zexall/zexall.com)
add_executable(test_zexall
    ${SRC}/kernel/core/cpu/ecpu_z80.c
    ${SRC}/kernel/core/cpu/ecpu_z80_alu.c
    test_mem_region_stubs.c
    test_zexall.c
)
target_include_directories(test_zexall PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_zexall PRIVATE
    ZEXALL_COM_PATH="${ZEXALL_COM}")
add_test(NAME zexall COMMAND test_zexall)
set_tests_properties(zexall PROPERTIES TIMEOUT 600 LABELS "manual")

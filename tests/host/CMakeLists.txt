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

# ── Shared stub library ───────────────────────────────────────────────────────
# Provides UART no-ops, linker-symbol stubs, and tty file objects so that
# kernel source files that call uart_puts() / uart_print_hex32() etc. compile
# and link cleanly on the host without any hardware.
add_library(stubs STATIC
    stubs/uart_stub.c
    stubs/tty_stub.c
    stubs/xip_stub.c
)
target_include_directories(stubs PUBLIC ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})

# ── test_kmem ─────────────────────────────────────────────────────────────────
# kmem.c has no hardware dependencies — compiles and tests cleanly on the host.
add_executable(test_kmem
    ${SRC}/kernel/mm/kmem.c
    test_kmem.c
)
target_include_directories(test_kmem PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME kmem COMMAND test_kmem)

# ── test_page ─────────────────────────────────────────────────────────────────
# page.c calls uart_puts/uart_print_hex32/uart_print_dec and references the
# linker symbols __bss_end / __stack_top.  Both are provided by stubs.
# page.c also exercises kmem (self-test inside mm_init), so kmem.c is included.
add_executable(test_page
    ${SRC}/kernel/mm/page.c
    ${SRC}/kernel/mm/kmem.c
    test_page.c
)
target_include_directories(test_page PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_page PRIVATE stubs)
add_test(NAME page COMMAND test_page)

# ── test_fd ───────────────────────────────────────────────────────────────────
# fd.c uses tty_stdin/stdout/stderr (provided by tty_stub.c) and errno.h.
# pcb_t is pulled from proc.h (pure C structs — no hardware deps).
add_executable(test_fd
    ${SRC}/kernel/fd/fd.c
    test_fd.c
)
target_include_directories(test_fd PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_fd PRIVATE stubs)
add_test(NAME fd COMMAND test_fd)

# ── test_elf ──────────────────────────────────────────────────────────────────
# elf.c is pure C — no hardware dependencies, no stubs needed.
add_executable(test_elf
    ${SRC}/kernel/exec/elf.c
    test_elf.c
)
target_include_directories(test_elf PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME elf COMMAND test_elf)

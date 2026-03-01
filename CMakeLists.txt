cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

project(ppap C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

add_executable(ppap
    src/boot/startup.S      # Vector table + Reset_Handler (Step 4)
    src/boot/stage1.S       # Stage 1 extension: VTOR redirect to 0x10001000 (Step 8)
    src/kernel/main.c
    src/kernel/mm/xip.c     # XIP verification + SysTick benchmark (Step 9)
    src/kernel/mm/page.c    # Page allocator + boot memory map (Phase 1 Step 1)
    src/kernel/mm/kmem.c    # Fixed-size kernel object pool (Phase 1 Step 2)
    src/kernel/mm/mpu.c     # MPU 4-region layout + per-context switch (Phase 1 Step 11)
    src/kernel/proc/proc.c  # PCB definition + process table (Phase 1 Step 3)
    src/kernel/proc/switch.S # PendSV context switch (Phase 1 Step 4)
    src/kernel/proc/sched.c # Round-robin scheduler + SysTick (Phase 1 Steps 4-5)
    src/kernel/syscall/svc.S      # SVC exception handler entry (Phase 1 Step 8)
    src/kernel/syscall/syscall.c  # Syscall dispatch table (Phase 1 Step 8)
    src/kernel/syscall/sys_proc.c # sys_exit, sys_getpid (Phase 1 Step 9)
    src/kernel/syscall/sys_io.c   # sys_write, sys_read via fd table (Phase 1 Step 9)
    src/kernel/syscall/sys_time.c # sys_nanosleep (Phase 1 Step 9)
    src/kernel/fd/fd.c            # fd table helpers + fd_stdio_init (Phase 1 Step 10)
    src/kernel/fd/tty.c           # UART tty driver (Phase 1 Step 10)
    src/kernel/vfs/vfs.c          # VFS layer: mount table, vnode pool (Phase 2 Step 1)
    src/kernel/vfs/namei.c        # Path resolution: vfs_lookup (Phase 2 Step 2)
    src/kernel/smp.c              # Core 1 launch + SIO FIFO IPC (Phase 1 Step 12)
    src/drivers/uart.c
    src/drivers/clock.c     # PLL_SYS → 133 MHz (Step 7)
)

target_include_directories(ppap PRIVATE src)

# Bare SDK headers only — no pico_stdlib / pico_runtime / pico_crt0.
# We provide our own startup.S entry point.
# hardware_* implementation libraries will be added in Steps 5+ (UART, clocks).
target_link_libraries(ppap
    pico_base_headers   # Platform / board definitions (INTERFACE, headers only)
    hardware_regs       # RP2040 register address definitions (INTERFACE, headers only)
)

# Include the Pico SDK boot2 objects directly as sources, not via target_link_libraries.
# Using target_link_libraries(ppap bs2_default_library) causes CMake to emit the .o file
# twice in the link command (once as a source object, once as a library object),
# producing a 512-byte .boot2 section that overflows the 256-byte FLASH_BOOT region.
# $<TARGET_OBJECTS:...> adds the object files exactly once as compile-time inputs.
target_sources(ppap PRIVATE $<TARGET_OBJECTS:bs2_default_library>)

# Custom linker script (ppap.ld) + bare-metal link flags:
#   -nostartfiles    do not link default crt0 / startup files
#   --gc-sections    discard unused sections to keep binary small
target_link_options(ppap PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/ppap.ld
    -nostartfiles
    -Wl,--gc-sections
)

# Produce ppap.bin and ppap.uf2 alongside ppap.elf
pico_add_extra_outputs(ppap)

# =============================================================================
# QEMU smoke-test target — mps2-an500 (Cortex-M0+)  [Step 11]
# =============================================================================
# Builds a standalone ELF for QEMU.  Uses the same startup.S and mm/xip.c
# but replaces main.c with main_qemu.c (no RP2040 clock/PLL init) and
# uart.c with uart_qemu.c (CMSDK UART at 0x40004000).
#
# Run with:  ./scripts/qemu.sh
# Install:   sudo apt install qemu-system-arm
# =============================================================================

add_executable(ppap_qemu
    src/boot/startup.S          # Same Reset_Handler (startup is portable)
    src/kernel/main_qemu.c      # No RP2040-specific hardware init
    src/kernel/mm/xip.c         # XIP verification + SysTick benchmark (Step 9)
    src/kernel/mm/page.c        # Page allocator + boot memory map (Phase 1 Step 1)
    src/kernel/mm/kmem.c        # Fixed-size kernel object pool (Phase 1 Step 2)
    src/kernel/mm/mpu.c         # MPU 4-region layout + per-context switch (Phase 1 Step 11)
    src/kernel/proc/proc.c      # PCB definition + process table (Phase 1 Step 3)
    src/kernel/proc/switch.S    # PendSV context switch (Phase 1 Step 4)
    src/kernel/proc/sched.c     # Round-robin scheduler + SysTick (Phase 1 Steps 4-5)
    src/kernel/syscall/svc.S          # SVC exception handler entry (Phase 1 Step 8)
    src/kernel/syscall/syscall.c      # Syscall dispatch table (Phase 1 Step 8)
    src/kernel/syscall/sys_proc.c     # sys_exit, sys_getpid (Phase 1 Step 9)
    src/kernel/syscall/sys_io.c       # sys_write, sys_read via fd table (Phase 1 Step 9)
    src/kernel/syscall/sys_time.c     # sys_nanosleep (Phase 1 Step 9)
    src/kernel/fd/fd.c                # fd table helpers + fd_stdio_init (Phase 1 Step 10)
    src/kernel/fd/tty.c               # UART tty driver (Phase 1 Step 10)
    src/kernel/vfs/vfs.c              # VFS layer: mount table, vnode pool (Phase 2 Step 1)
    src/kernel/vfs/namei.c            # Path resolution: vfs_lookup (Phase 2 Step 2)
    src/kernel/smp.c                  # Core 1 launch + SIO FIFO IPC (Phase 1 Step 12)
    src/drivers/uart_qemu.c     # CMSDK UART for mps2-an500
)

target_include_directories(ppap_qemu PRIVATE src)

# No Pico SDK libraries — ppap_qemu is a raw ARM bare-metal target.
# The toolchain (arm-none-eabi-gcc / Cortex-M0+) is inherited from
# pico_sdk_init() and is identical to the mps2-an500 ISA.

target_link_options(ppap_qemu PRIVATE
    -T ${CMAKE_SOURCE_DIR}/ldscripts/qemu.ld
    -nostartfiles
    -Wl,--gc-sections
)

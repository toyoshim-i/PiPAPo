# ppap_kernel component — PPAP kernel sources for ESP-IDF build
#
# Registers the PPAP kernel as an ESP-IDF component so that idf.py
# can build it alongside the ESP-IDF framework.

set(PPAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# ── Architecture sources ─────────────────────────────────────────────────────
set(ARCH_SOURCES
    ${PPAP_ROOT}/src/arch/xtensa/boot.S
    ${PPAP_ROOT}/src/arch/xtensa/trap.S
    ${PPAP_ROOT}/src/arch/xtensa/switch.S
    ${PPAP_ROOT}/src/arch/xtensa/xtensa_common.c
    ${PPAP_ROOT}/src/arch/xtensa/smp.c
    ${PPAP_ROOT}/src/drivers/arch/xtensa/uart_esp32s3.c
)

# ── Kernel shared sources ────────────────────────────────────────────────────
set(KERNEL_SOURCES
    ${PPAP_ROOT}/src/target/target_default.c
    ${PPAP_ROOT}/src/kernel/main.c
    ${PPAP_ROOT}/src/kernel/klog.c
    ${PPAP_ROOT}/src/kernel/mm/page.c
    ${PPAP_ROOT}/src/kernel/mm/kmem.c
    ${PPAP_ROOT}/src/kernel/proc/proc.c
    ${PPAP_ROOT}/src/kernel/proc/sched.c
    ${PPAP_ROOT}/src/kernel/syscall/syscall.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_proc.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_io.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_time.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_mem.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_poll.c
    ${PPAP_ROOT}/src/kernel/syscall/sys_fs.c
    ${PPAP_ROOT}/src/kernel/fd/fd.c
    ${PPAP_ROOT}/src/kernel/fd/tty.c
    ${PPAP_ROOT}/src/kernel/fd/pipe.c
    ${PPAP_ROOT}/src/kernel/signal/signal.c
    ${PPAP_ROOT}/src/kernel/vfs/vfs.c
    ${PPAP_ROOT}/src/kernel/vfs/namei.c
    ${PPAP_ROOT}/src/kernel/fs/romfs.c
    ${PPAP_ROOT}/src/kernel/fs/devfs.c
    ${PPAP_ROOT}/src/kernel/fs/procfs.c
    ${PPAP_ROOT}/src/kernel/fs/tmpfs.c
    ${PPAP_ROOT}/src/kernel/fs/fstab.c
    ${PPAP_ROOT}/src/kernel/cpu/cpu.c
    ${PPAP_ROOT}/src/kernel/cpu/cpu_native.c
    ${PPAP_ROOT}/src/kernel/exec/elf.c
    ${PPAP_ROOT}/src/kernel/exec/loader.c
    ${PPAP_ROOT}/src/kernel/exec/elf_loader.c
    ${PPAP_ROOT}/src/kernel/exec/exec.c
    ${PPAP_ROOT}/src/kernel/subsys/subsys.c
    ${PPAP_ROOT}/src/target/xtensa_cc/xtensa_cc_stubs.c
)

idf_component_register(
    SRCS ${ARCH_SOURCES} ${KERNEL_SOURCES}
    INCLUDE_DIRS
        "${PPAP_ROOT}/src"
        "${PPAP_ROOT}/src/kernel"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PPAP_KERNEL=1
    PPAP_SYS_HZ=240000000u
    PAGE_POOL_BASE=0x3FC90000u
    PAGE_COUNT_MAX=64u
)

# Warnings (do not add -Werror: ESP-IDF headers trigger warnings we can't fix)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)

# kernel.cmake — Shared kernel source lists for all targets.
#
# Defines arch-specific and arch-independent source lists that targets
# compose as needed:
#   ARM targets:  ${KERNEL_COMMON_SOURCES} [+ ${KERNEL_BLKDEV_SOURCES}]
#   m68k targets: ${ARCH_M68K_SOURCES} ${KERNEL_SHARED_SOURCES}
#
# Subsystem and eCPU sources are conditionally included based on CMake options:
#   - PPAP_ENABLE_HUMAN68K (default ON)
#   - PPAP_ENABLE_CPM (default ON)
#   - PPAP_ENABLE_ECPU_M68K (default ON)
#   - PPAP_ENABLE_ECPU_Z80 (default ON)

include_guard(GLOBAL)

get_filename_component(_KS_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

# ── Architecture-specific sources ───────────────────────────────────────────

set(ARCH_ARM_M_SOURCES
    ${_KS_ROOT}/src/arch/arm_m/boot.S
    ${_KS_ROOT}/src/arch/arm_m/switch.S
    ${_KS_ROOT}/src/arch/arm_m/trap.S
    ${_KS_ROOT}/src/arch/arm_m/arm_m_common.c
    ${_KS_ROOT}/src/arch/arm_m/smp.c
)

set(ARCH_M68K_SOURCES
    ${_KS_ROOT}/src/arch/m68k/boot.S
    ${_KS_ROOT}/src/arch/m68k/m68k_common.c
    ${_KS_ROOT}/src/arch/m68k/probe_ram.S
    ${_KS_ROOT}/src/arch/m68k/string.c
    ${_KS_ROOT}/src/arch/m68k/switch.S
    ${_KS_ROOT}/src/arch/m68k/trap.S
    ${_KS_ROOT}/src/arch/m68k/math.S
    ${_KS_ROOT}/src/arch/m68k/signal_m68k.S
    ${_KS_ROOT}/src/arch/m68k/smp.c
)

set(ARCH_RISCV_SOURCES
    ${_KS_ROOT}/src/arch/riscv/boot.S
    ${_KS_ROOT}/src/arch/riscv/trap.S
    ${_KS_ROOT}/src/arch/riscv/riscv_common.c
    ${_KS_ROOT}/src/arch/riscv/smp.c
)

set(ARCH_XTENSA_SOURCES
    ${_KS_ROOT}/src/arch/xtensa/boot.S
    ${_KS_ROOT}/src/arch/xtensa/trap.S
    ${_KS_ROOT}/src/arch/xtensa/xtensa_common.c
    ${_KS_ROOT}/src/arch/xtensa/smp.c
)

# ── Kernel sources shared across ALL architectures ──────────────────────────

set(KERNEL_SHARED_SOURCES_BASE
    ${_KS_ROOT}/src/target/target_default.c
    ${_KS_ROOT}/src/kernel/core/main.c
    ${_KS_ROOT}/src/kernel/core/klog.c
    ${_KS_ROOT}/src/kernel/common/mod/mod_core.c
    ${_KS_ROOT}/src/kernel/core/mm/page.c
    ${_KS_ROOT}/src/kernel/core/mm/kmem.c
    ${_KS_ROOT}/src/kernel/core/mm/mem_region.c
    ${_KS_ROOT}/src/kernel/core/proc/proc.c
    ${_KS_ROOT}/src/kernel/core/proc/sched.c
    ${_KS_ROOT}/src/kernel/core/syscall/syscall.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_proc.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_io.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_time.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_mem.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_poll.c
    ${_KS_ROOT}/src/kernel/core/syscall/sys_fs.c
    ${_KS_ROOT}/src/kernel/vfs/fd.c
    ${_KS_ROOT}/src/kernel/vfs/tty.c
    ${_KS_ROOT}/src/kernel/vfs/pipe.c
    ${_KS_ROOT}/src/kernel/core/signal/signal.c
    ${_KS_ROOT}/src/kernel/vfs/vfs.c
    ${_KS_ROOT}/src/kernel/vfs/namei.c
    ${_KS_ROOT}/src/kernel/vfs/romfs.c
    ${_KS_ROOT}/src/kernel/vfs/devfs.c
    ${_KS_ROOT}/src/kernel/vfs/procfs.c
    ${_KS_ROOT}/src/kernel/vfs/tmpfs.c
    ${_KS_ROOT}/src/kernel/vfs/fstab.c
    ${_KS_ROOT}/src/kernel/core/cpu/cpu.c
    ${_KS_ROOT}/src/kernel/core/cpu/cpu_native.c
    ${_KS_ROOT}/src/kernel/core/exec/elf.c
    ${_KS_ROOT}/src/kernel/core/exec/loader.c
    ${_KS_ROOT}/src/kernel/core/exec/elf_loader.c
    ${_KS_ROOT}/src/kernel/core/exec/exec.c
    ${_KS_ROOT}/src/kernel/core/subsys/subsys.c
)

# ── Optional subsystem and eCPU sources ──────────────────────────────────────

set(KERNEL_SUBSYS_HUMAN68K_SOURCES
    ${_KS_ROOT}/src/kernel/core/subsys/h68k_util.c
    ${_KS_ROOT}/src/kernel/core/subsys/human68k_bridge.c
    ${_KS_ROOT}/src/kernel/core/subsys/human68k_loader.c
    ${_KS_ROOT}/src/kernel/core/exec/x_loader.c
    ${_KS_ROOT}/src/kernel/core/exec/r_loader.c
    ${_KS_ROOT}/src/kernel/core/exec/h68k_emu.c
)

set(KERNEL_SUBSYS_CPM_SOURCES
    ${_KS_ROOT}/src/kernel/core/subsys/cpm_bridge.c
    ${_KS_ROOT}/src/kernel/core/subsys/cpm_loader.c
    ${_KS_ROOT}/src/kernel/core/exec/com_loader.c
)

set(KERNEL_SUBSYS_SOS_SOURCES
    ${_KS_ROOT}/src/kernel/core/subsys/sos_bridge.c
    ${_KS_ROOT}/src/kernel/core/exec/sos_loader.c
)

set(KERNEL_ECPU_Z80_SOURCES
    ${_KS_ROOT}/src/kernel/core/cpu/ecpu_z80.c
    ${_KS_ROOT}/src/kernel/core/cpu/ecpu_z80_alu.c
)

set(KERNEL_ECPU_M68K_SOURCES
    ${_KS_ROOT}/src/kernel/core/cpu/ecpu_m68k.c
    ${_KS_ROOT}/src/kernel/core/cpu/ecpu_m68k_alu.c
    ${_KS_ROOT}/src/kernel/core/exec/m68k_emu_loader.c
    ${_KS_ROOT}/src/kernel/core/subsys/ppap_m68k_bridge.c
)

# ── Composite kernel sources (conditionally assembled) ───────────────────────

set(KERNEL_SHARED_SOURCES ${KERNEL_SHARED_SOURCES_BASE})

if(PPAP_ENABLE_HUMAN68K)
    list(APPEND KERNEL_SHARED_SOURCES ${KERNEL_SUBSYS_HUMAN68K_SOURCES})
endif()

if(PPAP_ENABLE_CPM)
    list(APPEND KERNEL_SHARED_SOURCES ${KERNEL_SUBSYS_CPM_SOURCES})
endif()

if(PPAP_ENABLE_SOS)
    list(APPEND KERNEL_SHARED_SOURCES ${KERNEL_SUBSYS_SOS_SOURCES})
endif()

if(PPAP_ENABLE_ECPU_Z80)
    list(APPEND KERNEL_SHARED_SOURCES ${KERNEL_ECPU_Z80_SOURCES})
endif()

if(PPAP_ENABLE_ECPU_M68K)
    list(APPEND KERNEL_SHARED_SOURCES ${KERNEL_ECPU_M68K_SOURCES})
endif()

# ── ARM-only kernel modules (XIP flash, MPU) ────────────────────────────────

set(KERNEL_ARM_ONLY_SOURCES
    ${_KS_ROOT}/src/kernel/core/mm/xip.c
    ${_KS_ROOT}/src/kernel/core/mm/mpu.c
)

# ── Convenience: complete kernel sources for ARM targets ────────────────────
# Backward-compatible with existing ARM target CMakeLists.txt files.

set(KERNEL_COMMON_SOURCES
    ${ARCH_ARM_M_SOURCES}
    ${KERNEL_ARM_ONLY_SOURCES}
    ${KERNEL_SHARED_SOURCES}
)

# ── Convenience: complete kernel sources for RISC-V targets ──────────────────

set(KERNEL_RISCV_COMMON_SOURCES
    ${ARCH_RISCV_SOURCES}
    ${KERNEL_SHARED_SOURCES}
)

# ── Convenience: complete kernel sources for Xtensa targets ──────────────────

set(KERNEL_XTENSA_COMMON_SOURCES
    ${ARCH_XTENSA_SOURCES}
    ${KERNEL_SHARED_SOURCES}
)

# ── Block device + VFAT/UFS sources (targets with PPAP_HAS_BLKDEV) ─────────

set(KERNEL_BLKDEV_SOURCES
    ${_KS_ROOT}/src/kernel/core/driver/blkdev.c
    ${_KS_ROOT}/src/kernel/core/driver/loopback.c
    ${_KS_ROOT}/src/kernel/vfs/vfat.c
    ${_KS_ROOT}/src/kernel/vfs/ufs.c
)

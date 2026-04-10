/*
 * version.h — Kernel version and architecture name constants
 *
 * Single source of truth for version strings used by /proc/version,
 * sys_uname, and boot messages.
 *
 * PPAP_ARCH_NAME is resolved from compiler-defined macros so that both
 * core and VFS modules can use it without a cross-module call.
 */

#ifndef PPAP_KERNEL_COMMON_VERSION_H
#define PPAP_KERNEL_COMMON_VERSION_H

#define PPAP_SYSNAME "PiPAPo"
#define PPAP_VERSION "0.11.0"

#if defined(__ARM_ARCH) || defined(__arm__) || defined(__thumb__)
#define PPAP_ARCH_NAME "armv6m"
#elif defined(__m68k__)
#define PPAP_ARCH_NAME "m68k"
#elif defined(__ia16__)
#define PPAP_ARCH_NAME "i16"
#elif defined(__riscv)
#define PPAP_ARCH_NAME "riscv32"
#elif defined(__xtensa__)
#define PPAP_ARCH_NAME "xtensa"
#else
#error "Unsupported architecture — add PPAP_ARCH_NAME here"
#endif

#endif /* PPAP_KERNEL_COMMON_VERSION_H */

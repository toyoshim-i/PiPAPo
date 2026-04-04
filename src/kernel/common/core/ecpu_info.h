/*
 * ecpu_info.h --- Emulated CPU core name registry
 *
 * Shared between core (subsys.c) and VFS (procfs.c).
 * The name array is defined in subsys.c (core module).
 */

#ifndef PPAP_KERNEL_COMMON_CORE_ECPU_INFO_H
#define PPAP_KERNEL_COMMON_CORE_ECPU_INFO_H

#define ECPU_MAX 4

extern const char *ecpu_names[ECPU_MAX];

#endif /* PPAP_KERNEL_COMMON_CORE_ECPU_INFO_H */

/*
 * subsys_info.h --- OS personality subsystem constants and names
 *
 * Shared between core (proc.h, subsys.c) and VFS (procfs.c).
 * The name arrays are defined in subsys.c (core module).
 */

#ifndef PPAP_KERNEL_COMMON_CORE_SUBSYS_INFO_H
#define PPAP_KERNEL_COMMON_CORE_SUBSYS_INFO_H

#define SUBSYS_MAX 5
#define SUBSYS_PPAP 0
#define SUBSYS_HUMAN68K 1
#define SUBSYS_CPM 2
#define SUBSYS_SOS 3
#define SUBSYS_MSDOS 4

extern const char *subsys_names[SUBSYS_MAX];

#endif /* PPAP_KERNEL_COMMON_CORE_SUBSYS_INFO_H */

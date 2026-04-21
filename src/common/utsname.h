/*
 * common/utsname.h — Shared layout of struct utsname for SYS_UNAME
 *
 * sys_uname (kernel/core/syscall/sys_proc.c) writes UTS_FIELD_COUNT
 * fixed-size char fields back-to-back into the user buffer.  User-space
 * and kernel must agree on UTS_LEN and the field order.
 */

#ifndef PPAP_COMMON_UTSNAME_H
#define PPAP_COMMON_UTSNAME_H

#define UTS_LEN 65
#define UTS_FIELD_COUNT 6

struct utsname {
  char sysname[UTS_LEN];    /* e.g. "PPAP" */
  char nodename[UTS_LEN];   /* e.g. "qemu_arm" */
  char release[UTS_LEN];    /* kernel version string */
  char version[UTS_LEN];    /* build stamp */
  char machine[UTS_LEN];    /* arch name */
  char domainname[UTS_LEN]; /* currently empty */
};

#endif /* PPAP_COMMON_UTSNAME_H */

/*
 * uaccess.h — User-space memory access API
 *
 * All kernel code that reads/writes user-space memory must go through
 * these functions.  On 32-bit flat targets (ARM, m68k, RISC-V) the
 * user address is a plain linear address and access is a bounds-checked
 * memcpy.  On i16 the user address is a 20-bit linear address
 * (user_ds * 16 + offset) resolved through the page pool.
 *
 * user_addr is always a uint32_t linear address — never a raw pointer,
 * because void* is only 16-bit on i16.
 */

#ifndef PPAP_KERNEL_MM_UACCESS_H
#define PPAP_KERNEL_MM_UACCESS_H

#include <stddef.h>
#include <stdint.h>

/*
 * copy_from_user — copy n bytes from user-space to a kernel buffer.
 *
 * Returns 0 on success, -EFAULT if the user address range is invalid.
 */
int copy_from_user(void *kbuf, uint32_t user_addr, size_t n);

/*
 * copy_to_user — copy n bytes from a kernel buffer to user-space.
 *
 * Returns 0 on success, -EFAULT if the user address range is invalid.
 */
int copy_to_user(uint32_t user_addr, const void *kbuf, size_t n);

/*
 * strncpy_from_user — copy a NUL-terminated string from user-space.
 *
 * Copies up to max-1 bytes and always NUL-terminates kbuf.
 * Returns the string length (excluding NUL) on success,
 * or -EFAULT if the user address is invalid.
 */
int strncpy_from_user(char *kbuf, uint32_t user_addr, size_t max);

#endif /* PPAP_KERNEL_MM_UACCESS_H */

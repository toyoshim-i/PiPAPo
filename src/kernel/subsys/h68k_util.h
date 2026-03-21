/*
 * h68k_util.h — Human68k path translation and error code mapping
 *
 * Pure utility functions with no kernel dependencies beyond errno.h.
 * Separated so host tests can compile them directly.
 */

#ifndef PPAP_KERNEL_SUBSYS_H68K_UTIL_H
#define PPAP_KERNEL_SUBSYS_H68K_UTIL_H

#include <stdint.h>

/* Translate a Human68k path (e.g. "A:\DIR\FILE") to a UNIX path.
 * Returns length on success, -1 on buffer overflow. */
int h68k_translate_path(const char *src, char *dst, int dstsize);

/* Map a PPAP errno (negative) to a Human68k error code.
 * Non-negative values pass through unchanged. */
int32_t h68k_errno(long ppap_err);

#endif /* PPAP_KERNEL_SUBSYS_H68K_UTIL_H */

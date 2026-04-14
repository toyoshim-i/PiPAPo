/*
 * h68k_util.c — Human68k path translation and error code mapping
 */

#include "kernel/core/subsys/human68k/h68k_util.h"

#include "common/errno.h"

/* ── Path translation: Human68k → UNIX ─────────────────────────────── */

int h68k_translate_path(const char *src, char *dst, int dstsize) {
  int si = 0, di = 0;

  /* Drive letter: "X:" */
  if (src[0] && src[1] == ':') {
    char drive = src[0];
    if (drive >= 'A' && drive <= 'Z') drive += ('a' - 'A');
    if (di + 3 > dstsize) return -1;
    dst[di++] = '/';
    dst[di++] = drive;
    dst[di++] = '/';
    si = 2;
    /* Skip leading backslash after drive letter */
    if (src[si] == '\\' || src[si] == '/') si++;
  } else if (src[0] == '\\' || src[0] == '/') {
    /* Absolute path without drive letter */
    if (di + 1 > dstsize) return -1;
    dst[di++] = '/';
    si = 1;
  }
  /* else: relative path — no prefix */

  /* Copy rest, converting backslash to forward slash */
  while (src[si]) {
    if (di + 1 >= dstsize) return -1;
    dst[di++] = (src[si] == '\\') ? '/' : src[si];
    si++;
  }
  dst[di] = '\0';
  return di;
}

/* ── Error code translation: PPAP errno → Human68k ─────────────────── */

int32_t h68k_errno(long ppap_err) {
  if (ppap_err >= 0) return (int32_t)ppap_err;

  switch ((int)(-ppap_err)) {
    case ENOENT:
      return -2; /* File not found */
    case ENOTDIR:
      return -3; /* Directory not found */
    case EMFILE:
      return -4; /* Too many open files */
    case EISDIR:
      return -5; /* Is a directory */
    case EBADF:
      return -6; /* Invalid handle */
    case ENOMEM:
      return -8; /* Out of memory */
    case EACCES:
      return -13; /* Access denied */
    case EROFS:
      return -13; /* Read-only → access denied */
    case EINVAL:
      return -22; /* Invalid data */
    case ENOSPC:
      return -23; /* Disk full */
    case EEXIST:
      return -80; /* File exists */
    case ENOTEMPTY:
      return -21; /* Directory not empty */
    case ENOSYS:
      return -1; /* Invalid function */
    default:
      return -1; /* Generic error */
  }
}
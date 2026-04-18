/*
 * fcntl.h — host shadow of src/common/fcntl.h
 *
 * pi.c pulls this in for O_RDONLY/O_WRONLY/O_CREAT/O_TRUNC.  On host we
 * use the libc constants (same numeric values on Linux, and libc already
 * defines F_OK/R_OK/W_OK/X_OK which PPAP's version lacks).
 */

#ifndef PPAP_TARGET_HOST_INCLUDE_COMMON_FCNTL_H
#define PPAP_TARGET_HOST_INCLUDE_COMMON_FCNTL_H

#include <fcntl.h>
#include <unistd.h>  /* F_OK, R_OK, W_OK, X_OK */

#endif /* PPAP_TARGET_HOST_INCLUDE_COMMON_FCNTL_H */

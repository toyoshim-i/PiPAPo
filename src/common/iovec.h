/*
 * iovec.h --- Scatter/gather I/O vector
 *
 * Shared between kernel and user space.
 */

#ifndef PPAP_COMMON_IOVEC_H
#define PPAP_COMMON_IOVEC_H

#include <stddef.h>

struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

#endif /* PPAP_COMMON_IOVEC_H */

/*
 * <errno.h> — error number macros (EINVAL, ENOENT, …).
 *
 * Forwards to the kernel/user shared definitions under
 * src/common/errno.h.  The `errno` variable itself is not yet
 * provided — syscall wrappers still return negative error codes
 * directly, so adding storage without setters would only hide bugs.
 */

#ifndef _ERRNO_H
#define _ERRNO_H

#include "common/errno.h"

#endif /* _ERRNO_H */

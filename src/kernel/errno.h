/*
 * errno.h — POSIX error codes used by the kernel syscall layer
 *
 * Numbers match the Linux/ARM EABI ABI so that musl libc user programs
 * interpret them correctly when returned as negative values from syscalls.
 */

#ifndef PPAP_ERRNO_H
#define PPAP_ERRNO_H

#define ENOENT   2   /* No such file or directory           */
#define EIO      5   /* I/O error                           */
#define ENOEXEC  8   /* Exec format error                   */
#define EBADF    9   /* Bad file descriptor                 */
#define ENOMEM  12   /* Out of memory                       */
#define EACCES  13   /* Permission denied                   */
#define EBUSY   16   /* Device or resource busy             */
#define EEXIST  17   /* File exists                         */
#define ENOTDIR 20   /* Not a directory                     */
#define EISDIR  21   /* Is a directory                      */
#define EINVAL  22   /* Invalid argument                    */
#define EMFILE  24   /* Too many open files                 */
#define ENOSPC  28   /* No space left on device             */
#define ESPIPE  29   /* Illegal seek (pipe/socket/tty)      */
#define ERANGE  34   /* Result too large (buffer too small) */
#define ENAMETOOLONG 36 /* File name too long               */
#define ENOSYS  38   /* Function not implemented            */
#define ENOTEMPTY 39 /* Directory not empty                 */
#define ELOOP   40   /* Too many levels of symbolic links   */

#endif /* PPAP_ERRNO_H */

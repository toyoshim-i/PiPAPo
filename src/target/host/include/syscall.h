/*
 * syscall.h — host shadow of src/user/syscall.h
 *
 * When PPAP_HOST_BUILD is set, src/target/host/include/ is placed ahead
 * of src/user/ on the include path, so apps that write
 *   #include "syscall.h"
 * resolve to this file instead of the PPAP SVC wrappers.  It pulls in
 * libc, types them as libc does, and declares the few call-outs whose
 * signatures or semantics do not match libc 1-to-1.  Implementations of
 * those call-outs live in src/target/host/posix_shim.c.
 */

#ifndef PPAP_TARGET_HOST_INCLUDE_SYSCALL_H
#define PPAP_TARGET_HOST_INCLUDE_SYSCALL_H

/* Short-circuit the PPAP-original src/user/syscall.h.  The force-include
 * (-include ...) that loads this file runs before the app's quoted
 *   #include "syscall.h"
 * which then falls through because PPAP_USER_SYSCALL_H is already set. */
#define PPAP_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* ── Libc-backed types and prototypes ──────────────────────────────────── */

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* PPAP-specific dirent d_type values.  Linux's <dirent.h> already exposes
 * DT_REG, DT_DIR, DT_CHR, DT_LNK with the same numeric values PPAP uses, so
 * no redefinition is needed. */

/* PPAP_NAME_MAX: in-app buffer sizing for paths (was 63 on the device).  On
 * the host we keep the same value so app buffers stay consistent. */
#ifndef PPAP_NAME_MAX
#define PPAP_NAME_MAX 63
#endif

/* ── Non-trivial shims (see posix_shim.c) ──────────────────────────────── */

/* getdents: PPAP apps expect struct dirent-compatible entries packed into
 * the caller's buffer.  On host we wrap fdopendir+readdir, keyed by fd. */
int ppap_getdents(int fd, struct dirent *buf, size_t count);
#define getdents(fd, buf, count) ppap_getdents((fd), (buf), (count))

/* brk: PPAP semantics — brk(NULL) queries break, brk(addr) sets it and
 * returns the new break.  Libc's brk(2) returns 0/-1; we use sbrk(3). */
void *ppap_brk(void *addr);
#define brk(addr) ppap_brk(addr)

/* ppoll: PPAP has (fds, nfds, timeout, sigmask, sigsetsize).  Libc drops
 * sigsetsize.  Shim strips the last arg. */
int ppap_ppoll(struct pollfd *fds, int nfds, const void *timeout,
               const void *sigmask, int sigsetsize);
#define ppoll(f, n, t, s, z) ppap_ppoll((f), (n), (t), (s), (z))

/* sigaction: PPAP passes a raw function pointer; libc expects struct
 * sigaction*.  Shim adapts. */
int ppap_sigaction(int sig, void *handler, void *old_handler);
#define sigaction(sig, h, oh) ppap_sigaction((sig), (h), (oh))

/* getcwd: PPAP returns length (or -1).  Libc returns char* or NULL. */
int ppap_getcwd(char *buf, size_t size);
#define getcwd(buf, size) ppap_getcwd((buf), (size))

/* stat: field set differs (PPAP is st_ino/st_mode/st_nlink/st_size of
 * uint32_t each).  The host shadow just forwards to libc stat — callers
 * only touch st_mode and st_size in practice, both names match. */

/* PPAP ioctl codes that diverge from libc headers.  On Linux these happen
 * to match the kernel's values already; define them unconditionally so
 * apps compile on any host libc. */
#ifndef TCGETS
#define TCGETS 0x5401u
#endif
#ifndef TCSETS
#define TCSETS 0x5402u
#endif

/* PPAP termios flag values — ensure they match what the app expects.  On
 * a Linux libc, these are already defined by <termios.h> with the same
 * values; we simply don't redefine them. */

/* open() in PPAP is 3-arg (path, flags, mode).  Libc is variadic but the
 * 3-arg form matches exactly, so no wrapper needed.  Same for access/
 * mkdir/unlink/rmdir/chdir/readlink/pipe/dup/dup2/execve/kill/nanosleep/
 * clock_gettime/ioctl/vfork/waitpid/getpid/getppid/setsid/setpgid/
 * getpgid/_exit/read/write/readv/writev. */

/* poweroff / statfs64: not used by push or pi; provide no-op stubs so
 * code that happens to reference them still links. */
static inline void poweroff(void) { /* no-op on host */ }

/* PPAP's int lseek(int,int,int).  Libc returns off_t.  Neither push nor
 * pi use it, so we skip the wrapper.  If a future app needs it, add here. */

#endif /* PPAP_TARGET_HOST_INCLUDE_SYSCALL_H */

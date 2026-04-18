/*
 * posix_shim.c — host adapters for the five PPAP syscalls whose
 * signatures or semantics do not match libc 1-to-1.
 *
 *   ppap_getdents   fd-keyed DIR* cache over fdopendir/readdir
 *   ppap_brk        PPAP brk(NULL=query) semantics on top of sbrk(3)
 *   ppap_ppoll      5-arg PPAP form → 4-arg libc ppoll
 *   ppap_sigaction  raw handler pointer → struct sigaction
 *   ppap_getcwd     char*-or-NULL libc → int length-or-(-1) PPAP
 *
 * Everything else (read, write, open, vfork, execve, ioctl, ...)
 * resolves directly against libc.  The shadow syscall.h under
 * src/target/host/include/ redirects the above via #define.
 *
 * This TU intentionally does NOT include "syscall.h" or the PPAP common
 * shared headers — those names are shadowed elsewhere; here we work
 * purely against libc.
 */

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Forward declarations match the shim header exactly. */
int ppap_getdents(int fd, struct dirent *buf, size_t count);
void *ppap_brk(void *addr);
int ppap_ppoll(struct pollfd *fds, int nfds, const void *timeout,
               const void *sigmask, int sigsetsize);
int ppap_sigaction(int sig, void *handler, void *old_handler);
int ppap_getcwd(char *buf, size_t size);

/* ── getdents: fd → DIR* cache ─────────────────────────────────────────── */

/* Small fixed table.  push's tab completion opens one directory at a time;
 * pi does not call getdents.  Eight slots is ample. */
#define SHIM_DIR_SLOTS 8
static struct {
  int fd;     /* caller's fd; -1 == free slot */
  DIR *dirp;  /* fdopendir(dup(fd)) */
} shim_dirs[SHIM_DIR_SLOTS];

static DIR *shim_dir_get(int fd) {
  for (int i = 0; i < SHIM_DIR_SLOTS; i++)
    if (shim_dirs[i].fd == fd) return shim_dirs[i].dirp;
  for (int i = 0; i < SHIM_DIR_SLOTS; i++) {
    if (shim_dirs[i].fd < 0 || shim_dirs[i].fd == 0) {
      int dupfd = dup(fd);
      if (dupfd < 0) return NULL;
      DIR *d = fdopendir(dupfd);
      if (!d) { close(dupfd); return NULL; }
      shim_dirs[i].fd = fd;
      shim_dirs[i].dirp = d;
      return d;
    }
  }
  return NULL;
}

int ppap_getdents(int fd, struct dirent *buf, size_t count) {
  if (count < sizeof(struct dirent)) { errno = EINVAL; return -1; }
  DIR *d = shim_dir_get(fd);
  if (!d) return -1;
  errno = 0;
  struct dirent *e = readdir(d);
  if (!e) return 0;  /* end of directory */
  memcpy(buf, e, sizeof(struct dirent));
  return (int)sizeof(struct dirent);
}

/* ── brk: PPAP semantics on top of sbrk ────────────────────────────────── */

void *ppap_brk(void *addr) {
  if (addr == NULL) return sbrk(0);  /* query */
  void *cur = sbrk(0);
  intptr_t delta = (intptr_t)((char *)addr - (char *)cur);
  void *r = sbrk(delta);
  if (r == (void *)-1) return cur;   /* PPAP returns unchanged on failure */
  return sbrk(0);
}

/* ── ppoll: strip sigsetsize ───────────────────────────────────────────── */

int ppap_ppoll(struct pollfd *fds, int nfds, const void *timeout,
               const void *sigmask, int sigsetsize) {
  (void)sigsetsize;
  return ppoll(fds, (nfds_t)nfds, (const struct timespec *)timeout,
               (const sigset_t *)sigmask);
}

/* ── sigaction: raw handler → struct sigaction ─────────────────────────── */

int ppap_sigaction(int sig, void *handler, void *old_handler) {
  struct sigaction sa, old;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = (void (*)(int))handler;
  sigemptyset(&sa.sa_mask);
  int r = sigaction(sig, &sa, &old);
  if (old_handler)
    *(void **)old_handler = (void *)old.sa_handler;
  return r;
}

/* ── getcwd: libc char* → PPAP int length ──────────────────────────────── */

int ppap_getcwd(char *buf, size_t size) {
  if (getcwd(buf, size) == NULL) return -1;
  return (int)strlen(buf);
}

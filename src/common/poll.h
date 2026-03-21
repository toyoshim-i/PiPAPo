/*
 * poll.h --- Poll event flags and struct pollfd
 *
 * Shared between kernel and user space.
 */

#ifndef PPAP_COMMON_POLL_H
#define PPAP_COMMON_POLL_H

/* Poll event bitmask (matches Linux values) */
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

/* Poll file descriptor structure */
struct pollfd {
  int fd;
  short events;
  short revents;
};

#endif /* PPAP_COMMON_POLL_H */

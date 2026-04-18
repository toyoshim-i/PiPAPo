/*
 * fcntl.h --- File open flags and access mode constants
 *
 * Shared between kernel and user space.
 */

#ifndef PPAP_COMMON_FCNTL_H
#define PPAP_COMMON_FCNTL_H

/* Open-mode flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x0040    /* create file if it doesn't exist          */
#define O_TRUNC 0x0200    /* truncate file to zero length             */
#define O_APPEND 0x0400   /* append writes to end of file             */
#define O_NONBLOCK 0x0800 /* non-blocking I/O — read returns EAGAIN    */

/* Access-mode mask (low 2 bits) */
#define O_ACCMODE 3

/* fcntl() commands */
#define F_GETFL 3
#define F_SETFL 4

/* access() mode bits */
#define F_OK 0
#define R_OK 4

#endif /* PPAP_COMMON_FCNTL_H */

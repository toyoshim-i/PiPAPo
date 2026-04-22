/*
 * syscall_nr.h --- PPAP syscall numbers
 *
 * Shared between kernel (dispatch) and user-space (ASM stubs).
 * 16-bit scheme: high byte = group, low byte = index.
 * Architecture-independent --- same numbers on ARM and m68k.
 */

#ifndef PPAP_COMMON_SYSCALL_NR_H
#define PPAP_COMMON_SYSCALL_NR_H

/* Group 0x00: Process lifecycle */
#define SYS_EXIT 0x0000
#define SYS_EXIT_GROUP 0x0001
#define SYS_VFORK 0x0002
#define SYS_EXECVE 0x0003
#define SYS_WAITPID 0x0004
#define SYS_WAIT4 0x0005
#define SYS_GETPID 0x0006
#define SYS_GETPPID 0x0008
#define SYS_SETPGID 0x0009
#define SYS_GETPGID 0x000A
#define SYS_SETSID 0x000B
#define SYS_CLONE 0x000C
#define SYS_SET_TID_ADDRESS 0x000D
#define SYS_FORK 0x000E
#define SYS_PTRACE 0x000F

/* Group 0x01: I/O */
#define SYS_READ 0x0100
#define SYS_WRITE 0x0101
#define SYS_OPEN 0x0102
#define SYS_CLOSE 0x0103
#define SYS_DUP 0x0104
#define SYS_DUP2 0x0105
#define SYS_PIPE 0x0106
#define SYS_IOCTL 0x0107
#define SYS_FCNTL 0x0108
#define SYS_READV 0x0109
#define SYS_WRITEV 0x010A
#define SYS_LSEEK 0x010B

/* Group 0x02: File system --- paths */
#define SYS_STAT 0x0200
#define SYS_FSTAT 0x0201
#define SYS_ACCESS 0x0202
#define SYS_GETCWD 0x0203
#define SYS_MKDIR 0x0204
#define SYS_RMDIR 0x0205
#define SYS_UNLINK 0x0206
#define SYS_CHDIR 0x0207
#define SYS_READLINK 0x0208
#define SYS_RENAME 0x0209
#define SYS_MKNOD 0x020A
#define SYS_CHMOD 0x020B
#define SYS_OPENAT 0x020C
#define SYS_FSTATAT64 0x020D
#define SYS_LINK 0x020E

/* Group 0x03: File system --- extended */
#define SYS_GETDENTS 0x0300
#define SYS_STAT64 0x0301
#define SYS_FSTAT64 0x0302
#define SYS_UMASK 0x0303
#define SYS_LSTAT64 0x0304
#define SYS_STATFS64 0x0305
#define SYS_FSTATFS64 0x0306
#define SYS_LLSEEK 0x0307
#define SYS_STATX 0x0308
#define SYS_UTIMES 0x0309
#define SYS_GETDENTS64 0x030A

/* Group 0x04: Memory management */
#define SYS_BRK 0x0400
#define SYS_MMAP2 0x0401
#define SYS_MUNMAP 0x0402
#define SYS_MPROTECT 0x0403
#define SYS_MREMAP 0x0404

/* Group 0x05: Time */
#define SYS_NANOSLEEP 0x0500
#define SYS_CLOCK_GETTIME32 0x0501
#define SYS_GETTIMEOFDAY 0x0502
#define SYS_CLOCK_NANOSLEEP32 0x0503
#define SYS_CLOCK_GETTIME64 0x0504
#define SYS_CLOCK_NANOSLEEP64 0x0505
#define SYS_SETTIMEOFDAY 0x0506

/* Group 0x06: Signals */
#define SYS_KILL 0x0600
#define SYS_SIGACTION 0x0601
#define SYS_SIGRETURN 0x0602
#define SYS_RT_SIGACTION 0x0603
#define SYS_RT_SIGPROCMASK 0x0604
#define SYS_RT_SIGRETURN 0x0605

/* Group 0x07: Poll */
#define SYS_POLL 0x0700
#define SYS_PPOLL 0x0701

/* Group 0x08: User/group identity */
#define SYS_GETUID 0x0800
#define SYS_GETGID 0x0801
#define SYS_GETEUID 0x0802
#define SYS_GETEGID 0x0803
#define SYS_CHOWN 0x0804
#define SYS_LCHOWN 0x0805
#define SYS_SETGROUPS 0x0806
#define SYS_FCHOWN 0x0807

/* Group 0x09: Mount / filesystem ops */
#define SYS_MOUNT 0x0900
#define SYS_UMOUNT2 0x0901

/* Group 0x0A: Misc */
#define SYS_FUTEX 0x0A00
#define SYS_GETCPU 0x0A01

/* Group 0x0B: System control */
#define SYS_POWEROFF 0x0B00
#define SYS_UNAME 0x0B01

/* AT_FDCWD: *at syscalls use this as dirfd for cwd-relative paths */
#define AT_FDCWD (-100)

#endif /* PPAP_COMMON_SYSCALL_NR_H */

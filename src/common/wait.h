#ifndef PPAP_COMMON_WAIT_H
#define PPAP_COMMON_WAIT_H

/* waitpid options */
#define WNOHANG     1
#define WSTOPPED    2
#define WUNTRACED   WSTOPPED
#define WCONTINUED  8

/* Wait status helpers compatible with musl's sys/wait.h macros. */
#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WSTOPSIG(s)    WEXITSTATUS(s)
#define WIFEXITED(s)   (!WTERMSIG(s))
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WIFSIGNALED(s) (((unsigned)((s) & 0xffff) - 1U) < 0xffU)
#define WIFCONTINUED(s) ((s) == 0xffff)

#endif /* PPAP_COMMON_WAIT_H */

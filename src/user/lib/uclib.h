/*
 * uclib.h — micro C library for PPAP user space
 *
 * Tiny, freestanding utility library.  No heap, no global state.
 * All functions are reentrant.  Build with -ffunction-sections
 * -fdata-sections and link with --gc-sections so unused functions
 * are stripped at zero cost.
 */

#ifndef PPAP_USER_LIB_UCLIB_H
#define PPAP_USER_LIB_UCLIB_H

#include "syscall.h"

/* --- formatted output (fd-based) --- */
void uc_puts(const char *s);  /* NUL-terminated string to stdout */
void uc_eputs(const char *s); /* NUL-terminated string to stderr */
void uc_putc(char c);         /* single char to stdout */
void uc_putu(uint32_t v);     /* decimal unsigned */
void uc_puti(int32_t v);      /* decimal signed */
void uc_putx32(uint32_t v);   /* "0x" + 8 hex digits */
void uc_putx16(uint32_t v);   /* "0x" + 4 hex digits */
void uc_putx8(uint32_t v);    /* "0x" + 2 hex digits */

/* snprintf-like: returns bytes written (excl. NUL), truncates safely.
 * Supported: %s %d %u %x %c %%, width/zero-pad for %d/%u/%x. */
int uc_snprintf(char *buf, int size, const char *fmt, ...);

/* --- string operations --- */
int uc_strlen(const char *s);
int uc_strcmp(const char *a, const char *b);
int uc_strncmp(const char *a, const char *b, int n);
char *uc_strcpy(char *dst, const char *src);
char *uc_strncpy(char *dst, const char *src, int n);
char *uc_strchr(const char *s, int c);
char *uc_strrchr(const char *s, int c);

/* --- memory operations --- */
void *uc_memcpy(void *dst, const void *src, int n);
void *uc_memset(void *dst, int c, int n);
int uc_memcmp(const void *a, const void *b, int n);

/* --- numeric parsing --- */
int uc_atoi(const char *s);
int uc_parse_u32(const char *s, uint32_t *out); /* 0 on success */

/* --- path helpers --- */
const char *uc_basename(const char *path);

/* --- environment ---
 *
 * `environ` points at the NULL-terminated envp array the kernel placed
 * on the initial user stack (right after argv's NULL terminator).  It
 * is set by _uclib_init_env(), which crt0 calls before main().  Apps
 * can read env variables via uc_getenv() without having to walk argv
 * themselves.  Modifying environ or its strings is not supported — use
 * a shell/builtin-level env table for that (push does). */
extern char **environ;
const char *uc_getenv(const char *name);

/* Called from crt0 before main() — computes environ from argc/argv.
 * Never call from application code. */
void _uclib_init_env(int argc, char **argv);

#endif /* PPAP_USER_LIB_UCLIB_H */

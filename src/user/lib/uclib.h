/*
 * uclib.h — PPAP user-space extensions over the POSIX-flavoured libc.
 *
 * For the POSIX subset (strlen, memcpy, malloc, printf, snprintf, …)
 * include <string.h>, <stdlib.h>, <stdio.h> directly.  This header
 * keeps only the PPAP-specific helpers that have no POSIX equivalent
 * yet — the surviving `uc_` symbols.  The `uc_` prefix is a deliberate
 * TODO marker: each entry is slated to migrate to a standard name once
 * the matching POSIX surface lands.
 *
 * Build with -ffunction-sections -fdata-sections so each function
 * lands in its own ELF section.  Link with --gc-sections to strip
 * anything the app doesn't reference.
 */

#ifndef PPAP_USER_LIB_UCLIB_H
#define PPAP_USER_LIB_UCLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syscall.h"

/* ── Numeric parsing (will fold into strtoul-based replacement) ───── */
int uc_parse_u32(const char *s, uint32_t *out); /* 0 on success */

/* ── File copy helper ─────────────────────────────────────────────── *
 *
 * Read from src_fd, write to dst_fd, until EOF.  Returns total bytes
 * copied (≥ 0) on success, -1 on any read / write error (errno-ish
 * context is lost — callers that need detail should open-code the loop).
 * Internally uses a stack-sized buffer so it's safe in small applets
 * without heap.
 */
long uc_copy_fd(int src_fd, int dst_fd);

/* ── Environment ──────────────────────────────────────────────────── *
 *
 * `environ` points at the NULL-terminated envp array the kernel placed
 * on the initial user stack (right after argv's NULL terminator).  It
 * is set by _uclib_init_env(), which crt0 calls before main().  Apps
 * read env variables via getenv() (declared in <stdlib.h>) without
 * having to walk argv themselves.
 */
extern char **environ;

/* Called from crt0 before main() — computes environ from argc/argv.
 * Never call from application code. */
void _uclib_init_env(int argc, char **argv);

/* ── User-space heap pool seeding ─────────────────────────────────── *
 *
 * malloc / free are declared in <stdlib.h>.  Each process must seed
 * the allocator's pool before first malloc; uc_heap_init takes a
 * caller-owned static buffer.  See docs/user/malloc.md.
 */
void uc_heap_init(void *pool, size_t size);

#endif /* PPAP_USER_LIB_UCLIB_H */

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

/* ── stdout / stderr stopgaps ─────────────────────────────────────────
 *
 * Replaced by POSIX puts / fputs(s, stderr) / fputc(c, stderr) /
 * fprintf(stderr, …) once <stdio.h> grows FILE streams.
 */
void uc_puts(const char *s);  /* stdout, no auto-newline */
void uc_eputs(const char *s); /* stderr, no auto-newline */
void uc_eprintf(const char *fmt, ...);

/* ── Unbuffered numeric output (re-evaluate when printf is mature) ── */
void uc_putu(uint32_t v);   /* decimal unsigned */
void uc_puti(int32_t v);    /* decimal signed */
void uc_putx32(uint32_t v); /* "0x" + 8 hex digits */
void uc_putx16(uint32_t v); /* "0x" + 4 hex digits */
void uc_putx8(uint32_t v);  /* "0x" + 2 hex digits */

/* ── Numeric parsing (will fold into strtoul-based replacement) ───── */
int uc_parse_u32(const char *s, uint32_t *out); /* 0 on success */

/* ── Path (future <libgen.h>) ─────────────────────────────────────── */
const char *uc_basename(const char *path);

/* ── File copy helper ─────────────────────────────────────────────── *
 *
 * Read from src_fd, write to dst_fd, until EOF.  Returns total bytes
 * copied (≥ 0) on success, -1 on any read / write error (errno-ish
 * context is lost — callers that need detail should open-code the loop).
 * Internally uses a stack-sized buffer so it's safe in small applets
 * without heap.
 */
long uc_copy_fd(int src_fd, int dst_fd);

/* ── Calendar (future <time.h>) ────────────────────────────────────── *
 *
 * Convert Unix epoch seconds (UTC) to broken-down time and format
 * for display.  No locale, no timezone handling — everything is UTC.
 * Suitable for `ls -l`, `date`, and similar.  Up to year 2106
 * (uint32 seconds).  POSIX gmtime / struct tm / strftime will replace
 * these once <time.h> grows the full surface.
 */
struct uc_tm {
  int year; /* full Gregorian year, e.g. 2026 */
  int mon;  /* 1-12 */
  int mday; /* 1-31 */
  int hour; /* 0-23 */
  int min;  /* 0-59 */
  int sec;  /* 0-59 */
};
void uc_gmtime(uint32_t epoch, struct uc_tm *out);

/* Write "YYYY-MM-DD HH:MM" (exactly 16 bytes, NUL-terminated at
 * buf[16]) into buf.  Caller provides at least 17 bytes. */
void uc_format_ymdhm(char buf[17], uint32_t epoch);

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

/*
 * <stdio.h> — formatted and buffered I/O.
 *
 * POSIX subset.  Implementations live in src/user/lib/stdio.c
 * (formatted output) and src/user/lib/file.c (FILE streams).
 *
 * Not provided: freopen, tmpfile, mkstemp, %a / %n in printf,
 * locale-aware flags, wide-character variants.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* ── FILE streams ────────────────────────────────────────────────── */

typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifndef EOF
#define EOF (-1)
#endif

/* Default I/O buffer size for fopen-d streams.  Match what file.c uses. */
#ifndef BUFSIZ
#define BUFSIZ 512
#endif

/* setvbuf modes. */
#define _IOFBF 0 /* fully buffered */
#define _IOLBF 1 /* line buffered */
#define _IONBF 2 /* unbuffered */

/* fseek whence. */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *fp);
int fflush(FILE *fp);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);

int fputs(const char *s, FILE *fp);
int fputc(int c, FILE *fp);
int putc(int c, FILE *fp);

char *fgets(char *buf, int size, FILE *fp);
int fgetc(FILE *fp);
int getc(FILE *fp);
int getchar(void);
int ungetc(int c, FILE *fp);

int feof(FILE *fp);
int ferror(FILE *fp);
void clearerr(FILE *fp);

int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
void rewind(FILE *fp);
int setvbuf(FILE *fp, char *buf, int mode, size_t size);

/* ── Character / formatted output ─────────────────────────────────── */

int putchar(int c);
int puts(const char *s);

int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int fprintf(FILE *fp, const char *fmt, ...);
int vfprintf(FILE *fp, const char *fmt, va_list ap);

int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);

void perror(const char *s);
void setbuf(FILE *fp, char *buf);

#endif /* _STDIO_H */

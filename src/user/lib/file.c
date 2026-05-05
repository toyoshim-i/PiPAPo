/*
 * file.c — FILE streams.
 *
 * Provides the buffered-I/O surface POSIX programs expect: FILE *,
 * fopen / fclose / fflush, fread / fwrite, fputs / fputc / fgetc /
 * fgets / ungetc, feof / ferror / clearerr, fseek / ftell / rewind,
 * setvbuf — and the stdin / stdout / stderr globals.
 *
 * Design notes:
 *  - FILE is fully opaque to callers (struct _FILE is only declared
 *    here).
 *  - stdin / stdout / stderr default to unbuffered.  This matches
 *    PPAP's pre-M4 behaviour (every uc_puts / uc_eputs hit write(2)
 *    immediately) and avoids any malloc on programs that never call
 *    fopen.  Apps that want buffering can call setvbuf().
 *  - fopen-returned streams are fully buffered with a BUFSIZ-sized
 *    malloc'd buffer.
 *  - getc / putc are functions, not macros — keeps <stdio.h> opaque.
 */

#include "lib/uclib.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal FILE definition ────────────────────────────────────── */

#define _IO_READ      0x0001u /* opened for reading */
#define _IO_WRITE     0x0002u /* opened for writing */
#define _IO_EOF       0x0004u /* end-of-file reached */
#define _IO_ERR       0x0008u /* I/O error sticky flag */
#define _IO_NOBUF     0x0010u /* unbuffered (mode == _IONBF) */
#define _IO_LINEBUF   0x0020u /* flush on '\n' (mode == _IOLBF) */
#define _IO_FULLBUF   0x0040u /* default for fopen-d streams (_IOFBF) */
#define _IO_OWN_BUF   0x0080u /* free(buf) on fclose */
#define _IO_OWN_FILE  0x0100u /* free(fp) on fclose */
#define _IO_DIRTY     0x0200u /* buf holds unflushed write data */

struct _FILE {
  int fd;
  unsigned int flags;
  unsigned char *buf;
  size_t buf_size; /* capacity */
  size_t buf_pos;  /* read: next byte to consume; write: next slot to fill */
  size_t buf_end;  /* read: bytes valid in buf */
  int ungot;       /* 0..255 if a ungetc'd byte is pending, else -1 */
};

/* ── Standard streams ────────────────────────────────────────────── */

static FILE _stdin = {
  .fd = 0, .flags = _IO_READ | _IO_NOBUF, .ungot = -1};
static FILE _stdout = {
  .fd = 1, .flags = _IO_WRITE | _IO_NOBUF, .ungot = -1};
static FILE _stderr = {
  .fd = 2, .flags = _IO_WRITE | _IO_NOBUF, .ungot = -1};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Write `n` bytes from `buf` to fp's fd, dealing with short writes.
 * Sets _IO_ERR on failure.  Returns bytes written. */
static size_t fd_write_all(FILE *fp, const void *buf, size_t n) {
  const unsigned char *p = buf;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fp->fd, p + off, n - off);
    if (w <= 0) {
      fp->flags |= _IO_ERR;
      return off;
    }
    off += (size_t)w;
  }
  return off;
}

/* Drain the write buffer to fd.  Returns 0 on success, EOF on error. */
static int flush_write_buf(FILE *fp) {
  if (!(fp->flags & _IO_DIRTY) || fp->buf_pos == 0) {
    fp->buf_pos = 0;
    fp->flags &= ~_IO_DIRTY;
    return 0;
  }
  size_t n = fp->buf_pos;
  fp->buf_pos = 0;
  fp->flags &= ~_IO_DIRTY;
  if (fd_write_all(fp, fp->buf, n) != n) return EOF;
  return 0;
}

/* Refill the read buffer (read mode only).  Returns bytes loaded, or 0
 * on EOF, -1 on error. */
static int refill_read_buf(FILE *fp) {
  fp->buf_pos = 0;
  fp->buf_end = 0;
  ssize_t n = read(fp->fd, fp->buf, fp->buf_size);
  if (n < 0) {
    fp->flags |= _IO_ERR;
    return -1;
  }
  if (n == 0) {
    fp->flags |= _IO_EOF;
    return 0;
  }
  fp->buf_end = (size_t)n;
  return (int)n;
}

/* ── fopen / fclose / fflush ─────────────────────────────────────── */

#define BUFSIZ 512

FILE *fopen(const char *path, const char *mode) {
  if (!path || !mode || !mode[0]) return (void *)0;

  int oflags;
  unsigned int io_flag;
  if (mode[0] == 'r') {
    oflags = O_RDONLY;
    io_flag = _IO_READ;
  } else if (mode[0] == 'w') {
    oflags = O_WRONLY | O_CREAT | O_TRUNC;
    io_flag = _IO_WRITE;
  } else if (mode[0] == 'a') {
    oflags = O_WRONLY | O_CREAT | O_APPEND;
    io_flag = _IO_WRITE;
  } else {
    return (void *)0;
  }

  for (size_t i = 1; mode[i]; i++) {
    if (mode[i] == '+') {
      oflags = (oflags & ~O_ACCMODE) | O_RDWR;
      io_flag = _IO_READ | _IO_WRITE;
    }
    /* 'b' / 't' / 'e' / 'x' / etc. — accepted, ignored.  PPAP has no
     * text-mode translation. */
  }

  int fd = open(path, oflags, 0644);
  if (fd < 0) return (void *)0;

  FILE *fp = malloc(sizeof(FILE));
  if (!fp) {
    close(fd);
    return (void *)0;
  }
  unsigned char *buf = malloc(BUFSIZ);
  if (!buf) {
    free(fp);
    close(fd);
    return (void *)0;
  }
  fp->fd = fd;
  fp->flags = io_flag | _IO_FULLBUF | _IO_OWN_BUF | _IO_OWN_FILE;
  fp->buf = buf;
  fp->buf_size = BUFSIZ;
  fp->buf_pos = 0;
  fp->buf_end = 0;
  fp->ungot = -1;
  return fp;
}

int fflush(FILE *fp) {
  if (!fp) return 0; /* glibc flushes all — we don't track open list */
  if (fp->flags & _IO_DIRTY) return flush_write_buf(fp);
  /* For read mode, fflush is officially undefined; clear the buffer. */
  fp->buf_pos = 0;
  fp->buf_end = 0;
  fp->ungot = -1;
  return 0;
}

int fclose(FILE *fp) {
  if (!fp) return EOF;
  int rc = 0;
  if (fp->flags & _IO_DIRTY) {
    if (flush_write_buf(fp) != 0) rc = EOF;
  }
  if (close(fp->fd) < 0) rc = EOF;
  if (fp->flags & _IO_OWN_BUF) free(fp->buf);
  if (fp->flags & _IO_OWN_FILE) free(fp);
  return rc;
}

/* ── fwrite / fread ──────────────────────────────────────────────── */

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || size == 0 || nmemb == 0) return 0;
  size_t total = size * nmemb;
  const unsigned char *src = ptr;

  if (fp->flags & _IO_NOBUF) {
    size_t w = fd_write_all(fp, src, total);
    return w / size;
  }

  size_t off = 0;
  while (off < total) {
    /* If buffer full, flush. */
    if (fp->buf_pos == fp->buf_size) {
      if (flush_write_buf(fp) != 0) return off / size;
    }
    /* Bulk write bypassing buffer when chunk is at least buffer size. */
    if (off == 0 && fp->buf_pos == 0 && total - off >= fp->buf_size) {
      size_t w = fd_write_all(fp, src + off, total - off);
      off += w;
      if (w != total) return off / size;
      break;
    }
    size_t room = fp->buf_size - fp->buf_pos;
    size_t take = total - off;
    if (take > room) take = room;
    memcpy(fp->buf + fp->buf_pos, src + off, take);
    fp->buf_pos += take;
    fp->flags |= _IO_DIRTY;
    off += take;

    /* Line-buffer flush on newline. */
    if (fp->flags & _IO_LINEBUF) {
      for (size_t i = take; i > 0; i--) {
        if (src[off - i] == '\n') {
          if (flush_write_buf(fp) != 0) return off / size;
          break;
        }
      }
    }
  }
  return off / size;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || size == 0 || nmemb == 0) return 0;
  size_t total = size * nmemb;
  unsigned char *dst = ptr;
  size_t off = 0;

  /* Honour any ungetc'd byte first. */
  if (fp->ungot >= 0) {
    dst[off++] = (unsigned char)fp->ungot;
    fp->ungot = -1;
    if (off == total) return total / size;
  }

  if (fp->flags & _IO_NOBUF) {
    while (off < total) {
      ssize_t r = read(fp->fd, dst + off, total - off);
      if (r < 0) {
        fp->flags |= _IO_ERR;
        break;
      }
      if (r == 0) {
        fp->flags |= _IO_EOF;
        break;
      }
      off += (size_t)r;
    }
    return off / size;
  }

  while (off < total) {
    if (fp->buf_pos >= fp->buf_end) {
      /* Bulk read bypassing buffer for large remaining chunks. */
      if (total - off >= fp->buf_size) {
        ssize_t r = read(fp->fd, dst + off, total - off);
        if (r < 0) {
          fp->flags |= _IO_ERR;
          break;
        }
        if (r == 0) {
          fp->flags |= _IO_EOF;
          break;
        }
        off += (size_t)r;
        continue;
      }
      int got = refill_read_buf(fp);
      if (got <= 0) break;
    }
    size_t avail = fp->buf_end - fp->buf_pos;
    size_t take = total - off;
    if (take > avail) take = avail;
    memcpy(dst + off, fp->buf + fp->buf_pos, take);
    fp->buf_pos += take;
    off += take;
  }
  return off / size;
}

/* ── Single-byte / line ops ──────────────────────────────────────── */

int fputc(int c, FILE *fp) {
  unsigned char ch = (unsigned char)c;
  if (fwrite(&ch, 1, 1, fp) != 1) return EOF;
  return ch;
}

int putc(int c, FILE *fp) { return fputc(c, fp); }

int fputs(const char *s, FILE *fp) {
  size_t n = strlen(s);
  return fwrite(s, 1, n, fp) == n ? 0 : EOF;
}

int fgetc(FILE *fp) {
  unsigned char c;
  if (fread(&c, 1, 1, fp) != 1) return EOF;
  return c;
}

int getc(FILE *fp) { return fgetc(fp); }

int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *fp) {
  if (!fp || c == EOF) return EOF;
  fp->ungot = (unsigned char)c;
  fp->flags &= ~_IO_EOF;
  return (unsigned char)c;
}

char *fgets(char *buf, int size, FILE *fp) {
  if (!buf || size <= 0 || !fp) return (void *)0;
  int i = 0;
  while (i < size - 1) {
    int c = fgetc(fp);
    if (c == EOF) {
      if (i == 0) return (void *)0;
      break;
    }
    buf[i++] = (char)c;
    if (c == '\n') break;
  }
  buf[i] = '\0';
  return buf;
}

/* ── Status / positioning ───────────────────────────────────────── */

int feof(FILE *fp) { return fp && (fp->flags & _IO_EOF) ? 1 : 0; }

int ferror(FILE *fp) { return fp && (fp->flags & _IO_ERR) ? 1 : 0; }

void clearerr(FILE *fp) {
  if (fp) fp->flags &= ~(_IO_EOF | _IO_ERR);
}

int fseek(FILE *fp, long offset, int whence) {
  if (!fp) return -1;
  if (fp->flags & _IO_DIRTY) {
    if (flush_write_buf(fp) != 0) return -1;
  }
  fp->buf_pos = 0;
  fp->buf_end = 0;
  fp->ungot = -1;
  fp->flags &= ~_IO_EOF;
  if (lseek(fp->fd, (int)offset, whence) < 0) return -1;
  return 0;
}

long ftell(FILE *fp) {
  if (!fp) return -1;
  /* Sync first so the underlying fd offset is accurate. */
  if (fp->flags & _IO_DIRTY) {
    if (flush_write_buf(fp) != 0) return -1;
  }
  long pos = lseek(fp->fd, 0, SEEK_CUR);
  if (pos < 0) return -1;
  /* Adjust for unread buffered bytes (pos is past them). */
  if (fp->buf_end > fp->buf_pos)
    pos -= (long)(fp->buf_end - fp->buf_pos);
  if (fp->ungot >= 0) pos -= 1;
  return pos;
}

void rewind(FILE *fp) {
  fseek(fp, 0L, SEEK_SET);
  clearerr(fp);
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size) {
  if (!fp) return -1;
  /* Only allow before first I/O on the stream — caller's
   * responsibility.  We just swap the flags / buffer. */
  if (fp->flags & _IO_DIRTY) flush_write_buf(fp);
  fp->buf_pos = 0;
  fp->buf_end = 0;
  fp->ungot = -1;
  fp->flags &= ~(_IO_NOBUF | _IO_LINEBUF | _IO_FULLBUF);

  if (mode == _IONBF) {
    fp->flags |= _IO_NOBUF;
    return 0;
  }
  if (mode == _IOLBF)
    fp->flags |= _IO_LINEBUF;
  else if (mode == _IOFBF)
    fp->flags |= _IO_FULLBUF;
  else
    return -1;

  /* Caller-supplied buffer takes ownership away from us. */
  if (buf) {
    if (fp->flags & _IO_OWN_BUF) {
      free(fp->buf);
      fp->flags &= ~_IO_OWN_BUF;
    }
    fp->buf = (unsigned char *)buf;
    fp->buf_size = size;
  }
  return 0;
}

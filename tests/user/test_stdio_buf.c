/*
 * test_stdio_buf.c — libc FILE buffering behavior
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "lib/uclib.h"
#include "utest.h"

#define TMPFILE "/tmp/_stdio_buf_test"
#define DATA_SIZE 700

static char heap[4096];
static char data[DATA_SIZE];
static char tmp[64];
static char line_buf[32];

static void make_data(void) {
  for (int i = 0; i < DATA_SIZE; i++) data[i] = (char)('A' + (i % 26));
}

static int same_bytes(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

static int write_file(const char *path, const char *buf, int n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return 0;
  int off = 0;
  while (off < n) {
    ssize_t w = write(fd, buf + off, (size_t)(n - off));
    if (w <= 0) {
      close(fd);
      return 0;
    }
    off += (int)w;
  }
  close(fd);
  return 1;
}

static int read_file(const char *path, char *buf, int n) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  int got = (int)read(fd, buf, (size_t)n);
  close(fd);
  return got;
}

int main(void) {
  uc_heap_init(heap, sizeof(heap));
  make_data();

  UT_ASSERT(write_file(TMPFILE, data, DATA_SIZE), "seed buffered read file");

  FILE *fp = fopen(TMPFILE, "r");
  UT_ASSERT(fp != 0, "fopen read");
  if (fp) {
    UT_ASSERT_EQ((int)fread(tmp, 1, 20, fp), 20);
    UT_ASSERT(same_bytes(tmp, data, 20), "fread initial bytes");
    UT_ASSERT_EQ((int)ftell(fp), 20);

    int c = fgetc(fp);
    UT_ASSERT_EQ(c, data[20]);
    UT_ASSERT_EQ(ungetc(c, fp), data[20]);
    UT_ASSERT_EQ((int)ftell(fp), 20);
    UT_ASSERT_EQ(fgetc(fp), data[20]);

    UT_ASSERT_EQ(fseek(fp, 510, SEEK_SET), 0);
    UT_ASSERT_EQ((int)fread(tmp, 1, 32, fp), 32);
    UT_ASSERT(same_bytes(tmp, data + 510, 32), "fread crosses buffer edge");
    UT_ASSERT_EQ(fclose(fp), 0);
  }

  fp = fopen(TMPFILE, "w");
  UT_ASSERT(fp != 0, "fopen write");
  if (fp) {
    UT_ASSERT_EQ((int)fwrite("hello", 1, 5, fp), 5);
    UT_ASSERT_EQ(read_file(TMPFILE, tmp, sizeof(tmp)), 0);
    UT_ASSERT_EQ(fflush(fp), 0);
    int got = read_file(TMPFILE, tmp, sizeof(tmp));
    UT_ASSERT_EQ(got, 5);
    UT_ASSERT(same_bytes(tmp, "hello", 5), "fflush exposes buffered write");
    UT_ASSERT_EQ((int)fwrite("!", 1, 1, fp), 1);
    UT_ASSERT_EQ(fclose(fp), 0);
    got = read_file(TMPFILE, tmp, sizeof(tmp));
    UT_ASSERT_EQ(got, 6);
    UT_ASSERT(same_bytes(tmp, "hello!", 6), "fclose flushes buffered write");
  }

  fp = fopen(TMPFILE, "w");
  UT_ASSERT(fp != 0, "fopen setvbuf");
  if (fp) {
    UT_ASSERT_EQ(setvbuf(fp, 0, _IOFBF, 1024), 0);
    UT_ASSERT_EQ(fputc('x', fp), 'x');
    UT_ASSERT_EQ(read_file(TMPFILE, tmp, sizeof(tmp)), 0);
    UT_ASSERT_EQ(fclose(fp), 0);
    UT_ASSERT_EQ(read_file(TMPFILE, tmp, sizeof(tmp)), 1);
    UT_ASSERT_EQ(tmp[0], 'x');
  }

  fp = fopen(TMPFILE, "w");
  UT_ASSERT(fp != 0, "fopen line buffered");
  if (fp) {
    UT_ASSERT_EQ(setvbuf(fp, line_buf, _IOLBF, sizeof(line_buf)), 0);
    UT_ASSERT_EQ((int)fwrite("abc", 1, 3, fp), 3);
    UT_ASSERT_EQ(read_file(TMPFILE, tmp, sizeof(tmp)), 0);
    UT_ASSERT_EQ(fputc('\n', fp), '\n');
    UT_ASSERT_EQ(read_file(TMPFILE, tmp, sizeof(tmp)), 4);
    UT_ASSERT(same_bytes(tmp, "abc\n", 4), "line buffer flushes on newline");
    UT_ASSERT_EQ(fclose(fp), 0);
  }

  unlink(TMPFILE);
  UT_SUMMARY("test_stdio_buf");
}

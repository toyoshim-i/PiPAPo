/*
 * test_libc.c — exercises the libc surface added in M3 / M4 / M5.
 *
 *   printf width / zero-pad
 *   strtol / strtoul edge cases
 *   ctype classifiers
 *   qsort / bsearch
 *   FILE streams (fopen / fwrite / fread / fclose round-trip)
 *   strftime
 *   setjmp / longjmp
 */

#include "lib/uclib.h" /* uc_heap_init */
#include "utest.h"

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* tmpfs is mounted at /tmp in every PPAP image.  Use it for the FILE
 * round-trip test. */
#define TMPFILE "/tmp/_libc_test"

/* The allocator needs a pool seeded before first malloc(). */
static char libc_test_pool[2048];

static int int_cmp(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

int main(void) {
  uc_heap_init(libc_test_pool, sizeof(libc_test_pool));

  /* ── snprintf width / zero-pad ─────────────────────────────────── */
  char buf[64];
  snprintf(buf, sizeof(buf), "[%5d]", 42);
  UT_ASSERT(strcmp(buf, "[   42]") == 0, "snprintf %5d");
  snprintf(buf, sizeof(buf), "[%05d]", 42);
  UT_ASSERT(strcmp(buf, "[00042]") == 0, "snprintf %05d");
  snprintf(buf, sizeof(buf), "[%08x]", 0xabcdu);
  UT_ASSERT(strcmp(buf, "[0000abcd]") == 0, "snprintf %08x");

  /* ── strtol / strtoul ──────────────────────────────────────────── */
  char *end;
  UT_ASSERT_EQ(strtol("123", &end, 10), 123);
  UT_ASSERT(*end == '\0', "strtol: full consume");
  UT_ASSERT_EQ(strtol("  -42abc", &end, 10), -42);
  UT_ASSERT(*end == 'a', "strtol: stop at non-digit");
  UT_ASSERT_EQ(strtoul("0x1A", &end, 0), 26);
  UT_ASSERT(*end == '\0', "strtoul: 0x prefix");
  UT_ASSERT_EQ(strtoul("0755", &end, 0), 493); /* 0o755 = 493 */

  /* ── ctype ─────────────────────────────────────────────────────── */
  UT_ASSERT(isdigit('5'), "isdigit '5'");
  UT_ASSERT(!isdigit('x'), "!isdigit 'x'");
  UT_ASSERT(isalpha('Z'), "isalpha 'Z'");
  UT_ASSERT(isspace('\t'), "isspace tab");
  UT_ASSERT_EQ(toupper('a'), 'A');
  UT_ASSERT_EQ(tolower('Z'), 'z');

  /* ── qsort / bsearch ───────────────────────────────────────────── */
  int arr[] = {5, 2, 8, 1, 9, 3};
  int n = (int)(sizeof(arr) / sizeof(arr[0]));
  qsort(arr, n, sizeof(int), int_cmp);
  UT_ASSERT_EQ(arr[0], 1);
  UT_ASSERT_EQ(arr[n - 1], 9);
  int key = 8;
  int *hit = bsearch(&key, arr, n, sizeof(int), int_cmp);
  UT_ASSERT(hit && *hit == 8, "bsearch finds 8");
  key = 7;
  hit = bsearch(&key, arr, n, sizeof(int), int_cmp);
  UT_ASSERT(hit == 0, "bsearch misses 7");

  /* ── string ops added in M3 ────────────────────────────────────── */
  UT_ASSERT(strstr("hello world", "world") != 0, "strstr finds");
  UT_ASSERT(strstr("hello", "xyz") == 0, "strstr misses");
  UT_ASSERT_EQ(strspn("aabbcc", "ab"), 4);
  UT_ASSERT_EQ(strcspn("hello!", "!?,"), 5);

  /* ── FILE round-trip ───────────────────────────────────────────── */
  FILE *fp = fopen(TMPFILE, "w");
  UT_ASSERT(fp != 0, "fopen w");
  if (fp) {
    const char *msg = "abc\n123\n";
    size_t n_msg = strlen(msg);
    UT_ASSERT_EQ((int)fwrite(msg, 1, n_msg, fp), (int)n_msg);
    UT_ASSERT_EQ(fclose(fp), 0);
  }
  fp = fopen(TMPFILE, "r");
  UT_ASSERT(fp != 0, "fopen r");
  if (fp) {
    char rbuf[16] = {0};
    int got = (int)fread(rbuf, 1, sizeof(rbuf) - 1, fp);
    UT_ASSERT_EQ(got, 8);
    rbuf[got] = '\0';
    UT_ASSERT(strcmp(rbuf, "abc\n123\n") == 0, "fread round-trip");
    UT_ASSERT_EQ(fclose(fp), 0);
  }
  unlink(TMPFILE);

  /* ── strftime ──────────────────────────────────────────────────── */
  /* 2024-03-15 12:30:45 UTC = 1710505845 */
  time_t epoch = 1710505845L;
  struct tm tm;
  gmtime_r(&epoch, &tm);
  UT_ASSERT_EQ(tm.tm_year, 124);  /* 2024 - 1900 */
  UT_ASSERT_EQ(tm.tm_mon, 2);     /* March = 2 */
  UT_ASSERT_EQ(tm.tm_mday, 15);
  UT_ASSERT_EQ(tm.tm_hour, 12);
  UT_ASSERT_EQ(tm.tm_min, 30);
  UT_ASSERT_EQ(tm.tm_sec, 45);
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
  UT_ASSERT(strcmp(tbuf, "2024-03-15 12:30:45") == 0, "strftime full");
  strftime(tbuf, sizeof(tbuf), "%b %d", &tm);
  UT_ASSERT(strcmp(tbuf, "Mar 15") == 0, "strftime %b %d");

  /* ── setjmp / longjmp ──────────────────────────────────────────── */
  jmp_buf env;
  volatile int reached = 0;
  int rc = setjmp(env);
  if (rc == 0) {
    reached = 1;
    longjmp(env, 7);
  }
  UT_ASSERT_EQ(reached, 1);
  UT_ASSERT_EQ(rc, 7);

  /* longjmp(env, 0) should appear as 1 from setjmp. */
  reached = 0;
  rc = setjmp(env);
  if (rc == 0) {
    reached = 1;
    longjmp(env, 0);
  }
  UT_ASSERT_EQ(rc, 1);

  /* ── M6 additions: calloc / strcat / strncat / sprintf / strerror / */
  /*    perror / sscanf / rand / errno indirection                    */
  int *zeros = calloc(8, sizeof(int));
  UT_ASSERT(zeros != 0, "calloc returns ptr");
  UT_ASSERT_EQ(zeros[0], 0);
  UT_ASSERT_EQ(zeros[7], 0);
  free(zeros);

  char cat[16] = "foo";
  strcat(cat, "bar");
  UT_ASSERT(strcmp(cat, "foobar") == 0, "strcat");
  strncat(cat, "BAZQUX", 3);
  UT_ASSERT(strcmp(cat, "foobarBAZ") == 0, "strncat");

  char sp[32];
  sprintf(sp, "%s=%d", "answer", 42);
  UT_ASSERT(strcmp(sp, "answer=42") == 0, "sprintf");

  const char *msg = strerror(EINVAL);
  UT_ASSERT(msg && msg[0], "strerror returns string");

  /* errno + __errno_location plumbing. */
  errno = EAGAIN;
  UT_ASSERT_EQ(*__errno_location(), EAGAIN);
  errno = 0;

  /* sscanf — basic %d / %s / %x / %u. */
  int a, b;
  char w[32];
  int matched = sscanf("42 hello 0xff", "%d %s %x", &a, w, &b);
  UT_ASSERT_EQ(matched, 3);
  UT_ASSERT_EQ(a, 42);
  UT_ASSERT(strcmp(w, "hello") == 0, "sscanf %s");
  UT_ASSERT_EQ(b, 0xff);

  /* rand reproducibility under fixed seed. */
  srand(1);
  int r1 = rand();
  srand(1);
  int r2 = rand();
  UT_ASSERT_EQ(r1, r2);

  UT_SUMMARY("test_libc");
}

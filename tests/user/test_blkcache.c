/*
 * test_blkcache.c — block cache read-after-write coherency
 */

#include "utest.h"

#define SECTOR_SIZE 512

static unsigned char *original;
static unsigned char *pattern;
static unsigned char *verify;

static int same_sector(const unsigned char *a, const unsigned char *b) {
  for (int i = 0; i < SECTOR_SIZE; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

static void make_pattern(void) {
  for (int i = 0; i < SECTOR_SIZE; i++)
    pattern[i] = (unsigned char)(original[i] ^ (0xa5u + (unsigned int)i));
}

static int rewind_dev(int fd) { return lseek(fd, 0, SEEK_SET) == 0; }

static int alloc_sector_buffers(void) {
  uintptr_t brk0 = (uintptr_t)brk((void *)0);
  uintptr_t base = (brk0 + (SECTOR_SIZE - 1u)) & ~(uintptr_t)(SECTOR_SIZE - 1u);
  uintptr_t end = base + SECTOR_SIZE * 3u;

  if ((uintptr_t)brk((void *)end) != end) return 0;
  original = (unsigned char *)base;
  pattern = original + SECTOR_SIZE;
  verify = pattern + SECTOR_SIZE;
  return 1;
}

int main(void) {
  UT_ASSERT(alloc_sector_buffers(), "allocate sector-aligned buffers");

  int fd = open("/dev/mmcblk0", O_RDWR, 0);
  if (fd < 0) {
    UT_PRINT("  SKIP  /dev/mmcblk0 not available\n");
    UT_SUMMARY("test_blkcache");
  }

  ssize_t n = read(fd, original, SECTOR_SIZE);
  if (n != SECTOR_SIZE) {
    UT_PRINT("  SKIP  sector read not available\n");
    close(fd);
    UT_SUMMARY("test_blkcache");
  }
  make_pattern();

  UT_ASSERT(rewind_dev(fd), "rewind after cache fill");
  n = write(fd, pattern, SECTOR_SIZE);
  if (n != SECTOR_SIZE) {
    UT_PRINT("  SKIP  block device is not writable\n");
    close(fd);
    UT_SUMMARY("test_blkcache");
  }

  UT_ASSERT(rewind_dev(fd), "rewind after cached write");
  n = read(fd, verify, SECTOR_SIZE);
  UT_ASSERT_EQ(n, SECTOR_SIZE);
  UT_ASSERT(same_sector(verify, pattern), "read sees data written via cache");

  UT_ASSERT(rewind_dev(fd), "rewind before restore");
  n = write(fd, original, SECTOR_SIZE);
  UT_ASSERT_EQ(n, SECTOR_SIZE);

  UT_ASSERT(rewind_dev(fd), "rewind after restore");
  n = read(fd, verify, SECTOR_SIZE);
  UT_ASSERT_EQ(n, SECTOR_SIZE);
  UT_ASSERT(same_sector(verify, original), "restore invalidates cached sector");

  close(fd);
  UT_SUMMARY("test_blkcache");
}

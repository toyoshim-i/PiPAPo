/*
 * test_smp.c --- Hardware-only scheduler statistics smoke test
 *
 * The normal QEMU lanes do not execute this test because they have no second
 * core.  Hardware test overlays select it after Core 1 has been launched.
 */

#include "common/time.h"
#include "utest.h"

static int read_cpu1_total(uint32_t *total) {
  char buf[192];
  int fd = open("/proc/stat", O_RDONLY, 0);
  if (fd < 0) return -1;

  int n = read(fd, buf, sizeof(buf) - 1u);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = '\0';

  for (int i = 0; i + 5 < n; i++) {
    if (buf[i] != 'c' || buf[i + 1] != 'p' || buf[i + 2] != 'u' ||
        buf[i + 3] != '1' || buf[i + 4] != ' ')
      continue;

    uint32_t sum = 0;
    int value_count = 0;
    int j = i + 5;
    while (j < n && buf[j] != '\n' && value_count < 4) {
      uint32_t value = 0;
      while (j < n && buf[j] == ' ') j++;
      while (j < n && buf[j] >= '0' && buf[j] <= '9') {
        value = value * 10u + (uint32_t)(buf[j] - '0');
        j++;
      }
      sum += value;
      value_count++;
    }
    if (value_count == 4) {
      *total = sum;
      return 0;
    }
  }
  return -1;
}

int main(void) {
  struct timespec ts = {0, 50000000};
  uint32_t before = 0;
  uint32_t after = 0;

  UT_ASSERT_EQ(read_cpu1_total(&before), 0);
  UT_ASSERT(before > 0, "Core 1 accounting is active");
  UT_ASSERT_EQ(nanosleep(&ts, (void *)0), 0);
  UT_ASSERT_EQ(read_cpu1_total(&after), 0);
  UT_ASSERT(after > before, "Core 1 accounting advances");

  UT_SUMMARY("test_smp");
}

/*
 * test_calc_render.c — unit tests for src/user/calc/calc_render.c
 *
 * Pure value -> string formatting; no syscalls.
 */

#include "test_framework.h"

#include <string.h>

#include "calc/calc.h"

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int eq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

#define ASSERT_STREQ(actual, expected) do {                                 \
    tf_asserts++;                                                            \
    if (!eq((actual), (expected))) {                                         \
        fprintf(stderr, "  FAIL  %-40s  %s:%d  expected %s, got %s\n",      \
                tf_current, __FILE__, __LINE__, (expected), (actual));      \
        tf_fail++;                                                           \
    }                                                                        \
} while (0)

/* ── Raw-digit tests ───────────────────────────────────────────────────── */

static void test_render_dec_zero(void) {
  calc_value_str_t v;
  calc_render_value(0, CALC_BASE_DEC, CALC_W32, 1, &v);
  ASSERT_STREQ(v.digits, "0");
  ASSERT_EQ(v.negative, 0);
  ASSERT_EQ(v.len, 1);
}

static void test_render_dec_positive(void) {
  calc_value_str_t v;
  calc_render_value(1234, CALC_BASE_DEC, CALC_W32, 1, &v);
  ASSERT_STREQ(v.digits, "1234");
  ASSERT_EQ(v.negative, 0);
}

static void test_render_dec_negative_w32(void) {
  calc_value_str_t v;
  /* In W32 signed, the bit pattern 0xFFFFFFFF reads as -1 */
  calc_render_value((int64_t)0xFFFFFFFF, CALC_BASE_DEC, CALC_W32, 1, &v);
  ASSERT_STREQ(v.digits, "1");
  ASSERT_EQ(v.negative, 1);
}

static void test_render_dec_unsigned_negative(void) {
  /* Same bit pattern, unsigned interpretation -> 4294967295 */
  calc_value_str_t v;
  calc_render_value((int64_t)0xFFFFFFFF, CALC_BASE_DEC, CALC_W32, 0, &v);
  ASSERT_STREQ(v.digits, "4294967295");
  ASSERT_EQ(v.negative, 0);
}

static void test_render_hex(void) {
  calc_value_str_t v;
  calc_render_value(0xDEADBEEF, CALC_BASE_HEX, CALC_W32, 0, &v);
  ASSERT_STREQ(v.digits, "DEADBEEF");
}

static void test_render_oct(void) {
  calc_value_str_t v;
  calc_render_value(0755, CALC_BASE_OCT, CALC_W32, 0, &v);
  ASSERT_STREQ(v.digits, "755");
}

static void test_render_bin_pads_to_width(void) {
  calc_value_str_t v;
  calc_render_value(5, CALC_BASE_BIN, CALC_W8, 0, &v);
  /* width=8, value=5 -> 8 digits, leading zeros */
  ASSERT_STREQ(v.digits, "00000101");
  ASSERT_EQ(v.len, 8);
}

static void test_render_bin_w16(void) {
  calc_value_str_t v;
  calc_render_value(0xFF, CALC_BASE_BIN, CALC_W16, 0, &v);
  ASSERT_STREQ(v.digits, "0000000011111111");
  ASSERT_EQ(v.len, 16);
}

static void test_width_wrap_in_render(void) {
  /* Value of 0x1FF stored, but width=8 means we mask to 0xFF before
   * rendering — DEC sign reads the top bit of the masked value. */
  calc_value_str_t v;
  calc_render_value(0x1FF, CALC_BASE_DEC, CALC_W8, 1, &v);
  ASSERT_STREQ(v.digits, "1");
  ASSERT_EQ(v.negative, 1);
}

/* ── Grouped formatting tests ──────────────────────────────────────────── */

static void test_grouped_dec_short(void) {
  calc_value_str_t v;
  calc_render_value(123, CALC_BASE_DEC, CALC_W32, 1, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "123");
}

static void test_grouped_dec_with_commas(void) {
  calc_value_str_t v;
  calc_render_value(1234567, CALC_BASE_DEC, CALC_W32, 1, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "1,234,567");
}

static void test_grouped_dec_negative(void) {
  calc_value_str_t v;
  calc_render_value((int64_t)-1234, CALC_BASE_DEC, CALC_W32, 1, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "-1,234");
}

static void test_grouped_hex(void) {
  calc_value_str_t v;
  calc_render_value(0xDEADBEEF, CALC_BASE_HEX, CALC_W32, 0, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "0xDEAD_BEEF");
}

static void test_grouped_hex_short(void) {
  calc_value_str_t v;
  calc_render_value(0xFF, CALC_BASE_HEX, CALC_W32, 0, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "0xFF");
}

static void test_grouped_oct(void) {
  calc_value_str_t v;
  calc_render_value(012345670, CALC_BASE_OCT, CALC_W32, 0, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "0o12_345_670");
}

static void test_grouped_bin(void) {
  calc_value_str_t v;
  calc_render_value(0xCC, CALC_BASE_BIN, CALC_W8, 0, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "0b1100_1100");
}

static void test_grouped_bin_w16_padding(void) {
  calc_value_str_t v;
  calc_render_value(5, CALC_BASE_BIN, CALC_W16, 0, &v);
  char buf[32];
  calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_STREQ(buf, "0b0000_0000_0000_0101");
}

static void test_grouped_buffer_truncation(void) {
  calc_value_str_t v;
  calc_render_value(1234567, CALC_BASE_DEC, CALC_W32, 1, &v);
  char buf[5];
  int w = calc_render_grouped(&v, buf, sizeof(buf));
  ASSERT_EQ((long)buf[4], 0L);  /* NUL-terminated */
  ASSERT(w == 4, "wrote 4 bytes (excl. NUL)");
}

/* ── Driver ─────────────────────────────────────────────────────────────── */

int main(void) {
  TEST_GROUP("raw digit rendering");
  RUN_TEST(test_render_dec_zero);
  RUN_TEST(test_render_dec_positive);
  RUN_TEST(test_render_dec_negative_w32);
  RUN_TEST(test_render_dec_unsigned_negative);
  RUN_TEST(test_render_hex);
  RUN_TEST(test_render_oct);
  RUN_TEST(test_render_bin_pads_to_width);
  RUN_TEST(test_render_bin_w16);
  RUN_TEST(test_width_wrap_in_render);

  TEST_GROUP("grouped formatting");
  RUN_TEST(test_grouped_dec_short);
  RUN_TEST(test_grouped_dec_with_commas);
  RUN_TEST(test_grouped_dec_negative);
  RUN_TEST(test_grouped_hex);
  RUN_TEST(test_grouped_hex_short);
  RUN_TEST(test_grouped_oct);
  RUN_TEST(test_grouped_bin);
  RUN_TEST(test_grouped_bin_w16_padding);
  RUN_TEST(test_grouped_buffer_truncation);

  TEST_SUMMARY();
}

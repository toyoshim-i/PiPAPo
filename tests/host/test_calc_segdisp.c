/*
 * test_calc_segdisp.c — unit tests for src/user/calc/calc_segdisp.c
 */

#include "test_framework.h"

#include <string.h>

#include "calc/calc.h"

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

#define ASSERT_STREQ(actual, expected) do {                                 \
    tf_asserts++;                                                            \
    if (!eq((actual), (expected))) {                                         \
        fprintf(stderr, "  FAIL  %-40s  %s:%d  expected [%s], got [%s]\n",  \
                tf_current, __FILE__, __LINE__, (expected), (actual));      \
        tf_fail++;                                                           \
    }                                                                        \
} while (0)

/* ── 7-seg DEC/HEX/OCT ──────────────────────────────────────────────────── */

static void test_seg_dec_single_digit(void) {
  calc_value_str_t v;
  calc_render_value(8, CALC_BASE_DEC, CALC_W8, 1, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, 10, 0, &d);
  ASSERT_EQ(d.line_count, 3);
  ASSERT_EQ(d.used_seg, 1);
  /* Right-aligned in 10 cols: 7 spaces + 3 cols of digit '8' */
  ASSERT_STREQ(d.lines[0], "        _ ");
  ASSERT_STREQ(d.lines[1], "       |_|");
  ASSERT_STREQ(d.lines[2], "       |_|");
}

static void test_seg_negative_dec(void) {
  calc_value_str_t v;
  /* width=W8 signed, value -1 -> bits 0xFF -> negative=1, digits="1" */
  calc_render_value((int64_t)0xFF, CALC_BASE_DEC, CALC_W8, 1, &v);
  ASSERT_EQ(v.negative, 1);
  ASSERT_EQ(v.len, 1);

  calc_disp_t d;
  calc_segdisp_render(&v, 10, 0, &d);
  ASSERT_EQ(d.line_count, 3);
  ASSERT_EQ(d.used_seg, 1);
  /* '-' glyph (3 cols) then '1' glyph (3 cols) = 6 content cols,
   * 4 leading spaces to right-align in 10 cols.
   * '-' glyph rows: "   ", " _ ", "   " ; '1' rows: "   ", "  |", "  |" */
  ASSERT_STREQ(d.lines[0], "          ");        /* all 10 spaces */
  ASSERT_STREQ(d.lines[1], "     _   |");        /* '-' middle " _ " then '1' middle "  |" */
  ASSERT_STREQ(d.lines[2], "         |");        /* '-' bottom "   " then '1' bottom "  |" */
}

static void test_seg_hex_with_letters(void) {
  calc_value_str_t v;
  calc_render_value(0xAB, CALC_BASE_HEX, CALC_W8, 0, &v);
  ASSERT_STREQ(v.digits, "AB");
  calc_disp_t d;
  calc_segdisp_render(&v, 10, 0, &d);
  ASSERT_EQ(d.line_count, 3);
  ASSERT_EQ(d.used_seg, 1);
  /* 'A' glyph: " _ " "|_|" "| |"
   * 'b' glyph: "   " "|_ " "|_|"
   * Concatenated, right-aligned in 10 cols (4 leading spaces): */
  ASSERT_STREQ(d.lines[0], "     _    ");
  ASSERT_STREQ(d.lines[1], "    |_||_ ");
  ASSERT_STREQ(d.lines[2], "    | ||_|");
}

static void test_seg_falls_back_when_too_wide(void) {
  calc_value_str_t v;
  /* 10 digits in DEC (W32 max) needs 30 cols -- fits 30, doesn't fit 20 */
  calc_render_value(1234567890, CALC_BASE_DEC, CALC_W32, 0, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, 20, 0, &d);
  ASSERT_EQ(d.line_count, 1);
  ASSERT_EQ(d.used_seg, 0);
  /* Plain text: "1,234,567,890" = 13 chars, right-aligned in 20 cols */
}

static void test_force_text(void) {
  calc_value_str_t v;
  calc_render_value(8, CALC_BASE_DEC, CALC_W8, 1, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, 40, 1, &d);  /* force_text=1 */
  ASSERT_EQ(d.line_count, 1);
  ASSERT_EQ(d.used_seg, 0);
}

/* ── BIN LED-dot row ────────────────────────────────────────────────────── */

static void test_led_w8(void) {
  calc_value_str_t v;
  calc_render_value(0xCC, CALC_BASE_BIN, CALC_W8, 0, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, 20, 0, &d);
  ASSERT_EQ(d.line_count, 1);
  ASSERT_EQ(d.used_seg, 1);
  /* 0xCC = 1100 1100 -> "**.. **.." padded right in 20 cols */
  /* content width: 8 bits + 1 space gap = 9 cells */
  /* 11 leading spaces, then "**.. **.." */
  ASSERT_STREQ(d.lines[0], "           **.. **..");
}

static void test_led_w16_zero(void) {
  calc_value_str_t v;
  calc_render_value(0, CALC_BASE_BIN, CALC_W16, 0, &v);
  calc_disp_t d;
  calc_segdisp_render(&v, 30, 0, &d);
  ASSERT_EQ(d.line_count, 1);
  /* 16 bits + 3 gaps = 19 cells: ".... .... .... ...." */
  /* 11 leading spaces */
  ASSERT_STREQ(d.lines[0], "           .... .... .... ....");
}

static void test_led_falls_back_when_too_narrow(void) {
  calc_value_str_t v;
  calc_render_value(0xFFFFFFFF, CALC_BASE_BIN, CALC_W32, 0, &v);
  calc_disp_t d;
  /* 32 bits + 7 gaps = 39 cells: doesn't fit 20 cols */
  calc_segdisp_render(&v, 20, 0, &d);
  ASSERT_EQ(d.line_count, 1);
  ASSERT_EQ(d.used_seg, 0);  /* fell back to plain text */
}

/* ── Driver ─────────────────────────────────────────────────────────────── */

int main(void) {
  TEST_GROUP("7-seg DEC/HEX");
  RUN_TEST(test_seg_dec_single_digit);
  RUN_TEST(test_seg_negative_dec);
  RUN_TEST(test_seg_hex_with_letters);
  RUN_TEST(test_seg_falls_back_when_too_wide);
  RUN_TEST(test_force_text);

  TEST_GROUP("BIN LED dots");
  RUN_TEST(test_led_w8);
  RUN_TEST(test_led_w16_zero);
  RUN_TEST(test_led_falls_back_when_too_narrow);

  TEST_SUMMARY();
}

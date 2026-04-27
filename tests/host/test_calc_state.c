/*
 * test_calc_state.c — unit tests for src/user/calc/calc_state.c
 *
 * The state machine is pure C with no syscalls, so it builds cleanly on
 * the host with no stubs.  We pull in calc_state.c directly via the
 * CMake list rather than reproducing its API by hand.
 */

#include "test_framework.h"

#include <string.h>

#include "calc/calc.h"

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void enter_digits(calc_state_t *s, const char *digits) {
  for (const char *p = digits; *p; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'A' && *p <= 'F') d = 10 + (*p - 'A');
    else if (*p >= 'a' && *p <= 'f') d = 10 + (*p - 'a');
    else continue;
    calc_input_digit(s, d);
  }
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void test_init_defaults(void) {
  calc_state_t s;
  calc_init(&s);
  ASSERT_EQ(s.display, 0);
  ASSERT_EQ(s.accum, 0);
  ASSERT_EQ(s.ans, 0);
  ASSERT_EQ(s.pending, CALC_OP_NONE);
  ASSERT_EQ(s.entry, 0);
  ASSERT_EQ(s.base, CALC_BASE_DEC);
  ASSERT_EQ((int)s.width, (int)CALC_W32);
  ASSERT_EQ(s.sign, 1);
  ASSERT_EQ(s.err, CALC_ERR_NONE);
}

static void test_digit_entry_dec(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "1234");
  ASSERT_EQ(s.display, 1234);
  ASSERT_EQ(s.entry, 1);
}

static void test_dec_rejects_hex_digits(void) {
  calc_state_t s;
  calc_init(&s);
  calc_input_digit(&s, 12);  /* 'C' */
  ASSERT_EQ(s.display, 0);
  ASSERT_EQ(s.entry, 0);
}

static void test_bin_rejects_above_1(void) {
  calc_state_t s;
  calc_init(&s);
  calc_set_base(&s, CALC_BASE_BIN);
  calc_input_digit(&s, 1);
  calc_input_digit(&s, 0);
  calc_input_digit(&s, 1);
  ASSERT_EQ(s.display, 0b101);
  calc_input_digit(&s, 2);  /* invalid: '2' in BIN */
  ASSERT_EQ(s.display, 0b101);
}

static void test_oct_rejects_above_7(void) {
  calc_state_t s;
  calc_init(&s);
  calc_set_base(&s, CALC_BASE_OCT);
  calc_input_digit(&s, 7);
  calc_input_digit(&s, 8);  /* invalid */
  ASSERT_EQ(s.display, 7);
}

static void test_basic_add(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "12");
  calc_input_op(&s, CALC_OP_ADD);
  enter_digits(&s, "34");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 46);
  ASSERT_EQ(s.ans, 46);
  ASSERT_EQ(s.pending, CALC_OP_NONE);
}

static void test_chained_ops_left_to_right(void) {
  /* No precedence — like a real calc: 2 + 3 * 4 = (2+3)*4 = 20 */
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "2");
  calc_input_op(&s, CALC_OP_ADD);
  enter_digits(&s, "3");
  calc_input_op(&s, CALC_OP_MUL);  /* commits 2+3 = 5 */
  ASSERT_EQ(s.display, 5);
  enter_digits(&s, "4");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 20);
}

static void test_width_wrap_add(void) {
  calc_state_t s;
  calc_init(&s);
  /* width=8, 200+200=400=0x190; masked to 8 bits = 0x90 = 144 */
  s.width = CALC_W8;
  enter_digits(&s, "200");
  ASSERT_EQ(s.display, 200);
  calc_input_op(&s, CALC_OP_ADD);
  enter_digits(&s, "200");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 144);
}

static void test_negate_w8(void) {
  /* 5 negate at width=8 -> 0xFB = 251 (or -5 signed) */
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W8;
  enter_digits(&s, "5");
  calc_input_negate(&s);
  ASSERT_EQ((unsigned long)s.display & 0xFF, 0xFBu);
}

static void test_bitwise_and(void) {
  calc_state_t s;
  calc_init(&s);
  calc_set_base(&s, CALC_BASE_HEX);
  enter_digits(&s, "FF");
  calc_input_op(&s, CALC_OP_AND);
  enter_digits(&s, "0F");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 0x0F);
}

static void test_shift_left_clamp(void) {
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W8;
  enter_digits(&s, "1");
  calc_input_op(&s, CALC_OP_SHL);
  enter_digits(&s, "7");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 0x80);

  /* shift count == width clamps to 0 */
  calc_init(&s);
  s.width = CALC_W8;
  enter_digits(&s, "1");
  calc_input_op(&s, CALC_OP_SHL);
  enter_digits(&s, "8");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 0);
}

static void test_shift_right_signed(void) {
  /* width=8 signed, value 0xFF (-1), >> 1 should give 0xFF (-1, arithmetic) */
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W8;
  s.sign = 1;
  s.display = 0xFF;
  s.entry = 1;
  calc_input_op(&s, CALC_OP_SHR);
  enter_digits(&s, "1");
  calc_input_equals(&s);
  ASSERT_EQ((unsigned long)s.display & 0xFF, 0xFFu);

  /* same value, unsigned: 0xFF >> 1 = 0x7F */
  calc_init(&s);
  s.width = CALC_W8;
  s.sign = 0;
  s.display = 0xFF;
  s.entry = 1;
  calc_input_op(&s, CALC_OP_SHR);
  enter_digits(&s, "1");
  calc_input_equals(&s);
  ASSERT_EQ(s.display, 0x7F);
}

static void test_divide_by_zero_sets_err(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "10");
  calc_input_op(&s, CALC_OP_DIV);
  enter_digits(&s, "0");
  calc_input_equals(&s);
  ASSERT_EQ(s.err, CALC_ERR_DIV0);
  ASSERT_EQ(s.display, 0);
}

static void test_clear_entry_clears_err(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "5");
  calc_input_op(&s, CALC_OP_DIV);
  enter_digits(&s, "0");
  calc_input_equals(&s);
  ASSERT_EQ(s.err, CALC_ERR_DIV0);
  calc_input_clear_entry(&s);
  ASSERT_EQ(s.err, CALC_ERR_NONE);
  ASSERT_EQ(s.display, 0);
}

static void test_base_switch_preserves_value(void) {
  calc_state_t s;
  calc_init(&s);
  calc_set_base(&s, CALC_BASE_HEX);
  enter_digits(&s, "FF");
  calc_set_base(&s, CALC_BASE_DEC);
  ASSERT_EQ(s.display, 255);
  calc_set_base(&s, CALC_BASE_BIN);
  ASSERT_EQ(s.display, 255);  /* same bit pattern */
}

static void test_backspace_dec(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "1234");
  calc_input_backspace(&s);
  ASSERT_EQ(s.display, 123);
  calc_input_backspace(&s);
  ASSERT_EQ(s.display, 12);
}

static void test_backspace_hex(void) {
  calc_state_t s;
  calc_init(&s);
  calc_set_base(&s, CALC_BASE_HEX);
  enter_digits(&s, "DEAD");
  calc_input_backspace(&s);
  ASSERT_EQ(s.display, 0xDEA);
}

static void test_ac_clears_all_but_prefs(void) {
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W16;
  s.base = CALC_BASE_HEX;
  enter_digits(&s, "AB");
  calc_input_op(&s, CALC_OP_ADD);
  enter_digits(&s, "12");
  calc_input_all_clear(&s);
  ASSERT_EQ(s.display, 0);
  ASSERT_EQ(s.accum, 0);
  ASSERT_EQ(s.pending, CALC_OP_NONE);
  ASSERT_EQ((int)s.width, (int)CALC_W16);  /* preserved */
  ASSERT_EQ((int)s.base, (int)CALC_BASE_HEX);
}

static void test_width_cycle(void) {
  calc_state_t s;
  calc_init(&s);
  ASSERT_EQ((int)s.width, (int)CALC_W32);
  calc_cycle_width(&s);
  ASSERT_EQ((int)s.width, (int)CALC_W64);
  calc_cycle_width(&s);
  ASSERT_EQ((int)s.width, (int)CALC_W8);
  calc_cycle_width(&s);
  ASSERT_EQ((int)s.width, (int)CALC_W16);
  calc_cycle_width(&s);
  ASSERT_EQ((int)s.width, (int)CALC_W32);
}

static void test_width_down_masks_value(void) {
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W32;
  s.display = 0x1FF;
  s.entry = 1;
  /* cycle 32 -> 64 -> 8 */
  calc_cycle_width(&s); /* W64 */
  ASSERT_EQ(s.display, 0x1FF);
  calc_cycle_width(&s); /* W8 */
  ASSERT_EQ(s.display, 0xFF);
}

static void test_signed_div_w8(void) {
  /* width=8 signed: -10 / 3 = -3 (truncation toward zero) */
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W8;
  s.sign  = 1;
  enter_digits(&s, "10");
  calc_input_negate(&s);  /* display = 0xF6 = -10 */
  calc_input_op(&s, CALC_OP_DIV);
  enter_digits(&s, "3");
  calc_input_equals(&s);
  /* -10 / 3 = -3 -> 0xFD */
  ASSERT_EQ((unsigned long)s.display & 0xFF, 0xFDu);
}

/* ── Memory register tests ─────────────────────────────────────────────── */

static void test_mem_initial_zero(void) {
  calc_state_t s;
  calc_init(&s);
  ASSERT_EQ(s.mem, 0);
}

static void test_mem_store_and_recall(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "42");
  calc_mem_store(&s);
  ASSERT_EQ(s.mem, 42);
  calc_input_all_clear(&s);
  ASSERT_EQ(s.display, 0);
  calc_mem_recall(&s);
  ASSERT_EQ(s.display, 42);
  ASSERT_EQ(s.mem, 42);  /* recall does not consume */
}

static void test_mem_add_sub(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "10");
  calc_mem_add(&s);          /* mem = 10 */
  ASSERT_EQ(s.mem, 10);
  calc_input_clear_entry(&s);
  enter_digits(&s, "3");
  calc_mem_add(&s);          /* mem = 13 */
  ASSERT_EQ(s.mem, 13);
  calc_input_clear_entry(&s);
  enter_digits(&s, "5");
  calc_mem_sub(&s);          /* mem = 8 */
  ASSERT_EQ(s.mem, 8);
}

static void test_mem_clear(void) {
  calc_state_t s;
  calc_init(&s);
  enter_digits(&s, "99");
  calc_mem_store(&s);
  ASSERT_EQ(s.mem, 99);
  calc_mem_clear(&s);
  ASSERT_EQ(s.mem, 0);
}

static void test_mem_width_mask(void) {
  /* width=8: mem = 200; M+ with display=200 -> mem wraps to 144 */
  calc_state_t s;
  calc_init(&s);
  s.width = CALC_W8;
  enter_digits(&s, "200");
  calc_mem_add(&s);          /* mem = 200 */
  ASSERT_EQ(s.mem, 200);
  calc_input_clear_entry(&s);
  enter_digits(&s, "200");
  calc_mem_add(&s);          /* (200 + 200) & 0xFF = 144 */
  ASSERT_EQ((unsigned long)s.mem & 0xFFu, 144u);
}

/* ── Driver ─────────────────────────────────────────────────────────────── */

int main(void) {
  TEST_GROUP("init / digit entry");
  RUN_TEST(test_init_defaults);
  RUN_TEST(test_digit_entry_dec);
  RUN_TEST(test_dec_rejects_hex_digits);
  RUN_TEST(test_bin_rejects_above_1);
  RUN_TEST(test_oct_rejects_above_7);

  TEST_GROUP("arithmetic");
  RUN_TEST(test_basic_add);
  RUN_TEST(test_chained_ops_left_to_right);
  RUN_TEST(test_width_wrap_add);
  RUN_TEST(test_signed_div_w8);

  TEST_GROUP("bitwise / shifts / negate / not");
  RUN_TEST(test_negate_w8);
  RUN_TEST(test_bitwise_and);
  RUN_TEST(test_shift_left_clamp);
  RUN_TEST(test_shift_right_signed);

  TEST_GROUP("error handling");
  RUN_TEST(test_divide_by_zero_sets_err);
  RUN_TEST(test_clear_entry_clears_err);

  TEST_GROUP("mode + clear + width");
  RUN_TEST(test_base_switch_preserves_value);
  RUN_TEST(test_backspace_dec);
  RUN_TEST(test_backspace_hex);
  RUN_TEST(test_ac_clears_all_but_prefs);
  RUN_TEST(test_width_cycle);
  RUN_TEST(test_width_down_masks_value);

  TEST_GROUP("memory register");
  RUN_TEST(test_mem_initial_zero);
  RUN_TEST(test_mem_store_and_recall);
  RUN_TEST(test_mem_add_sub);
  RUN_TEST(test_mem_clear);
  RUN_TEST(test_mem_width_mask);

  TEST_SUMMARY();
}

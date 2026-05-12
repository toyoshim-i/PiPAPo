/*
 * cc_kbd.c — M5Stack CardComputer keyboard matrix-scan driver
 *
 * Algorithm (per docs/reference/cardcomputer.md §Keyboard):
 *   for cnt in 0..7:
 *     drive (gpio8, gpio9, gpio11) = low 3 bits of cnt (LSB→MSB)
 *     read 7 INPUT_PULLUP lines; each low-going bit = pressed key
 *     decode (cnt, input_idx) → (logical_y, logical_x) via X_map_chart
 *
 * Edge-only delivery: a key has to transition from "not pressed" to
 * "pressed" between two scans to produce one event.  Held keys do
 * not auto-repeat (the kernel line discipline / shell handles that).
 *
 * Multi-byte VT100 escape sequences are emitted one byte per
 * kbd_poll() call via an internal cursor — same shape as
 * src/kernel/vfs/driver/i2c_kbd.c on pico1calc.
 */

#include "kernel/vfs/driver/cc_kbd.h"

#include <driver/gpio.h>
#include <stdint.h>
#include <string.h>

#include "kernel/common/xtensa_cc.h"
#include "kernel/vfs/driver/cc_keymap.h"

/* ── Pin tables (driven by xtensa_cc.h) ────────────────────────────────── */

static const int kbd_out_pins[KBD_NUM_OUT] = KBD_OUT_PINS;
static const int kbd_in_pins[KBD_NUM_IN] = KBD_IN_PINS;

/* X_map_chart from M5Cardputer's KeyboardReader/IOMatrix.h.  Each input
 * pin maps to a different pair of logical_x columns depending on whether
 * the counter is in its lower half (use x_2) or upper half (use x_1). */
typedef struct {
  uint8_t mask;
  uint8_t x_1;
  uint8_t x_2;
} x_map_t;

static const x_map_t x_map_chart[KBD_NUM_IN] = {
    {1, 0, 1},  {2, 2, 3},    {4, 4, 5},    {8, 6, 7},
    {16, 8, 9}, {32, 10, 11}, {64, 12, 13},
};

/* ── Pressed-key bitmap (4 rows × 14 cols = 56 bits) ───────────────────── */

#define CC_KEY_BIT(y, x) ((uint64_t)1 << ((unsigned)(y) * 14u + (unsigned)(x)))

#define MOD_FN_BIT CC_KEY_BIT(CC_KEY_FN_Y, CC_KEY_FN_X)
#define MOD_SHIFT_BIT CC_KEY_BIT(CC_KEY_SHIFT_Y, CC_KEY_SHIFT_X)
#define MOD_CTRL_BIT CC_KEY_BIT(CC_KEY_CTRL_Y, CC_KEY_CTRL_X)
#define MOD_ALL_BITS (MOD_FN_BIT | MOD_SHIFT_BIT | MOD_CTRL_BIT)

/* ── Driver state ──────────────────────────────────────────────────────── *
 *
 * `prev_pressed`  bits we have already delivered an event for and which
 *                 are still being held down.  Cleared bit-by-bit as keys
 *                 are released so that re-pressing the same key produces
 *                 a fresh edge.  Updated by both kbd_poll_avail() (clears
 *                 released bits) and kbd_poll() (sets the bit it just
 *                 emitted an event for).
 * `seq_buf`       output byte queue.  Single-byte resolutions land in
 *                 seq_buf[0]; multi-byte escape sequences are copied in
 *                 whole.  Always NUL-terminated.
 * `seq_pos`       read cursor into seq_buf; next byte is seq_buf[seq_pos].
 *
 * No `static char return_buf[2]` style scratch in the syscall path —
 * everything lives in this single state block.
 */

#define SEQ_BUF_SIZE 8u /* longest sequence today is "\x1b[3~" = 5 incl NUL */

static uint64_t prev_pressed;
static char seq_buf[SEQ_BUF_SIZE];
static unsigned seq_pos;

/* ── GPIO sweep ────────────────────────────────────────────────────────── */

static uint64_t scan_matrix(void) {
  uint64_t pressed = 0;
  for (unsigned cnt = 0; cnt < 8u; cnt++) {
    /* Drive the 3-bit counter; LSB → kbd_out_pins[0]. */
    gpio_set_level(kbd_out_pins[0], (cnt >> 0) & 1u);
    gpio_set_level(kbd_out_pins[1], (cnt >> 1) & 1u);
    gpio_set_level(kbd_out_pins[2], (cnt >> 2) & 1u);
    /* No explicit settle delay: the gpio_set_level / gpio_get_level
     * call sequence already burns more cycles than the demux + RC
     * settle requires at 240 MHz. */

    unsigned logical_y = (cnt > 3u) ? cnt - 4u : cnt;
    logical_y = 3u - logical_y; /* flip so y=0 is the top (number) row */

    for (unsigned i = 0; i < KBD_NUM_IN; i++) {
      if (gpio_get_level(kbd_in_pins[i]) == 0) {
        unsigned logical_x =
            (cnt > 3u) ? x_map_chart[i].x_1 : x_map_chart[i].x_2;
        pressed |= CC_KEY_BIT(logical_y, logical_x);
      }
    }
  }
  return pressed;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void kbd_init(void) {
  /* Configure the 3 counter outputs. */
  gpio_config_t out_cfg = {
      .pin_bit_mask = (1ULL << kbd_out_pins[0]) | (1ULL << kbd_out_pins[1]) |
                      (1ULL << kbd_out_pins[2]),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  (void)gpio_config(&out_cfg);

  /* Configure the 7 inputs as INPUT_PULLUP, idle-high. */
  uint64_t in_mask = 0;
  for (unsigned i = 0; i < KBD_NUM_IN; i++) in_mask |= 1ULL << kbd_in_pins[i];
  gpio_config_t in_cfg = {
      .pin_bit_mask = in_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  (void)gpio_config(&in_cfg);

  prev_pressed = 0;
  seq_buf[0] = '\0';
  seq_pos = 0;
}

int kbd_poll_avail(void) {
  /* Pending byte in the escape-sequence queue? */
  if (seq_buf[seq_pos] != '\0') return 1;

  /* Otherwise scan the matrix and see if any non-modifier key is
   * newly pressed.  Crucially, clear released bits from prev_pressed
   * here too — without this step, a phantom press at kbd_init (while
   * INPUT_PULLUP lines settle) gets latched into prev_pressed and the
   * key "never works" because every subsequent press is suppressed
   * as already-delivered.  kbd_poll only runs when avail returns 1,
   * so if avail does not maintain the release bookkeeping nothing
   * else will. */
  uint64_t cur = scan_matrix();
  prev_pressed &= cur;
  uint64_t newly = (cur & ~prev_pressed) & ~MOD_ALL_BITS;
  return newly != 0;
}

int kbd_poll(void) {
  /* 1. If the escape-sequence queue still has bytes, return the next. */
  if (seq_buf[seq_pos] != '\0') return (unsigned char)seq_buf[seq_pos++];
  seq_buf[0] = '\0';
  seq_pos = 0;

  /* 2. Scan the matrix; isolate newly-pressed non-modifier keys. */
  uint64_t cur = scan_matrix();
  uint64_t mods = cur & MOD_ALL_BITS;
  prev_pressed &= cur; /* release bookkeeping */
  uint64_t newly = (cur & ~prev_pressed) & ~MOD_ALL_BITS;
  if (newly == 0) return -1;

  /* 3. Pick the lowest-numbered newly-pressed key (deterministic when
   *    multiple keys edge in the same scan cycle).  Mark only that
   *    one bit as delivered — other simultaneously-pressed keys stay
   *    out of prev_pressed and will fire on the next poll cycle. */
  unsigned bit = (unsigned)__builtin_ctzll(newly);
  prev_pressed |= ((uint64_t)1 << bit);
  unsigned y = bit / 14u;
  unsigned x = bit % 14u;

  /* 4. Fn layer — if Fn is held and a mapping exists, emit the
   *    multi-byte escape sequence. */
  if (mods & MOD_FN_BIT) {
    const char *fn_seq = cc_fn_lookup((int)y, (int)x);
    if (fn_seq != (const char *)0) {
      size_t n = strlen(fn_seq);
      if (n >= SEQ_BUF_SIZE) n = SEQ_BUF_SIZE - 1u;
      memcpy(seq_buf, fn_seq, n);
      seq_buf[n] = '\0';
      seq_pos = 1;
      return (unsigned char)seq_buf[0];
    }
    /* No Fn mapping for this key — fall through to base resolution. */
  }

  /* 5. Base / Shift table lookup. */
  uint8_t ch =
      (mods & MOD_SHIFT_BIT) ? cc_keymap[y][x].shift : cc_keymap[y][x].base;

  /* 6. Ctrl combines with letter keys into 0x01..0x1A control codes. */
  if (mods & MOD_CTRL_BIT) {
    if (ch >= 'a' && ch <= 'z')
      ch = (uint8_t)(ch - 'a' + 1);
    else if (ch >= 'A' && ch <= 'Z')
      ch = (uint8_t)(ch - 'A' + 1);
  }
  return (int)ch;
}

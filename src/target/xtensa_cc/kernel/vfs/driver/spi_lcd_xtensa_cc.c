/*
 * spi_lcd_xtensa_cc.c — SPI2 LCD transport for the M5Stack CardComputer
 *
 * Implements the spi_lcd.h contract on top of ESP-IDF's `spi_master`
 * driver.  Targets the ST7789V2 panel wired to SPI2 (MOSI=35, SCK=36,
 * CS=37, DC=34, RST=33) — pin numbers come from xtensa_cc.h.
 *
 * Per CC-4b: this is the first-cut implementation.  CC-3.5e tracks
 * the eventual lowering to bare-MMIO at the GPSPI2 register block
 * (0x60024000), at which point the DMA scratch heap allocation here
 * goes away.
 *
 * No MISO line is wired (TX-only panel).  All transactions are
 * sent via spi_device_polling_transmit() so the implementation does
 * not depend on the FreeRTOS scheduler — which xtensa_cc explicitly
 * holds disabled (port_xSchedulerRunning[0] forced to 0).  The
 * pre-callback writes the DC pin from spi_transaction_t.user before
 * each transfer.
 *
 * Endianness: the panel takes RGB565 as big-endian bytes, but ESP-IDF
 * spi_master sends bytes in array order.  spi_lcd_data16 / fill /
 * stream therefore copy each pixel into a byte-swapped DMA scratch
 * before queuing the transfer.
 */

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <stdint.h>
#include <string.h>

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/spi_lcd.h"
#include "target/xtensa_cc/kernel/core/xtensa_cc.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_HZ (40 * 1000 * 1000) /* 40 MHz — well within ST7789V2 spec */

/* DMA scratch sized to hold one 240-pixel scanline (RGB565) plus slack;
 * fill / data16 paths chunk through this buffer. */
#define LCD_SCRATCH_BYTES 1024u

/* DC pin polarity passed via spi_transaction_t.user — DC=0 selects
 * command, DC=1 selects data. */
#define DC_CMD ((void *)(uintptr_t)0)
#define DC_DATA ((void *)(uintptr_t)1)

/* ── State ──────────────────────────────────────────────────────────────── */

static spi_device_handle_t lcd_spi;
static uint8_t *lcd_scratch; /* DMA-capable, MALLOC_CAP_DMA */
static int lcd_ok;           /* 1 once init has succeeded */

/* ── Busy-wait delays ───────────────────────────────────────────────────── *
 *
 * FreeRTOS scheduler is disabled on this target (vTaskDelay would not
 * yield), and esp_rom_delay_us is avoided to keep the bring-up free of
 * any ROM-routed call (some ROM helpers issue `syscall 0`, which
 * crashes in kernel mode where KernelExceptionVector does not handle
 * it — see usj.c for the same rationale).  At PPAP_SYS_HZ = 240 MHz a
 * volatile loop iteration is ~1-2 cycles, so the multiplier below is
 * deliberately conservative; the panel reset timings (10 ms / 120 ms)
 * are minimums, longer is safe.
 */

#define DELAY_LOOPS_PER_MS 240000u

static void delay_ms(uint32_t ms) {
  volatile uint32_t count = ms * DELAY_LOOPS_PER_MS;
  while (count--);
}

/* ── DC pre-callback ───────────────────────────────────────────────────── *
 *
 * spi_master invokes this immediately before clocking out the first
 * bit of each transaction.  IRAM_ATTR because queued (non-polling)
 * transmissions would dispatch this from the SPI ISR; we use polling
 * exclusively today, but the attribute costs nothing and matches the
 * canonical ESP-IDF idiom.
 */
static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t) {
  gpio_set_level(DISPLAY_DC_PIN, (int)(uintptr_t)t->user);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int spi_lcd_ok(void) { return lcd_ok; }

uint32_t spi_lcd_read_sr(void) {
  /* Status-register passthrough doesn't translate to spi_master; the
   * arm_m PL022 driver exposes it for SPI bring-up debugging only. */
  return 0;
}

void spi_lcd_init(void) {
  /* 1. DC and RST as plain GPIO outputs (CS is owned by the SPI peripheral). */
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << DISPLAY_DC_PIN) | (1ULL << DISPLAY_RST_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&io) != ESP_OK) {
    mod_vfs.klogf("LCD: GPIO config failed\n");
    return;
  }
  gpio_set_level(DISPLAY_DC_PIN, 1);
  gpio_set_level(DISPLAY_RST_PIN, 1);

  /* 2. SPI2 bus.  No MISO (TX-only panel).  max_transfer_sz caps the
   *    largest single transaction; our chunking respects LCD_SCRATCH_BYTES. */
  spi_bus_config_t buscfg = {
      .miso_io_num = -1,
      .mosi_io_num = DISPLAY_MOSI_PIN,
      .sclk_io_num = DISPLAY_SCK_PIN,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LCD_SCRATCH_BYTES,
  };
  esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    mod_vfs.klogf("LCD: SPI bus init failed (0x%x)\n", (unsigned)err);
    return;
  }

  /* 3. ST7789V2 device on the bus. */
  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = LCD_SPI_HZ,
      .mode = 0, /* CPOL=0 CPHA=0 */
      .spics_io_num = DISPLAY_CS_PIN,
      .queue_size = 1,
      .pre_cb = lcd_pre_cb,
  };
  err = spi_bus_add_device(LCD_SPI_HOST, &devcfg, &lcd_spi);
  if (err != ESP_OK) {
    mod_vfs.klogf("LCD: SPI device add failed (0x%x)\n", (unsigned)err);
    return;
  }

  /* 4. DMA-capable scratch for byte-swap and fill paths.  Allocated
   *    once; freed never (kernel lifetime).  Tracked under CC-3.5a
   *    until the bare-MMIO transport replaces this allocation. */
  lcd_scratch =
      heap_caps_malloc(LCD_SCRATCH_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!lcd_scratch) {
    mod_vfs.klogf("LCD: DMA scratch alloc failed\n");
    return;
  }

  lcd_ok = 1;
}

void spi_lcd_reset(void) {
  if (!lcd_ok) return;
  gpio_set_level(DISPLAY_RST_PIN, 0);
  delay_ms(10);
  gpio_set_level(DISPLAY_RST_PIN, 1);
  delay_ms(120);
}

void spi_lcd_cmd(uint8_t cmd) {
  if (!lcd_ok) return;
  spi_transaction_t t = {
      .length = 8,
      .flags = SPI_TRANS_USE_TXDATA,
      .user = DC_CMD,
  };
  t.tx_data[0] = cmd;
  (void)spi_device_polling_transmit(lcd_spi, &t);
}

void spi_lcd_data(const uint8_t *buf, size_t len) {
  if (!lcd_ok || len == 0) return;

  /* Small payloads (typical for register writes) ride in the inline
   * tx_data[] without touching the scratch buffer. */
  if (len <= 4) {
    spi_transaction_t t = {
        .length = len * 8u,
        .flags = SPI_TRANS_USE_TXDATA,
        .user = DC_DATA,
    };
    memcpy(t.tx_data, buf, len);
    (void)spi_device_polling_transmit(lcd_spi, &t);
    return;
  }

  /* Larger payloads chunk through the DMA scratch.  Caller buffers
   * may live in flash / non-DMA memory, so the copy is unavoidable
   * until the bare-MMIO transport lands. */
  size_t off = 0;
  while (off < len) {
    size_t chunk = len - off;
    if (chunk > LCD_SCRATCH_BYTES) chunk = LCD_SCRATCH_BYTES;
    memcpy(lcd_scratch, buf + off, chunk);
    spi_transaction_t t = {
        .length = chunk * 8u,
        .tx_buffer = lcd_scratch,
        .user = DC_DATA,
    };
    (void)spi_device_polling_transmit(lcd_spi, &t);
    off += chunk;
  }
}

void spi_lcd_data16(const uint16_t *buf, size_t count) {
  if (!lcd_ok || count == 0) return;
  const size_t pixels_per_chunk = LCD_SCRATCH_BYTES / 2u;
  size_t i = 0;
  while (i < count) {
    size_t chunk = count - i;
    if (chunk > pixels_per_chunk) chunk = pixels_per_chunk;
    /* Byte-swap into scratch: panel takes big-endian RGB565. */
    for (size_t j = 0; j < chunk; j++) {
      uint16_t v = buf[i + j];
      lcd_scratch[j * 2u] = (uint8_t)(v >> 8);
      lcd_scratch[j * 2u + 1u] = (uint8_t)(v & 0xFFu);
    }
    spi_transaction_t t = {
        .length = chunk * 16u,
        .tx_buffer = lcd_scratch,
        .user = DC_DATA,
    };
    (void)spi_device_polling_transmit(lcd_spi, &t);
    i += chunk;
  }
}

/* Streaming API: the PL022 implementation keeps CS asserted across
 * data16_stream calls to avoid per-call CS toggling overhead.  ESP-IDF
 * spi_master manages CS per-transaction via the peripheral, so the
 * first-cut here treats stream_begin/end as no-ops and routes
 * data16_stream straight through data16.  Performance impact: one
 * CS toggle per fbcon row flush instead of one per panel update.
 * Acceptable for bring-up; revisit if flush latency hurts. */

void spi_lcd_stream_begin(void) { /* no-op */ }

void spi_lcd_data16_stream(const uint16_t *buf, size_t count) {
  spi_lcd_data16(buf, count);
}

void spi_lcd_stream_end(void) { /* no-op */ }

void spi_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t col[4] = {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFFu),
                    (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFFu)};
  uint8_t row[4] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFFu),
                    (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFFu)};
  spi_lcd_cmd(0x2A); /* CASET — Column Address Set */
  spi_lcd_data(col, 4);
  spi_lcd_cmd(0x2B); /* RASET — Row Address Set */
  spi_lcd_data(row, 4);
  spi_lcd_cmd(0x2C); /* RAMWR — Memory Write */
}

void spi_lcd_fill(uint16_t color, uint32_t count) {
  if (!lcd_ok || count == 0) return;

  /* Pre-fill scratch with the repeating big-endian pixel pattern. */
  size_t pixels_per_chunk = LCD_SCRATCH_BYTES / 2u;
  if ((uint32_t)pixels_per_chunk > count) pixels_per_chunk = count;
  uint8_t hi = (uint8_t)(color >> 8);
  uint8_t lo = (uint8_t)(color & 0xFFu);
  for (size_t j = 0; j < pixels_per_chunk; j++) {
    lcd_scratch[j * 2u] = hi;
    lcd_scratch[j * 2u + 1u] = lo;
  }

  while (count > 0) {
    uint32_t chunk = (count > (uint32_t)pixels_per_chunk)
                         ? (uint32_t)pixels_per_chunk
                         : count;
    spi_transaction_t t = {
        .length = chunk * 16u,
        .tx_buffer = lcd_scratch,
        .user = DC_DATA,
    };
    (void)spi_device_polling_transmit(lcd_spi, &t);
    count -= chunk;
  }
}

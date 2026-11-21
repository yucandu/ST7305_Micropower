#include "clock_port.h"
/*
 * ---------------------------------------------------------------------------
 * ST7305 2.13" 122 x 250 driver (STM32U375, HAL, SPI1 + GPDMA)
 * ---------------------------------------------------------------------------
 * Port of the 168x384 driver. What changed and why:
 *
 *  - g_framebuffer now IS the panel's wire format. The 168x384 code kept a
 *    u8g2-style tile buffer and ran a map1/map2 conversion pass (~8 KB) before
 *    every SPI transfer. The 122x250 Arduino library (ST7305.cpp) packs pixels
 *    straight into the on-the-wire layout instead, so fb_set_pixel() does the
 *    same and st7305_refresh() just DMAs the buffer out. That removes the
 *    second 8 KB buffer, the convert step, and the pre-rendered glyph tables.
 *
 *  - Pixel packing (native portrait coords nx 0..121, ny 0..249):
 *        col      = nx + 10                          (PANEL_X_OFFSET)
 *        byte     = (ny / 2) * 33 + col / 4
 *        bit      = 7 - ((col % 4) * 2 + (ny % 2))
 *    and the landscape canvas is mapped with one rotation:
 *        logical (x, y) -> native (121 - y, x)
 *    (identical to ST7305::setPixel()/setNativePixel() in the Arduino lib).
 *
 *  - Window registers: CASET = 0x19 + 12-px block, RASET = 2-row block.
 *    Full window = CASET 0x19..0x23, RASET 0x00..0x7C.
 *
 *  - Init: same register sequence and low-power-mode (0x39) flow as the
 *    168x384 STM32 port; panel-specific values match the PANEL_122X250 config
 *    with trimmed gate (0xC0) and LPM source (0xC4/0xC5) voltages to reduce
 *    per-refresh charge injection during 1 Hz updates:
 *        0xD6 0x17,0x02   0xC0 0x09,0x03   0xB0 0x3F
 *        0x2A 0x19,0x23   0x2B 0x00,0x7C
 *    0xB2 stays 0x00 (LPM frame rate), the value your ESP32 sweep found to be
 *    the minimum-current step on this panel.
 * ---------------------------------------------------------------------------
 */

uint8_t g_framebuffer[DISPLAY_BYTES]; /* panel-native packed buffer, 4125 bytes */
volatile bool spi_dma_complete = false;

/* Dynamic sub-window bounds (logical X and width) updated by
   draw_clock_face_seconds() so 50/60 ticks only transmit the units digit. */
uint16_t g_sec_partial_x = SEC_RECT_X;
uint16_t g_sec_partial_w = SEC_RECT_W;

static const uint8_t font5x7[95][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x04,0x1F,0x04,0x00,0x00},{0x0E,0x11,0x0E,0x00,0x00},
  {0x1F,0x15,0x1F,0x00,0x00},{0x1F,0x0A,0x1F,0x00,0x00},{0x0D,0x1F,0x0D,0x00,0x00},
  {0x0F,0x14,0x1F,0x00,0x00},{0x1F,0x04,0x04,0x00,0x00},{0x0A,0x1F,0x0A,0x00,0x00},
  {0x04,0x1F,0x04,0x00,0x00},{0x04,0x04,0x04,0x00,0x00},{0x1F,0x04,0x1F,0x00,0x00},
  {0x1F,0x0C,0x1F,0x00,0x00},{0x0F,0x14,0x0F,0x00,0x00},{0x1F,0x14,0x1F,0x00,0x00},
  {0x1F,0x15,0x0D,0x00,0x00},{0x1F,0x0A,0x1F,0x00,0x00},{0x0F,0x14,0x0F,0x00,0x00},
  {0x1F,0x04,0x1F,0x00,0x00},{0x1F,0x14,0x0F,0x00,0x00},{0x0F,0x17,0x1F,0x00,0x00},
  {0x1F,0x04,0x1F,0x00,0x00},{0x1F,0x10,0x1F,0x00,0x00},{0x1F,0x10,0x1F,0x00,0x00},{0x1F,0x14,0x1F,0x00,0x00},
  {0x1F,0x00,0x1F,0x00,0x00},{0x1F,0x04,0x1F,0x00,0x00},{0x1F,0x04,0x1F,0x00,0x00},{0x0E,0x11,0x0E,0x00,0x00},
  {0x00,0x1F,0x00,0x00,0x00},{0x01,0x1F,0x01,0x00,0x00},{0x1F,0x0E,0x1F,0x00,0x00},{0x1F,0x01,0x1F,0x00,0x00},
  {0x1F,0x09,0x1F,0x00,0x00},{0x1F,0x05,0x1F,0x00,0x00},{0x1F,0x15,0x0F,0x00,0x00},{0x1F,0x1D,0x1F,0x00,0x00},
  {0x1F,0x15,0x1F,0x00,0x00},{0x1F,0x12,0x1B,0x00,0x00},{0x0F,0x14,0x0F,0x00,0x00},{0x1F,0x14,0x0F,0x00,0x00},
  {0x1F,0x15,0x17,0x00,0x00},{0x1F,0x09,0x17,0x00,0x00},{0x0F,0x14,0x0F,0x00,0x00},{0x1F,0x15,0x17,0x00,0x00},
  {0x1F,0x04,0x1F,0x00,0x00},{0x1F,0x14,0x1F,0x00,0x00},{0x1F,0x15,0x1F,0x00,0x00},{0x1F,0x04,0x0F,0x00,0x00},
  {0x1F,0x0C,0x1F,0x00,0x00},{0x1F,0x0F,0x1F,0x00,0x00},{0x1F,0x1F,0x1F,0x00,0x00},{0x0F,0x1F,0x0F,0x00,0x00},
  {0x1F,0x15,0x1F,0x00,0x00},{0x1F,0x14,0x1F,0x00,0x00},{0x1F,0x05,0x1F,0x00,0x00},{0x1F,0x0A,0x1F,0x00,0x00},
  {0x1F,0x0D,0x1F,0x00,0x00},{0x1F,0x1D,0x1F,0x00,0x00},{0x1F,0x1C,0x1F,0x00,0x00},{0x1F,0x0E,0x1F,0x00,0x00},
  {0x1F,0x04,0x04,0x00,0x00},{0x1F,0x04,0x1F,0x00,0x00},{0x1F,0x14,0x1F,0x00,0x00},{0x0E,0x11,0x0E,0x00,0x00},
  {0x1F,0x14,0x1F,0x00,0x00},{0x1F,0x0C,0x0F,0x00,0x00},{0x1F,0x15,0x1F,0x00,0x00},{0x1F,0x10,0x1F,0x00,0x00},
  {0x1F,0x1F,0x1F,0x00,0x00},{0x14,0x1F,0x14,0x00,0x00},{0x1F,0x1E,0x1F,0x00,0x00},{0x1F,0x1D,0x1F,0x00,0x00},
  {0x1F,0x1A,0x1F,0x00,0x00},{0x1F,0x09,0x1F,0x00,0x00},{0x10,0x1F,0x10,0x00,0x00},{0x1F,0x0F,0x1F,0x00,0x00},
  {0x1F,0x1C,0x1F,0x00,0x00},{0x1F,0x1F,0x0F,0x00,0x00},{0x1F,0x1F,0x0F,0x00,0x00},{0x1F,0x1F,0x1F,0x00,0x00}
};

/* ---------------------------------------------------------------------- */
/* Low-level SPI/command primitives                                        */
/* ---------------------------------------------------------------------- */

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        /* 1. Terminate the SPI transaction */
        HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);

        /* 2. Signal the main loop that we can enter deep sleep */
        spi_dma_complete = true;
    }
}

/* Start the panel in low-power mode (LPM, 0.25 Hz, FRAME 0x00) - the end state
   of ST7305::initPanel(true). Set to 0 for bring-up: ends in HPM (FRAME 0x02,
   16 Hz) exactly like ST7305::begin() on the ESP32, which is the quickest way
   to see *something* on the glass. */
#ifndef ST7305_START_LOW_POWER
#define ST7305_START_LOW_POWER 1
#endif

/* 1 = init goes through HPM (0x38) and then switches to LPM, as ST7305.cpp's
   initPanel() does. 0 = go straight to LPM and never send 0x38 (the original
   order of this port). Try 0 if the panel draws bursts of extra current after
   each write, which can mean it isn't settling into LPM. */
#ifndef ST7305_INIT_VIA_HPM
#define ST7305_INIT_VIA_HPM 0
#endif

/* Watch these in the debugger: HAL_SPI_Transmit() failures used to be silently
   ignored, so a dead SPI looked exactly like a working one. */
volatile uint32_t g_st7305_spi_errors = 0U;

static void st7305_spi_tx(const uint8_t *data, uint16_t len)
{
  if (HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, 1000U) != HAL_OK) {
    ++g_st7305_spi_errors;
  }
}

/* Mirrors ST7305::writeReg(): the command byte is its own CS-framed transfer
   (DC low), and the parameters are a second CS-framed transfer (DC high).
   CS goes back high in between - the old code held CS low across the whole
   init, which the Arduino library never does. */
static void st7305_reg(uint8_t reg, const uint8_t *args, uint8_t n)
{
  HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
  st7305_spi_tx(&reg, 1U);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);

  if (n > 0U) {
    HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
    st7305_spi_tx(args, n);
    HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
  }
}

/* Single-byte conveniences (same framing as st7305_reg) */
static void st7305_reg0(uint8_t reg)
{
  st7305_reg(reg, NULL, 0U);
}

static void st7305_reg1(uint8_t reg, uint8_t a)
{
  st7305_reg(reg, &a, 1U);
}

/* Public primitives kept for the header's API; each is one CS-framed transfer. */
void st7305_write_command(uint8_t cmd)
{
  st7305_reg0(cmd);
}

void st7305_write_data(uint8_t data)
{
  HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
  st7305_spi_tx(&data, 1U);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
}

void st7305_write_data_buffer(const uint8_t *data, uint16_t len)
{
  HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
  st7305_spi_tx(data, len);
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
}

/* Hardware reset - ST7305::initPanel(): high 20 ms, low 20 ms, high 10 ms. */
void st7305_reset(void)
{
  HAL_GPIO_WritePin(GPIOA, LCD_RST_PIN, GPIO_PIN_SET);
  delay_ms(20U);
  HAL_GPIO_WritePin(GPIOA, LCD_RST_PIN, GPIO_PIN_RESET);
  delay_ms(20U);
  HAL_GPIO_WritePin(GPIOA, LCD_RST_PIN, GPIO_PIN_SET);
  delay_ms(10U);
}

/* Sets the column/row address window in the ST7305's native block units -
   NOT raw pixels. Columns are 12-pixel blocks counted from PANEL_CASET_START
   (0..10 covers the 132 RAM columns incl. the 10-column offset); rows are
   2-line blocks (0..124 covers all 250 lines). Both ranges are inclusive
   block indices. Ends with RAMWR (0x2C) so pixel data can follow.
   Same three register writes, same CS framing, as ST7305::display(). */
static void st7305_set_window(uint8_t caset_start_block, uint8_t caset_end_block,
                               uint8_t raset_start_block, uint8_t raset_end_block)
{
  const uint8_t caset[2] = {(uint8_t)(PANEL_CASET_START + caset_start_block),
                            (uint8_t)(PANEL_CASET_START + caset_end_block)};
  const uint8_t raset[2] = {raset_start_block, raset_end_block};
  st7305_reg(0x2AU, caset, 2U);
  st7305_reg(0x2BU, raset, 2U);
  st7305_reg0(0x2CU);
}

static void st7305_set_full_window(void)
{
  st7305_set_window(0x00U, (uint8_t)(PANEL_CASET_BLOCKS - 1U), 0x00U, (uint8_t)(PANEL_RASET_BLOCKS - 1U));
}

/* ---------------------------------------------------------------------- */
/* Panel initialization                                                    */
/* ---------------------------------------------------------------------- */

void st7305_enter_sleep(void)
{
  st7305_reg0(0x10U); /* Sleep In */
  delay_ms(5U);
}

void st7305_exit_sleep(void)
{
  st7305_reg0(0x11U); /* Sleep Out */
  delay_ms(10U);
  st7305_reg0(0x29U); /* Display On */
  delay_ms(5U);
}

/* ST7305::writeSourceVoltages() - C1/C2/C4/C5 + C9
   C4/C5 (LPM VSHN/VSLN) are trimmed down from {0x5A,0x5C..} / {0x37,0x35..}
   to match C1/C2, reducing dynamic pixel-charging energy on 1 Hz ticks. */
static void st7305_write_source_voltages(void)
{
  static const uint8_t c1[] = {0x3CU, 0x3EU, 0x3CU, 0x3CU};
  static const uint8_t c2[] = {0x23U, 0x21U, 0x23U, 0x23U};
  static const uint8_t c4[] = {0x3CU, 0x3EU, 0x3CU, 0x3CU};
  static const uint8_t c5[] = {0x23U, 0x21U, 0x23U, 0x23U};
  st7305_reg(0xC1U, c1, 4U);
  st7305_reg(0xC2U, c2, 4U);
  st7305_reg(0xC4U, c4, 4U);
  st7305_reg(0xC5U, c5, 4U);
  st7305_reg1(0xC9U, 0x00U);
}

/* Line-for-line port of ST7305::initPanel(bool lowPower) (122x250 panel),
   with lowered gate voltages (0xC0: {0x09, 0x03} vs {0x0E, 0x05}) and lowered
   LPM source voltages (0xC4/0xC5). */
static void st7305_init_panel(bool low_power)
{
  static const uint8_t d6[] = {0x17U, 0x02U};
  static const uint8_t c0[] = {0x09U, 0x03U};  /* lowered VGH/VGL gate swing */
  static const uint8_t d8[] = {0xA6U, 0xE9U};
  static const uint8_t b3[] = {0xE5U, 0xF6U, 0x17U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x71U};
  static const uint8_t b4[] = {0x05U, 0x46U, 0x77U, 0x77U, 0x77U, 0x77U, 0x76U, 0x45U};
  static const uint8_t r62[] = {0x32U, 0x03U, 0x1FU};
  static const uint8_t caset[] = {PANEL_CASET_START, (uint8_t)(PANEL_CASET_START + PANEL_CASET_BLOCKS - 1U)}; /* 0x19, 0x23 */
  static const uint8_t raset[] = {0x00U, (uint8_t)(PANEL_RASET_BLOCKS - 1U)};                                 /* 0x00, 0x7C */

  st7305_reset();                                   /* NOTE: no 0x01 software reset - the Arduino code never sends one */

  st7305_reg(0xD6U, d6, 2U);                        /* NVM load */
  st7305_reg1(0xD1U, 0x01U);                        /* booster enable */
  st7305_reg(0xC0U, c0, 2U);                        /* gate voltages */
  st7305_write_source_voltages();                   /* C1/C2/C4/C5 + C9 */
  st7305_reg(0xD8U, d8, 2U);                        /* oscillator */
  st7305_reg1(0xB2U, low_power ? 0x00U : 0x02U);    /* FRAME_LPM / FRAME_HPM */
  st7305_reg(0xB3U, b3, 10U);
  st7305_reg(0xB4U, b4, 8U);
  st7305_reg(0x62U, r62, 3U);
  st7305_reg1(0xB7U, 0x13U);
  st7305_reg1(0xB0U, 0x3FU);                        /* gate lines */
  st7305_reg0(0x11U);                               /* sleep out */
  delay_ms(120U);                                   /* was 10 ms - panel needs the full 120 ms here */
  st7305_reg1(0xC9U, 0x00U);
  st7305_reg1(0x36U, 0x48U);
  st7305_reg1(0x3AU, 0x11U);
  st7305_reg1(0xB9U, 0x20U);
  st7305_reg1(0xB8U, 0x09U);
  st7305_reg(0x2AU, caset, 2U);
  st7305_reg(0x2BU, raset, 2U);
  st7305_reg1(0x34U, 0x00U);                        /* TE on (was 0x34); harmless if the TE pin is unconnected */
  st7305_reg1(0xD0U, 0xFFU);                        /* auto power down */

#if ST7305_INIT_VIA_HPM
  /* Same as ST7305::initPanel(): go through HPM first, then the datasheet
     HPM => LPM step. */
  st7305_reg0(0x38U);                               /* HPM */
  st7305_reg0(0x29U);
  st7305_reg0(0x20U);
  st7305_reg1(0xBBU, 0x4FU);
  st7305_reg0(0x38U);                               /* HPM */
  delay_ms(300U);

  /* Datasheet HPM => LPM: voltage setting, 20 ms, LPM on, 100 ms */
  st7305_write_source_voltages();
  st7305_reg0(0x29U);
  delay_ms(20U);

  if (low_power) {
    st7305_reg0(0x39U);                             /* LPM */
    delay_ms(100U);
  }
#else
  /* Direct-to-LPM variant: the original order of this port (LPM on before
     Display On, 0x38 never sent), but keeping the corrected CS framing, the
     120 ms sleep-out wait and no software reset. */
  st7305_reg0(low_power ? 0x39U : 0x38U);           /* LPM or HPM, chosen once */
  st7305_reg0(0x29U);
  delay_ms(20U);
  st7305_reg0(0x20U);
  st7305_reg1(0xBBU, 0x4FU);
  delay_ms(100U);
#endif
}

void st7305_init(void)
{
  HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
  st7305_init_panel(ST7305_START_LOW_POWER != 0);
}

/* ---------------------------------------------------------------------- */
/* Full refresh                                                            */
/* ---------------------------------------------------------------------- */

void st7305_refresh(void)
{
    /* Reset our completion flag */
    spi_dma_complete = false;

    /* Window + RAMWR, each CS-framed exactly like ST7305::display() */
    st7305_set_full_window();

    /* START BULK TRANSACTION: DC high, CS low, then DMA straight from the
       framebuffer (already in the panel's wire format). The TxCplt callback
       raises CS when the transfer is finished. */
    HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
    if (HAL_SPI_Transmit_DMA(&hspi1, g_framebuffer, DISPLAY_BYTES) != HAL_OK) {
        ++g_st7305_spi_errors;
        HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
        spi_dma_complete = true;   /* don't leave main() waiting in WFI forever */
    }
}

/* ---------------------------------------------------------------------- */
/* Partial refresh - seconds digits only (dynamic 1-digit or 2-digit)      */
/* ---------------------------------------------------------------------- */
/* The seconds rectangle (SEC_RECT_* in clock_port.h, logical coords) is
   turned into panel blocks:
     logical y range  -> native x = 121 - y -> RAM column = nx + 10 -> caset block = col / 12
     logical x range  -> native y = x                              -> raset block = y / 2
   draw_clock_face_seconds() sets g_sec_partial_x and g_sec_partial_w to
   either the units-digit sub-window (50/60 ticks) or the full two-digit
   SEC_RECT window (10/60 ticks on tens rollovers), shrinking the RASET span
   and SPI DMA payload by ~50% on routine ticks. */
#define SEC_NX_MIN      ((DISPLAY_HEIGHT - 1U) - (SEC_RECT_Y + SEC_RECT_H - 1U))
#define SEC_NX_MAX      ((DISPLAY_HEIGHT - 1U) - SEC_RECT_Y)
#define SEC_CASET_FIRST ((SEC_NX_MIN + PANEL_X_OFFSET) / 12U)
#define SEC_CASET_LAST  ((SEC_NX_MAX + PANEL_X_OFFSET) / 12U)
#define SEC_RASET_FIRST (SEC_RECT_X / 2U)
#define SEC_RASET_LAST  ((SEC_RECT_X + SEC_RECT_W - 1U) / 2U)
#define SEC_CASET_COUNT (SEC_CASET_LAST - SEC_CASET_FIRST + 1U)
#define SEC_RASET_COUNT (SEC_RASET_LAST - SEC_RASET_FIRST + 1U)
#define SEC_WINDOW_BYTES (SEC_CASET_COUNT * SEC_RASET_COUNT * 3U)

#if (SEC_CASET_LAST >= PANEL_CASET_BLOCKS) || (SEC_RASET_LAST >= PANEL_RASET_BLOCKS)
#error "Seconds rectangle falls outside the panel - check SEC_RECT_* in clock_port.h"
#endif

static uint8_t partial_seconds_buf[SEC_WINDOW_BYTES];

void st7305_refresh_seconds_partial(void)
{
    /* Reset our completion flag */
    spi_dma_complete = false;

    uint16_t win_x = g_sec_partial_x;
    uint16_t win_w = g_sec_partial_w;
    if ((win_w == 0U) || (win_x < SEC_RECT_X) || ((win_x + win_w) > (SEC_RECT_X + SEC_RECT_W))) {
        win_x = SEC_RECT_X;
        win_w = SEC_RECT_W;
    }

    uint8_t raset_first = (uint8_t)(win_x / 2U);
    uint8_t raset_last  = (uint8_t)((win_x + win_w - 1U) / 2U);
    uint16_t raset_count = (uint16_t)(raset_last - raset_first + 1U);
    uint16_t window_bytes = (uint16_t)(SEC_CASET_COUNT * raset_count * 3U);

    /* Gather the active window, one row pair at a time, in wire order:
       for each raset row, the caset blocks left to right, 3 bytes per block. */
    uint8_t *dst = partial_seconds_buf;
    for (uint8_t r = raset_first; r <= raset_last; ++r) {
        const uint8_t *src = &g_framebuffer[(uint16_t)r * PANEL_BYTES_PER_ROWPAIR + (uint16_t)SEC_CASET_FIRST * 3U];
        memcpy(dst, src, SEC_CASET_COUNT * 3U);
        dst += SEC_CASET_COUNT * 3U;
    }

    st7305_set_window(SEC_CASET_FIRST, SEC_CASET_LAST, raset_first, raset_last);

    HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_RESET);
    if (HAL_SPI_Transmit_DMA(&hspi1, partial_seconds_buf, window_bytes) != HAL_OK) {
        ++g_st7305_spi_errors;
        HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
        spi_dma_complete = true;
    }
    /* otherwise CS is released by HAL_SPI_TxCpltCallback() */
}

/* ---------------------------------------------------------------------- */
/* Framebuffer drawing primitives (logical 250x122 canvas)                  */
/* ---------------------------------------------------------------------- */

void fb_clear(void)
{
  memset(g_framebuffer, 0x00U, sizeof(g_framebuffer));
}

void fb_set_pixel(uint16_t x, uint16_t y, uint8_t value)
{
  if ((x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT)) {
    return;
  }

  /* logical -> native portrait */
  uint16_t nx = (uint16_t)(DISPLAY_HEIGHT - 1U - y);
  uint16_t ny = x;

  uint16_t col = (uint16_t)(nx + PANEL_X_OFFSET);
  uint16_t byte_pos = (uint16_t)((ny >> 1) * PANEL_BYTES_PER_ROWPAIR + (col >> 2));
  uint8_t bit = (uint8_t)(1U << (7U - (((col & 0x03U) << 1) + (ny & 0x01U))));

  if (value != 0U) {
    g_framebuffer[byte_pos] |= bit;
  } else {
    g_framebuffer[byte_pos] &= (uint8_t)~bit;
  }
}

/* Fills a rectangle (logical landscape coordinates) in g_framebuffer one
   BYTE at a time instead of one pixel at a time.

   Same result as the old double loop over fb_set_pixel(), including clipping
   at the canvas edge, but ~7x fewer operations for a typical 60x48 rect (that
   per-pixel loop was a big part of the time the MCU spent awake each second).

   Frame buffer layout (see fb_set_pixel): logical (x, y) maps to native
   row ny = x, native column nx = 121 - y, RAM column col = nx + PANEL_X_OFFSET.
   One byte = 4 columns x 2 rows: its bits, MSB first, are
       col0/row0, col0/row1, col1/row0, col1/row1, col2/row0, ... col3/row1
   so row 0 of the pair is bits 7,5,3,1 (0xAA) and row 1 is bits 6,4,2,0 (0x55),
   and column c (0..3) of the byte is the bit pair (0xC0 >> (2 * c)). */
void fb_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t value)
{
  if ((w == 0U) || (h == 0U) || (x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT)) {
    return;
  }
  /* Clip to the logical canvas, as fb_set_pixel() would have */
  if ((uint32_t)x + w > DISPLAY_WIDTH) {
    w = (uint16_t)(DISPLAY_WIDTH - x);
  }
  if ((uint32_t)y + h > DISPLAY_HEIGHT) {
    h = (uint16_t)(DISPLAY_HEIGHT - y);
  }

  /* Rectangle in native panel coordinates (inclusive ranges) */
  uint16_t row_first = x;                                                   /* native row    */
  uint16_t row_last  = (uint16_t)(x + w - 1U);
  uint16_t col_first = (uint16_t)((DISPLAY_HEIGHT - 1U - (y + h - 1U)) + PANEL_X_OFFSET); /* RAM column */
  uint16_t col_last  = (uint16_t)((DISPLAY_HEIGHT - 1U - y) + PANEL_X_OFFSET);

  uint16_t pair_first = (uint16_t)(row_first >> 1);
  uint16_t pair_last  = (uint16_t)(row_last >> 1);
  uint16_t byte_col_first = (uint16_t)(col_first >> 2);
  uint16_t byte_col_last  = (uint16_t)(col_last >> 2);

  for (uint16_t pair = pair_first; pair <= pair_last; ++pair) {
    /* Which of this pair's two native rows are inside the rectangle? */
    uint16_t upper_row = (uint16_t)(pair * 2U);
    uint16_t lower_row = (uint16_t)(upper_row + 1U);
    uint8_t row_mask = 0U;
    if ((upper_row >= row_first) && (upper_row <= row_last)) {
      row_mask |= 0xAAU;
    }
    if ((lower_row >= row_first) && (lower_row <= row_last)) {
      row_mask |= 0x55U;
    }

    uint8_t *byte_ptr = &g_framebuffer[(uint32_t)pair * PANEL_BYTES_PER_ROWPAIR + byte_col_first];
    for (uint16_t byte_col = byte_col_first; byte_col <= byte_col_last; ++byte_col) {
      /* Only the first and last byte of the row can be partly covered */
      uint8_t first_c = (byte_col == byte_col_first) ? (uint8_t)(col_first & 0x03U) : 0U;
      uint8_t last_c  = (byte_col == byte_col_last)  ? (uint8_t)(col_last & 0x03U)  : 3U;
      uint8_t col_mask = (uint8_t)((0xFFU >> (2U * first_c)) & (0xFFU << (2U * (3U - last_c))));
      uint8_t mask = (uint8_t)(col_mask & row_mask);

      if (value != 0U) {
        *byte_ptr |= mask;
      } else {
        *byte_ptr &= (uint8_t)~mask;
      }
      ++byte_ptr;
    }
  }
}

void fb_draw_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t value)
{
  if ((w == 0U) || (h == 0U)) {
    return;
  }
  for (uint16_t xx = x; xx < x + w; ++xx) {
    fb_set_pixel(xx, y, value);
    fb_set_pixel(xx, (uint16_t)(y + h - 1U), value);
  }
  for (uint16_t yy = y; yy < y + h; ++yy) {
    fb_set_pixel(x, yy, value);
    fb_set_pixel((uint16_t)(x + w - 1U), yy, value);
  }
}

void fb_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t value)
{
  int16_t dx = (int16_t)abs_int((int32_t)x1 - (int32_t)x0);
  int16_t dy = (int16_t)abs_int((int32_t)y1 - (int32_t)y0);
  int16_t sx = (x0 < x1) ? 1 : -1;
  int16_t sy = (y0 < y1) ? 1 : -1;
  int16_t err = dx - dy;

  while (1) {
    fb_set_pixel((uint16_t)x0, (uint16_t)y0, value);
    if ((x0 == x1) && (y0 == y1)) {
      break;
    }
    int16_t e2 = err * 2;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void fb_draw_char(uint16_t x, uint16_t y, char ch, uint8_t value)
{
  if ((ch < 32) || (ch > 126)) {
    return;
  }
  const uint8_t *glyph = font5x7[(uint8_t)ch - 32U];
  for (uint8_t row = 0U; row < 5U; ++row) {
    for (uint8_t col = 0U; col < 8U; ++col) {
      if ((glyph[row] >> (7U - col)) & 0x01U) {
        fb_set_pixel(x + col, y + row, value);
      }
    }
  }
}

void fb_draw_string(uint16_t x, uint16_t y, const char *text, uint8_t value)
{
  while (*text != '\0') {
    fb_draw_char(x, y, *text, value);
    x += 6U;
    ++text;
  }
}
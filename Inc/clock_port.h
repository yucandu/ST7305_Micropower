#ifndef __CLOCK_PORT_H
#define __CLOCK_PORT_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LCD_CS_PIN        GPIO_PIN_4
#define LCD_DC_PIN        GPIO_PIN_0
#define LCD_RST_PIN       GPIO_PIN_2
#define PWR_SENSORS_PIN   GPIO_PIN_0
/* ---- Panel: 2.13" ST7305, 122 x 250 native (portrait) ------------------- */
/* DISPLAY_WIDTH/HEIGHT is the LOGICAL landscape canvas all the drawing code
   uses (250 wide x 122 tall). The panel RAM is portrait-native 122 x 250;
   fb_set_pixel() maps logical (x,y) -> native (121-y, x), exactly like the
   Arduino ST7305 library's setPixel() for the 122x250 variant. */
#define DISPLAY_WIDTH     250U
#define DISPLAY_HEIGHT    122U

/* g_framebuffer is stored in the panel's own wire format (no conversion pass
   before SPI): one byte = 4 columns x 2 rows, 33 bytes (= 11 caset blocks x
   3 bytes) per pair of rows, 125 row pairs. The first visible column sits at
   RAM column PANEL_X_OFFSET (10). */
#define PANEL_NATIVE_W        122U
#define PANEL_NATIVE_H        250U
#define PANEL_X_OFFSET        10U
#define PANEL_CASET_START     0x19U   /* 0x2A column window start (12-px blocks) */
#define PANEL_CASET_BLOCKS    11U     /* 0x19..0x23 */
#define PANEL_RASET_BLOCKS    125U    /* 0x00..0x7C (2-line blocks) */
#define PANEL_BYTES_PER_ROWPAIR (PANEL_CASET_BLOCKS * 3U)   /* 33 */
#define DISPLAY_BYTES         (PANEL_BYTES_PER_ROWPAIR * PANEL_RASET_BLOCKS)  /* 4125 */

/* USER CODE BEGIN HistorySizing */
/* ---- History storage ------------------------------------------------------ */
/* Everything is stored as float (no more scaled int16/uint16 counts), and the
   rings are sized so that all of the history together fits in about 80% of the
   STM32U375's 192 KB of SRAM. The static assert below the last size define
   fails the build if the three rings ever outgrow RAM_HISTORY_BUDGET.

     per-minute ring  MAX_SAMPLES       720  = ~1 day x 7 floats (temp, RH,
                                                            abs humidity, AvgCurrent,
                                                            estimated current, solar V, VDDA)
                                                                                   = 20,160 B
     hourly ring      MAX_VOLT_SAMPLES   336  = 83 days  x 1 float (battery V)   =  8,000 B
     per-second ring  CUR_HIST_SAMPLES  7200  = 2 hours   x 2 floats (current,
                                                            solar V)              = 57,600 B
                                                    total   = 105,920 B (~54% of 192 KB)

   Sample counts and ring indices are uint16_t, and the chart cursor is an
   int16_t, so no ring may exceed 32767 samples. */
/* USER CODE END HistorySizing */
#define RAM_TOTAL_BYTES       (192U * 1024U)
#define RAM_HISTORY_BUDGET    ((RAM_TOTAL_BYTES / 10U) * 8U)   /* 80% = 157,280 B */
#define MAX_SAMPLES           720U   /* per-minute history */
#define MAX_VOLT_SAMPLES      336U    /* hourly battery-voltage history */

/* ---- MAX17055 current / voltage / charge measurement (I2C3) -------------- */
/* Used as a plain meter: Current / AvgCurrent / VCell / QH. Every LSB below
   follows from the sense resistor (datasheet: 1.5625 uV and 5.0 uVh per count
   divided by Rsense), so changing the resistor is a one-line change. At 1 ohm
   the Current register spans +/-51.2 mA. */
#define MAX17055_RSENSE_OHMS        1.0f
#define MAX17055_DESIGNCAP_MAH      0.568f
#define MAX17055_VOLTAGE_LSB_V      0.000078125f
#define MAX17055_CURRENT_LSB_UA     (1.5625f / MAX17055_RSENSE_OHMS)   /* uA per count */
#define MAX17055_CAPACITY_LSB_UAH   (5.0f / MAX17055_RSENSE_OHMS)      /* uAh per count (also QH) */
#define MAX17055_REG_COUNT          0x50U                              /* g_max17055_regs[] size */
/* +1: positive gauge current (charging) is positive current and ADDS to the
   cumulative uAh. Set to -1 to flip both if the shunt is wired the other way. */
#define MAX17055_CURRENT_SIGN       1
/* Per-second current and solar-voltage histories behind the Live Current
   chart: 7200 samples = 57.6 KB total. */
#define CUR_HIST_SAMPLES      7200U

/* ---- Supercap charge / current estimate (from AvgVCell) ------------------- */
/* The store is two 3.3 F supercaps in series: C_eff = C1*C2/(C1+C2) = 1.65 F.
   Usable window 2.9 V .. 4.4 V.
     stored charge above the floor:  Q = C_eff * (V - VCAP_MIN_V)      [coulombs]
                                     uAh = Q / 3600 * 1e6
        -> 0 uAh at 2.9 V, 687.5 uAh at 4.4 V
     average current over an interval: I = C_eff * dV / dt              [amps]
        -> 1 mV/min of AvgVCell change = 27.5 uA
   Positive = voltage rising = charging, same convention as the gauge current. */
#define SUPERCAP_CELL_F       3.3f
#define SUPERCAP_COUNT_SERIES 2U
#define SUPERCAP_EFF_F        (SUPERCAP_CELL_F / (float)SUPERCAP_COUNT_SERIES)   /* 1.65 F */
#define VCAP_MIN_V            2.9f
#define VCAP_MAX_V            4.4f
/* Only estimate current across a gap between AvgVCell samples of this length
   (seconds). Shorter = skip (double call); longer = clock was set or reads
   were missed for a while, so just re-baseline. */
#define VCAP_MIN_DT_S         30U
#define VCAP_MAX_DT_S         150U

/* USER CODE BEGIN HistoryRam */
#define HISTORY_RAM_BYTES  (((size_t)MAX_SAMPLES * 6U * sizeof(float)) \
                            + ((size_t)MAX_SAMPLES * sizeof(float)) \
                            + ((size_t)MAX_VOLT_SAMPLES * sizeof(float)) \
                            + ((size_t)CUR_HIST_SAMPLES * 2U * sizeof(float)))
/* USER CODE END HistoryRam */
_Static_assert(HISTORY_RAM_BYTES <= RAM_HISTORY_BUDGET, "history rings exceed 80% of RAM");
_Static_assert((MAX_SAMPLES <= 32767U) && (MAX_VOLT_SAMPLES <= 32767U) && (CUR_HIST_SAMPLES <= 32767U),
               "ring sizes must fit int16_t/uint16_t indices");

typedef enum {
  STATE_MAIN = 0,
  STATE_MENU,
  STATE_SET_TIME,
  STATE_CHART,
  STATE_MAX17055
} app_state_t;
extern volatile bool spi_dma_complete;
extern SPI_HandleTypeDef hspi1;
extern I2C_HandleTypeDef hi2c1;
extern uint8_t g_framebuffer[DISPLAY_BYTES];
extern uint32_t g_boot_ms;
extern uint32_t g_last_sensor_ms;
extern volatile  uint32_t g_epoch_seconds;
extern uint32_t g_last_sample_epoch;
extern uint32_t g_last_input_ms;
extern uint32_t g_last_clock_tick_ms;
extern volatile  bool g_wake_flag;
extern float g_temperature_c;
extern float g_humidity_pct;
extern float g_abs_humidity;
extern float g_battery_voltage;   /* MAX17055 VCell */
extern float g_solar_voltage;     /* PB2/ADC15 divider reading, scaled to panel voltage */
/* USER CODE BEGIN VddaData */
extern float g_vdda_voltage;      /* VDDA derived from the calibrated VREFINT ADC reading */
extern float g_hist_vdda[MAX_SAMPLES]; /* V, one-minute VDDA history */
/* USER CODE END VddaData */
/* Latest MAX17055 AvgCurrent reading in uA, in every mode (sign per
   MAX17055_CURRENT_SIGN). */
extern float g_current_ua;
/* Estimated charge stored in the supercaps above VCAP_MIN_V, in uAh,
   computed from AvgVCell (see the SUPERCAP_* block above). Shown bottom-centre
   on the clock faces. No longer derived from the MAX17055 QH register. */
extern double g_cumulative_uah;
/* Latest MAX17055 AvgVCell reading (V), and whether the last
   update_sensors() actually got one. */
extern float g_battery_avg_voltage;
extern bool g_avg_vcell_valid;
/* Average current (uA) estimated from the change in AvgVCell over the last
   minute: I = C_eff * dV/dt. Positive = charging. */
extern float g_est_current_ua;
extern float g_cur_hist[CUR_HIST_SAMPLES];
extern float g_cur_solar_hist[CUR_HIST_SAMPLES];
extern uint16_t g_cur_head;
extern uint16_t g_cur_count;
extern bool g_display_dirty;

/* Low-power clock face: temperature + HH:MM, redrawn once a minute, instead
   of the ticking seconds clock. Chosen from the menu ("Low Power"/"Seconds"). */
extern bool g_low_power_mode;
/* Live Current mode: MAX17055 AvgCurrent read every second, chart face, gauge
   held out of Hibernate. Mutually exclusive with g_low_power_mode; both false
   = Seconds mode (SHT40 and AvgCurrent read once a minute). */
extern bool g_live_current_mode;

/* Battery icon level in percent. There is no fuel gauge any more, so nothing
   updates this: it stays at 100 and the icon stays fully lit. */
extern float g_battery_soc_pct;

/* Result of the most recent DS3231 read. The clock must never be set from
   a timestamp that didn't actually come back this minute. */
extern bool g_ds3231_valid;
extern uint32_t g_ds3231_epoch;
extern app_state_t g_app_state;
extern int8_t g_menu_selection;
extern int8_t g_menu_scroll_offset;
extern bool g_editing_minutes;
extern uint8_t g_temp_hour;
extern uint8_t g_temp_minute;
extern bool g_left_pressed_prev;
extern bool g_up_pressed_prev;
extern bool g_down_pressed_prev;
extern bool g_right_pressed_prev;
extern bool g_button_wake;
extern float g_hist_temp[MAX_SAMPLES];        /* degC */
extern float g_hist_rh[MAX_SAMPLES];          /* % */
extern float g_hist_abs_hum[MAX_SAMPLES];     /* g/m^3 */
extern float g_hist_volt[MAX_VOLT_SAMPLES];   /* V, one sample an hour */
extern float g_hist_cur[MAX_SAMPLES];         /* uA, one-minute average */
extern float g_hist_est_cur[MAX_SAMPLES];     /* uA, supercap dV/dt estimate, one per minute */
extern float g_hist_solar[MAX_SAMPLES];       /* V, solar voltage, one sample per minute */
extern uint16_t g_hist_head;
extern uint16_t g_hist_count;
extern uint16_t g_volt_head;
extern uint16_t g_volt_count;
extern uint8_t g_volt_sample_counter;
extern int16_t g_chart_cursor;
extern int8_t g_chart_index;

void MX_I2C1_Init(void);

int abs_int(int value);
void rtc_set_epoch(uint32_t epoch_s);
uint32_t rtc_get_epoch(void);
void rtc_arm_wake_in_1s(void);
void rtc_arm_wake_at_epoch(uint32_t target_epoch);
/* Wait for `ms`. >= 4 ms: Stop 2 via sleep_stop2_us(). 2..3 ms: core Sleep
   (WFI, woken by the 1 ms SysTick). <= 1 ms: HAL_Delay(). Never shorter
   than asked. */
void sleep_delay(uint32_t ms);
/* Stop 2 wait of at least `us` microseconds, timed by the RTC sub-second
   alarm (resolution 1/(PREDIV_S+1) s = 3.9 ms at PREDIV_S = 255). Rounds up
   to whole RTC ticks and adds one tick for the unknown phase, so the wait is
   never short: 1..2 conversions of 2.1 ms -> 3.9..7.8 ms. */
void sleep_stop2_us(uint32_t us);
void enter_stop2_sleep(void);
void st7305_init(void);
void st7305_reset(void);
void st7305_write_command(uint8_t cmd);
void st7305_write_data(uint8_t data);
void st7305_write_data_buffer(const uint8_t *data, uint16_t len);
void st7305_enter_sleep(void);
void st7305_exit_sleep(void);
void st7305_refresh(void);
void fb_clear(void);
void fb_set_pixel(uint16_t x, uint16_t y, uint8_t value);
void fb_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t value);
void fb_draw_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t value);
void fb_draw_char(uint16_t x, uint16_t y, char ch, uint8_t value);
void fb_draw_string(uint16_t x, uint16_t y, const char *text, uint8_t value);
void fb_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t value);

/* u8g2 compiled-font renderer (see clock_port_font.c) - pass one of
   g_font_big / g_font_small below as the font pointer. `y` is the text
   baseline, matching u8g2's own drawStr() convention. */
uint16_t fb_draw_string_u8g2(const uint8_t *font, uint16_t x, uint16_t y, const char *text, uint8_t value);
uint16_t fb_string_width_u8g2(const uint8_t *font, const char *text);
void fb_draw_rle_bitmap(uint16_t x, uint16_t y, uint16_t w_pixels, uint16_t h_pixels, const uint8_t *comp_data);

/* Font arrays live in the u8g2_font_*.h headers included by
   clock_port_font.c; sizes are left open here so swapping a font only means
   changing the #include + the #defines below. */
extern const uint8_t u8g2_font_logisoso46_tn[];
extern const uint8_t u8g2_font_logisoso38_tn[];
extern const uint8_t u8g2_font_profont15_tr[];
extern const unsigned char epd_bitmap_rle[];
#define FONT_BIG     u8g2_font_logisoso46_tn   /* seconds face + set-time digits (46 px tall) */
#define FONT_BIG_LP  u8g2_font_logisoso38_tn   /* low-power face (time + temperature) */
#define FONT_SMALL   u8g2_font_profont15_tr    /* readouts, menu, chart, AM/PM (9 px caps, 7 px advance) */

/* Height of a glyph above the baseline (bbox height + y offset), read from the
   font itself - lets the layout code stay correct if a font is swapped. */
uint8_t fb_font_glyph_top(const uint8_t *font, char ch);

/* 1 = draw the converted 250x122 background art (monochromebg_rle.h) behind
   the clock faces. 0 = draw a built-in placeholder (battery outline +
   separator lines) so the firmware runs before the art is converted. */
#define USE_RLE_BACKGROUND   0
#define BG_ART_WIDTH         250U
#define BG_ART_HEIGHT        122U

/* ---- Clock-face geometry shared by the UI and the partial-refresh code --- */
#define FACE_TOP_BASELINE     14U    /* top readout row text baseline */
#define FACE_BOT_BASELINE     119U   /* bottom readout row text baseline */
#define FACE_BIG_ASCENT       46U    /* FONT_BIG digit height */
#define FACE_BIG_BASELINE     86U    /* seconds-face digit baseline */
#define FACE_SEC_X            168U   /* left edge of the two seconds digits */
#define FACE_SEC_DIGITS_W     60U    /* 2 x 30 px advance of FONT_BIG */
/* Rectangle (logical coords) the seconds partial refresh redraws + sends. */
#define SEC_RECT_X            (FACE_SEC_X)
#define SEC_RECT_Y            (FACE_BIG_BASELINE - FACE_BIG_ASCENT - 1U)
#define SEC_RECT_W            (FACE_SEC_DIGITS_W)
#define SEC_RECT_H            (FACE_BIG_ASCENT + 2U)

void st7305_refresh_seconds_partial(void);

extern uint32_t g_boot_epoch;
extern float g_accumulated_energy_uwh;
extern bool g_main_needs_full_redraw;

/* draw_clock_face_full() is the Seconds face (big ticking clock): full redraw
   once a minute, then draw_clock_face_seconds() + the partial SPI window every
   second. draw_live_current_face() is the Live Current dashboard (current
   chart), redrawn and sent in full every second. */
void draw_clock_face_full(void);
void draw_clock_face_seconds(void);
void draw_live_current_face(void);
void draw_clock_face_low_power(void);

uint32_t millis_now(void);
void delay_ms(uint32_t ms);
bool i2c_init(void);
void i2c_deinit(void);
bool i2c_write(uint8_t addr, const uint8_t *data, uint16_t len);
bool i2c_read(uint8_t addr, uint8_t *data, uint16_t len);
bool i2c_writeread(uint8_t addr, const uint8_t *wdata, uint16_t wlen, uint8_t *rdata, uint16_t rlen);
void sleep_pins(void);
void wake_pins(void);

bool ds3231_begin(void);
bool ds3231_now(uint32_t *epoch_s);
bool ds3231_set_epoch(uint32_t epoch_s);
void update_sensors(void);

/* I2C3: MAX17055 + SHT40, always powered (clock_port_i2c3.c). Wrap bus use in
   i2c3_begin()/i2c3_end() unless a function says it brackets the bus itself. */
void i2c3_begin(void);
void i2c3_end(void);
bool max17055_init_if_needed(void);          /* EZ config after power-up; bus must be begun */
void max17055_set_live_mode(bool live);      /* true: out of Hibernate + Current reg; brackets the bus */
bool max17055_read_vcell(float *volts);      /* bus must be begun */
bool max17055_read_avgvcell(float *volts);   /* bus must be begun */
/* Call once a minute after update_sensors(), with the best timestamp for that
   reading. Updates g_cumulative_uah (charge from AvgVCell) and
   g_est_current_ua (dV/dt since the previous call). */
void vcap_estimator_update(uint32_t sample_epoch);
/* Forget the dV/dt baseline and zero the estimated current (Reset Data). */
void vcap_estimator_reset(void);
bool max17055_read_info_regs(void);          /* fills g_max17055_regs[]; brackets the bus */
extern uint16_t g_max17055_regs[MAX17055_REG_COUNT];
extern bool g_max17055_regs_valid;
bool sht40_read(float *temperature_c, float *humidity_pct);
bool current_sample_take(uint32_t now_epoch, bool record_history);
float current_history_get(uint16_t k);   /* uA; k = 0 is the oldest sample */
float current_solar_history_get(uint16_t k); /* V; aligned with current_history_get() */
void current_history_reset(void);

void draw_menu_screen(void);
void draw_set_time_screen(void);
void draw_chart_screen(void);
void draw_max17055_screen(void);
void handle_ui(void);
void record_history_sample(void);
float get_history_value(uint8_t chart_index, uint16_t raw_index);

#endif
#include "clock_port.h"

#define MENU_VISIBLE     7
#define MENU_ROW_PITCH   16U   /* 7 rows x 16 px = 112 of the 122 px canvas */

/* Shared with clock_port_display.c so st7305_refresh_seconds_partial() sends
   only the units-digit window on the 50/60 ticks where tens don't change. */
extern uint16_t g_sec_partial_x;
extern uint16_t g_sec_partial_w;
static uint8_t s_displayed_sec_tens = 0xFFU;

/* USER CODE BEGIN VddaChartMenu */
/* Menu order. The first eight entries double as chart indices (g_chart_index
   is set straight from g_menu_selection, and draw_chart_screen() titles the
   chart with menu_items[g_chart_index]), so keep the chart entries first
   and in this order (menu_items[] below must match this enum). */
typedef enum {
  MENU_TEMPERATURE = 0,
  MENU_REL_HUMIDITY,
  MENU_ABS_HUMIDITY,
  MENU_VOLTAGE,
  MENU_AVG_CURRENT,
  MENU_EST_CURRENT,
  MENU_SOLAR,
  MENU_VDDA,
  MENU_LOW_POWER,
  MENU_SECONDS,
  MENU_LIVE_CURRENT,
  MENU_MAX17055,
  MENU_SET_TIME,
  MENU_RESET_DATA,
  MENU_RESET_SYSTEM,
  MENU_ITEM_COUNT
} menu_item_t;

#define NUM_MENU_ITEMS   ((int)MENU_ITEM_COUNT)
#define NUM_CHART_MENU_ITEMS  ((int)MENU_LOW_POWER)   /* entries 0..7 open a chart */
#define CHART_VOLTAGE    ((uint8_t)MENU_VOLTAGE)       /* hourly samples, own ring */
#define CHART_CURRENT    ((uint8_t)MENU_AVG_CURRENT)   /* stored and shown in uA */
#define CHART_EST_CURRENT ((uint8_t)MENU_EST_CURRENT)  /* supercap dV/dt estimate, uA */
#define CHART_SOLAR      ((uint8_t)MENU_SOLAR)         /* one-minute solar voltage */
#define CHART_VDDA       ((uint8_t)MENU_VDDA)          /* one-minute VDDA voltage */
/* USER CODE END VddaChartMenu */
/* The chart plot is 176 px wide (x 66..242), so with more samples than that
   each cursor step moves one pixel column's worth of samples instead of one. */
#define CHART_PLOT_PIXELS 176U

/* ---- Clock-face layout (logical 250x122 canvas) -------------------------- */
/* Three horizontal bands:
     y   0..17   top readout row   (text baseline FACE_TOP_BASELINE = 14)
     y  19..106  Seconds face: current chart / Low Power face: big digits
     y 108..121  bottom readout row (text baseline FACE_BOT_BASELINE = 119)
   Icon slots the background art (monochromebg_rle.h) should provide - all
   12 px wide, vertically on rows 3..14 of the top band:
     thermometer  x   2..13   -> temperature text starts at x=16
     droplet      x  84..95   -> "RH% abs-g" text starts at x=98
     (x 169..180 was the pressure icon; the Seconds face now puts the clock
      text there, right-aligned to x=247)
   Bottom band: battery icon at x 2..28, solar V above battery V at x=30,
   estimated supercap uAh centred on x=125, uptime right-aligned to x=247. */
#define TOP_TEMP_X            16U
#define TOP_HUM_X             98U
#define TOP_CLOCK_RIGHT_X     247U
#define BOT_VOLT_X            30U
#define BOT_UAH_CENTER_X      115U
#define BOT_UPTIME_RIGHT_X    247U

/* Seconds-face current chart (inside the y 19..106 band; the separator lines
   are at y=18 and y=107). Y-axis labels sit left of the axis bar; the plot
   is CHART_PLOT_W columns wide. */
/* Seconds face AM/PM tag: x=232, AM level with the digit tops, PM level with
   the digit baseline (only the active one is drawn). */
#define SEC_AMPM_X            232U
#define TOP_CURRENT_RIGHT_X   247U

#define SCHART_AXIS_X         50
#define SCHART_PLOT_X0        52
#define SCHART_PLOT_X1        220
#define SCHART_PLOT_W         ((SCHART_PLOT_X1 - SCHART_PLOT_X0) + 1)
#define SCHART_SOLAR_AXIS_X    222
#define SCHART_Y_TOP          48
#define SCHART_READOUT_BASELINE 29   /* unit (left) and latest reading (right) above the plot */
#define SCHART_Y_BOTTOM       93
/* Smallest vertical span shown, in uA, so a flat trace isn't blown up into
   noise. */
#define SCHART_MIN_SPAN_UA     2.0f

/* Battery icon: outline x 2..26 / y 109..119 with a nub, 5 lit segments of
   3 px on a 4 px pitch inside it. draw_battery_level_mask() whites out
   segments from the right for each lower RepSOC band, so the background art
   must have all 5 segments lit at exactly this geometry. */
#define BATT_FRAME_X          2U
#define BATT_FRAME_Y          109U
#define BATT_FRAME_W          25U
#define BATT_FRAME_H          11U
#define BATT_SEG_X0           5U
#define BATT_SEG_PITCH        4U
#define BATT_SEG_W            3U
#define BATT_SEG_COUNT        5U
#define BATT_SEG_Y            111U
#define BATT_SEG_H            7U

/* Low-power face: H:MM (left) + AM/PM + temperature (right), all in
   FONT_BIG_LP (38 px). Photoshop-style tracking scaled from the 62 px
   original (-3 px) to this size. */
#define LP_BIG_TRACKING_PX    (-2)
#define LP_BIG_BASELINE_Y     82U
#define LP_TIME_X             6U
#define LP_UNIT_X             234U   /* degree mark x; temp digits end LP_UNIT_GAP_X before it */
#define LP_UNIT_GAP_X         4U
#define LP_AMPM_GAP_X         5U     /* gap between the time and its AM/PM tag */

/* Hard thumbwheel switches auto-repeat while held: fire once immediately,
   wait UI_REPEAT_INITIAL_MS, then keep firing every UI_REPEAT_INTERVAL_MS.
   Slight switches (handled separately below) never repeat - one press is
   one step, no matter how long it's held. */
#define UI_REPEAT_INITIAL_MS   400U
#define UI_REPEAT_INTERVAL_MS  120U

/* profont17_tr doesn't include a degree-sign glyph (confirmed by scanning
   its glyph table), so drawing "\xB0" through fb_draw_string_u8g2() just
   silently skips it (0 advance) - the digit and "C" end up jammed
   together with no gap. Draw a small manual ring instead. */
static void fb_draw_degree_mark(uint16_t x, uint16_t y)
{
  fb_set_pixel((uint16_t)(x + 1U), y, 1U);
  fb_set_pixel((uint16_t)(x + 2U), y, 1U);
  fb_set_pixel(x, (uint16_t)(y + 1U), 1U);
  fb_set_pixel((uint16_t)(x + 3U), (uint16_t)(y + 1U), 1U);
  fb_set_pixel((uint16_t)(x + 1U), (uint16_t)(y + 2U), 1U);
  fb_set_pixel((uint16_t)(x + 2U), (uint16_t)(y + 2U), 1U);
}

/* x for text of width `w` whose right edge should land on `right`; clamps at 0
   instead of wrapping (fb_set_pixel() would then clip the whole string). */
static uint16_t right_aligned_x(uint16_t right, uint16_t w)
{
  return (right > w) ? (uint16_t)(right - w) : 0U;
}

/* USER CODE BEGIN VddaChartMenuItems */
static const char *menu_items[NUM_MENU_ITEMS] = {
  "Temperature", "Rel Humidity", "Abs Humidity",
  "Voltage", "Avg Current", "Est Current", "Solar", "VDDA", "Low Power", "Seconds", "Live Current",
  "MAX17055", "Set Time", "Reset Data", "Reset System"
};
/* USER CODE END VddaChartMenuItems */

/* Outline-only circle (u8g2.drawCircle() equivalent), used for the chart
   cursor marker. fb_set_pixel() clips out-of-bounds coordinates itself
   (an int16_t cast to uint16_t wraps to a huge value and fails the bounds
   check), so no manual clamping is needed here - same pattern fb_draw_line()
   already relies on. */
static void fb_draw_circle(int16_t cx, int16_t cy, int16_t r)
{
  int16_t x = r;
  int16_t y = 0;
  int16_t err = 0;

  while (x >= y) {
    fb_set_pixel((uint16_t)(cx + x), (uint16_t)(cy + y), 1U);
    fb_set_pixel((uint16_t)(cx + y), (uint16_t)(cy + x), 1U);
    fb_set_pixel((uint16_t)(cx - y), (uint16_t)(cy + x), 1U);
    fb_set_pixel((uint16_t)(cx - x), (uint16_t)(cy + y), 1U);
    fb_set_pixel((uint16_t)(cx - x), (uint16_t)(cy - y), 1U);
    fb_set_pixel((uint16_t)(cx - y), (uint16_t)(cy - x), 1U);
    fb_set_pixel((uint16_t)(cx + y), (uint16_t)(cy - x), 1U);
    fb_set_pixel((uint16_t)(cx + x), (uint16_t)(cy - y), 1U);

    ++y;
    if (err <= 0) {
      err += (2 * y) + 1;
    }
    if (err > 0) {
      --x;
      err -= (2 * x) + 1;
    }
  }
}

/* Powers of 10 for 0-5 decimal places - covers the base 1/2/3 decimal
   precisions plus the +1 (axis labels) / +2 (cursor) bumps added below. */
static int32_t decimal_scale(uint8_t decimals)
{
  static const int32_t scales[6] = {1L, 10L, 100L, 1000L, 10000L, 100000L};
  return (decimals <= 5U) ? scales[decimals] : scales[5];
}

static void format_fixed_float(float value, uint8_t decimals, char *buffer, size_t buffer_size)
{
  int32_t scale = decimal_scale(decimals);

  float scaled_value = value * (float)scale;
  int32_t rounded_value = (scaled_value < 0.0f)
                            ? (int32_t)(scaled_value - 0.5f)
                            : (int32_t)(scaled_value + 0.5f);
  bool negative = rounded_value < 0;
  int32_t magnitude = negative ? -rounded_value : rounded_value;
  int32_t whole = magnitude / scale;
  int32_t fraction = magnitude % scale;

  if (decimals == 0U) {
    snprintf(buffer, buffer_size, "%s%ld", negative ? "-" : "", (long)whole);
  } else {
    snprintf(buffer, buffer_size, "%s%ld.%0*ld", negative ? "-" : "", (long)whole, (int)decimals, (long)fraction);
  }
}

/* Base per-chart-type decimal precision (3 decimals for Voltage, 1 for
   everything else). */
static uint8_t base_chart_decimals(uint8_t chart_index)
{
  /* USER CODE BEGIN VddaChartPrecision */
  if ((chart_index == CHART_VOLTAGE) || (chart_index == CHART_SOLAR) ||
      (chart_index == CHART_VDDA)) {
    return 3U;
  }
  return 1U;
  /* USER CODE END VddaChartPrecision */
}

/* Y-axis labels (min/mid/max), shown on every chart: one extra decimal of
   precision over the base. */
static void format_chart_value(uint8_t chart_index, float value, char *buffer, size_t buffer_size)
{
  format_fixed_float(value, (uint8_t)(base_chart_decimals(chart_index) + 1U), buffer, buffer_size);
}

/* Cursor readout on the currently-viewed chart: two extra decimals of
   precision over the base (one more than the axis labels). */
static void format_chart_value_cursor(uint8_t chart_index, float value, char *buffer, size_t buffer_size)
{
  format_fixed_float(value, (uint8_t)(base_chart_decimals(chart_index) + 2U), buffer, buffer_size);
}

float get_history_value(uint8_t chart_index, uint16_t raw_index)
{
  bool is_voltage = (chart_index == CHART_VOLTAGE);
  uint16_t c_count = is_voltage ? g_volt_count : g_hist_count;
  uint16_t c_head = is_voltage ? g_volt_head : g_hist_head;
  uint16_t capacity = is_voltage ? MAX_VOLT_SAMPLES : MAX_SAMPLES;
  uint16_t index = (c_count < capacity) ? raw_index : (uint16_t)((c_head + raw_index) % capacity);
  switch (chart_index) {
    case 0U:
      return g_hist_temp[index];
    case 1U:
      return g_hist_rh[index];
    case 2U:
      return g_hist_abs_hum[index];
    case 3U:
      return g_hist_volt[index];
    case 4U:
      /* One-minute average current, uA (see record_history_sample). */
      return g_hist_cur[index];
    case 5U:
      /* Supercap dV/dt estimate, uA, one per minute (see record_history_sample). */
      return g_hist_est_cur[index];
    case 6U:
      return g_hist_solar[index];
    /* USER CODE BEGIN VddaChartHistory */
    case CHART_VDDA:
      return g_hist_vdda[index];
    /* USER CODE END VddaChartHistory */
    default:
      return 0.0f;
  }
}

void record_history_sample(void)
{
  g_hist_temp[g_hist_head] = g_temperature_c;
  g_hist_rh[g_hist_head] = g_humidity_pct;
  g_hist_abs_hum[g_hist_head] = g_abs_humidity;

  /* One-minute average current. In Live Current mode that is the mean of the
     last (up to) 60 per-second samples; in Seconds and Low Power modes only
     one sample a minute exists, so that sample is used. Stored as float uA. */
  float avg_current_ua = g_current_ua;
  if (g_live_current_mode && (g_cur_count > 0U)) {
    uint16_t n = (g_cur_count < 60U) ? g_cur_count : 60U;
    float sum_ua = 0.0f;
    for (uint16_t k = 0U; k < n; ++k) {
      sum_ua += current_history_get((uint16_t)(g_cur_count - 1U - k));
    }
    avg_current_ua = sum_ua / (float)n;
  }
  g_hist_cur[g_hist_head] = avg_current_ua;

  /* Current estimated from the change in AvgVCell over the last minute
     (I = C_eff * dV/dt, see vcap_estimator_update()). Shares the per-minute
     ring index with the other charts. 0 until the second good AvgVCell
     reading gives it a baseline. */
  g_hist_est_cur[g_hist_head] = g_est_current_ua;
  g_hist_solar[g_hist_head] = g_solar_voltage;
  /* USER CODE BEGIN VddaChartSample */
  g_hist_vdda[g_hist_head] = g_vdda_voltage;
  /* USER CODE END VddaChartSample */

  g_hist_head = (g_hist_head + 1U) % MAX_SAMPLES;
  if (g_hist_count < MAX_SAMPLES) {
    ++g_hist_count;
  }

  if (g_volt_sample_counter == 0U) {
    g_hist_volt[g_volt_head] = g_battery_voltage;
    g_volt_head = (g_volt_head + 1U) % MAX_VOLT_SAMPLES;
    if (g_volt_count < MAX_VOLT_SAMPLES) {
      ++g_volt_count;
    }
  }
  g_volt_sample_counter = (uint8_t)((g_volt_sample_counter + 1U) % 60U);
}

/* ---------------------------------------------------------------------- */
/* Shared clock-face helpers                                               */
/* ---------------------------------------------------------------------- */

/* Battery icon, drawn by the prerendered background with every segment lit.
   Each lower RepSOC band whites out one more segment from the right-hand
   end; the rectangles are cumulative (each includes the previous band's
   segments), so only the one matching the current band is drawn. White is
   pixel value 0 on this canvas - the same thing the AM/PM clearing uses. */
static void draw_battery_level_mask(void)
{
  float soc = g_battery_soc_pct;
  uint16_t segments_off = 0U;

  if (soc < 1.0f) {
    segments_off = 5U;
  } else if (soc < 20.0f) {
    segments_off = 4U;
  } else if (soc < 40.0f) {
    segments_off = 3U;
  } else if (soc < 60.0f) {
    segments_off = 2U;
  } else if (soc < 80.0f) {
    segments_off = 1U;
  }
  /* 80% and above: all segments stay lit. */

  if (segments_off != 0U) {
    uint16_t x = (uint16_t)(BATT_SEG_X0 + ((BATT_SEG_COUNT - segments_off) * BATT_SEG_PITCH));
    fb_draw_rect(x, BATT_SEG_Y, (uint16_t)(segments_off * BATT_SEG_PITCH), BATT_SEG_H, 0U);
  }
}

/* Static parts of the clock face. With USE_RLE_BACKGROUND=1 this is the
   converted 250x122 art; otherwise a built-in stand-in: two separator lines
   and the battery icon (no thermometer/droplet/pressure icons). */
static void draw_background(void)
{
#if USE_RLE_BACKGROUND
  fb_draw_rle_bitmap(0U, 0U, BG_ART_WIDTH, BG_ART_HEIGHT, epd_bitmap_rle);
#else
  fb_draw_line(2, 18, 247, 18, 1U);
  fb_draw_line(2, 107, 247, 107, 1U);
  fb_draw_frame(BATT_FRAME_X, BATT_FRAME_Y, BATT_FRAME_W, BATT_FRAME_H, 1U);
  fb_draw_rect((uint16_t)(BATT_FRAME_X + BATT_FRAME_W), (uint16_t)(BATT_FRAME_Y + 3U), 2U, 5U, 1U);
  for (uint16_t i = 0U; i < BATT_SEG_COUNT; ++i) {
    fb_draw_rect((uint16_t)(BATT_SEG_X0 + (i * BATT_SEG_PITCH)), BATT_SEG_Y, BATT_SEG_W, BATT_SEG_H, 1U);
  }
#endif
}

/* Top-left readout: "<value>°C" in the small font at the spot the
   prerendered art leaves for it. Used for temperature on the seconds face
   and for humidex on the low-power face. */
static void draw_top_left_celsius(float value_c)
{
  char buffer[16];
  format_fixed_float(value_c, 1U, buffer, sizeof(buffer));
  uint16_t num_width = fb_draw_string_u8g2(FONT_SMALL, TOP_TEMP_X, FACE_TOP_BASELINE, buffer, 1U);
  /* Degree ring level with the cap height, then the "C" right after it. */
  fb_draw_degree_mark((uint16_t)(TOP_TEMP_X + num_width + 2U), (uint16_t)(FACE_TOP_BASELINE - 9U));
  fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(TOP_TEMP_X + num_width + 8U), FACE_TOP_BASELINE, "C", 1U);
}

/* Humidex (Environment Canada formula):
 *   humidex = T + 0.5555 * (e - 10)
 * where e is the vapour pressure in hPa derived from the dew point:
 *   e = 6.11 * exp(5417.7530 * (1/273.16 - 1/(Td + 273.16)))
 * Dew point comes from the Magnus formula using the SHT40 temperature and
 * relative humidity. RH is clamped to 1..100% so a failed/zero humidity read
 * can't send logf() to -infinity. */
static float calculate_humidex(float temp_c, float rh_pct)
{
  if (rh_pct < 1.0f) {
    rh_pct = 1.0f;
  } else if (rh_pct > 100.0f) {
    rh_pct = 100.0f;
  }

  float gamma = logf(rh_pct / 100.0f) + ((17.62f * temp_c) / (243.12f + temp_c));
  float dew_point_c = (243.12f * gamma) / (17.62f - gamma);
  float dew_point_k = dew_point_c + 273.16f;
  float vapour_pressure_hpa = 6.11f * expf(5417.7530f * ((1.0f / 273.16f) - (1.0f / dew_point_k)));

  return temp_c + (0.5555f * (vapour_pressure_hpa - 10.0f));
}

/* Big-font text with custom tracking. fb_draw_string_u8g2() returns the
   advance of what it drew, so each glyph is drawn on its own and the cursor
   moves by that advance plus LP_BIG_TRACKING_PX. The width helper uses the
   font's normal whole-string width and removes one tracking step per gap, so
   centring/right-justifying lines up with what draw_big_text_tracked()
   actually produces. */
static uint16_t big_text_width_tracked(const char *text)
{
  size_t length = strlen(text);
  if (length == 0U) {
    return 0U;
  }
  int32_t width = (int32_t)fb_string_width_u8g2(FONT_BIG_LP, text)
                  + ((int32_t)(length - 1U) * (int32_t)LP_BIG_TRACKING_PX);
  return (width > 0) ? (uint16_t)width : 0U;
}

static void draw_big_text_tracked(uint16_t x, uint16_t baseline_y, const char *text)
{
  int32_t cursor_x = (int32_t)x;
  char glyph[2] = { 0, 0 };

  for (size_t i = 0U; text[i] != '\0'; ++i) {
    glyph[0] = text[i];
    uint16_t advance = fb_draw_string_u8g2(FONT_BIG_LP, (uint16_t)cursor_x, baseline_y, glyph, 1U);
    cursor_x += (int32_t)advance + (int32_t)LP_BIG_TRACKING_PX;
  }
}
/* Estimated current below this magnitude (uA) is treated as "not charging or
   discharging" and shows "--" instead of a huge, meaningless time. */
#define TIME_EST_MIN_CURRENT_UA   0.0

/* Formats a duration in seconds as "12d 3h 45m", or "2y 40d 5h" past a year.
   Same format the uptime readout always used. */
static void format_duration_dhm(uint32_t total_secs, char *buffer, size_t buffer_size)
{
  uint32_t days = total_secs / 86400U;
  uint32_t hours = (total_secs % 86400U) / 3600U;
  uint32_t mins = (total_secs % 3600U) / 60U;

  if (days > 365U) {
    uint32_t years = days / 365U;
    uint32_t rem_days = days % 365U;
    snprintf(buffer, buffer_size, "%luy %lud %luh",
             (unsigned long)years, (unsigned long)rem_days, (unsigned long)hours);
  } else {
    snprintf(buffer, buffer_size, "%lud %luh %lum",
             (unsigned long)days, (unsigned long)hours, (unsigned long)mins);
  }
}

/* Bottom-right, one row above the uptime (and above the bottom separator
   line): "TTE: 12d 3h 45m" while the supercaps are discharging, or
   "TTF: ..." while charging, from g_est_current_ua and g_cumulative_uah.
     uAh / uA = hours.
     TTE = stored charge / discharge current
     TTF = (full charge - stored charge) / charge current
   Full charge is the uAh at VCAP_MAX_V, using the same C_eff formula as
   vcap_estimator_update(). */
static void draw_time_remaining(void)
{
  char duration[24];
  char buffer[32];
  const double full_uah = (double)SUPERCAP_EFF_F
                          * ((double)VCAP_MAX_V - (double)VCAP_MIN_V)
                          * (1.0e6 / 3600.0);
  const double current_ua = (double)g_est_current_ua;
  const char *label;
  double hours;

  if (current_ua <= -TIME_EST_MIN_CURRENT_UA) {
    label = "TTE:";
    hours = g_cumulative_uah / (-current_ua);
  } else if (current_ua >= TIME_EST_MIN_CURRENT_UA) {
    label = "TTF:";
    double remaining_uah = full_uah - g_cumulative_uah;
    if (remaining_uah < 0.0) {
      remaining_uah = 0.0;
    }
    hours = remaining_uah / current_ua;
  } else {
    label = "TTE:";
    hours = -1.0;   /* flat: no meaningful estimate */
  }

  if (hours < 0.0) {
    snprintf(buffer, sizeof(buffer), "%s --", label);
  } else {
    if (hours > 100000.0) {
      hours = 100000.0;   /* keep hours * 3600 inside uint32_t */
    }
    format_duration_dhm((uint32_t)(hours * 3600.0), duration, sizeof(duration));
    snprintf(buffer, sizeof(buffer), "%s %s", label, duration);
  }

  uint16_t width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(BOT_UPTIME_RIGHT_X, width),
                      (uint16_t)(FACE_BOT_BASELINE - 14U), buffer, 1U);
}
/*
 * Live Current face (250x122): a once-per-second current dashboard, sent to
 * the panel in full every second.
 *
 *   top band     : [thermo] 23.4°C      [drop] 45% 9.1g      12:34:56 PM
 *   middle band  : MAX17055 current chart, up to the last 3600 samples
 *   bottom band  : [batt] 3.987v         -1234uAh             12d 3h 45m
 */

/* Readouts both faces share: RH/abs-humidity (top), battery voltage (MAX17055
   VCell, read once a minute), cumulative uAh and uptime (bottom). */
static void draw_readouts(void)
{
  char buffer[32];
  char humidity_buffer[16];
  char abs_humidity_buffer[16];

  format_fixed_float(g_humidity_pct, 0U, humidity_buffer, sizeof(humidity_buffer));
  format_fixed_float(g_abs_humidity, 1U, abs_humidity_buffer, sizeof(abs_humidity_buffer));
  snprintf(buffer, sizeof(buffer), "%s%% %sg", humidity_buffer, abs_humidity_buffer);
  fb_draw_string_u8g2(FONT_SMALL, TOP_HUM_X, FACE_TOP_BASELINE, buffer, 1U);

  format_fixed_float(g_battery_voltage, 3U, buffer, sizeof(buffer));
  size_t voltage_length = strlen(buffer);
  if (voltage_length + 2U < sizeof(buffer)) {
    buffer[voltage_length] = 'v';
    buffer[voltage_length + 1U] = '\0';
  }
  fb_draw_string_u8g2(FONT_SMALL, BOT_VOLT_X, FACE_BOT_BASELINE, buffer, 1U);

  format_fixed_float(g_solar_voltage, 4U, buffer, sizeof(buffer));
  size_t solar_length = strlen(buffer);
  if (solar_length + 2U < sizeof(buffer)) {
    buffer[solar_length] = 'v';
    buffer[solar_length + 1U] = '\0';
  }
  fb_draw_string_u8g2(FONT_SMALL, BOT_VOLT_X, (uint16_t)(FACE_BOT_BASELINE - 14U), buffer, 1U);

  /* Bottom-centre: estimated stored supercap charge in uAh (from AvgVCell,
     see vcap_estimator_update()), two decimals. It is a double; this is split
     into whole and hundredths with plain integer printf (no %f / %lld
     needed). Whole part clamps at 4.29 billion uAh. */
  double uah = g_cumulative_uah;
  bool negative = (uah < 0.0);
  double magnitude = negative ? -uah : uah;
  if (magnitude > 4294967000.0) {
    magnitude = 4294967000.0;
  }
  uint32_t whole = (uint32_t)magnitude;
  uint32_t hundredths = (uint32_t)(((magnitude - (double)whole) * 100.0) + 0.5);
  if (hundredths >= 100U) {
    hundredths -= 100U;
    ++whole;
  }
  if ((whole == 0U) && (hundredths == 0U)) {
    negative = false;   /* no "-0.00" */
  }
  snprintf(buffer, sizeof(buffer), "%s%lu.%02luuAh", negative ? "-" : "", (unsigned long)whole, (unsigned long)hundredths);
  uint16_t uah_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)(BOT_UAH_CENTER_X + (uah_width / 2U)), uah_width),
                      FACE_BOT_BASELINE, buffer, 1U);

  /* Uptime, bottom-right, prefixed with "UP:". */
  char uptime_duration[24];
  format_duration_dhm(g_epoch_seconds - g_boot_epoch, uptime_duration, sizeof(uptime_duration));
  snprintf(buffer, sizeof(buffer), "UP: %s", uptime_duration);
  uint16_t uptime_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(BOT_UPTIME_RIGHT_X, uptime_width), FACE_BOT_BASELINE, buffer, 1U);

  /* Time to empty / full, one row above. Skipped on the Live Current face,
     where that spot (y=105) holds the chart's "now" label. */
  //if (!g_live_current_mode) {
    draw_time_remaining();
  //}
}

/* Top-right: the clock in the small font, "H:MM:SS AM". */
static void draw_top_right_clock(void)
{
  char buffer[16];
  uint32_t hours24 = (g_epoch_seconds % 86400U) / 3600U;
  uint32_t minutes = (g_epoch_seconds % 3600U) / 60U;
  uint32_t seconds = g_epoch_seconds % 60U;
  uint8_t h12 = (uint8_t)(hours24 % 12U);
  if (h12 == 0U) {
    h12 = 12U;
  }
  snprintf(buffer, sizeof(buffer), "%u:%02u:%02u %s", (unsigned)h12, (unsigned)minutes, (unsigned)seconds,
           (hours24 >= 12U) ? "PM" : "AM");
  uint16_t width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(TOP_CLOCK_RIGHT_X, width), FACE_TOP_BASELINE, buffer, 1U);
}

/* Value (uA) -> pixel row inside the plot (higher value = smaller y). */
static int16_t schart_y(float value, float vmin, float span)
{
  float plot_h = (float)((SCHART_Y_BOTTOM - SCHART_Y_TOP) - 4);
  return (int16_t)((float)(SCHART_Y_BOTTOM - 2) - (((value - vmin) * plot_h) / span) + 0.5f);
}

static int16_t schart_solar_y(float value, float vmin, float span)
{
  float plot_h = (float)((SCHART_Y_BOTTOM - SCHART_Y_TOP) - 4);
  return (int16_t)((float)(SCHART_Y_BOTTOM - 2) - (((value - vmin) * plot_h) / span) + 0.5f);
}

/* Live Current chart over the last g_cur_count (<= 7200) one-second samples
   (raw AvgCurrent readings, in uA), auto-scaled in Y and
   stretched to fill the plot width in X.
     n <= plot width : one point per sample, joined by lines.
     n >  plot width : each pixel column draws the min..max envelope of the
                       samples that fall in it, joined to the previous column's
                       last value, so spikes survive the decimation.
   Above the plot, left: the current-axis unit. Right: latest AvgCurrent and
   the latest minute's supercap-estimated current. */
static void draw_current_chart(void)
{
  static float col_min[SCHART_PLOT_W];
  static float col_max[SCHART_PLOT_W];
  static float col_last[SCHART_PLOT_W];
  static float solar_col_min[SCHART_PLOT_W];
  static float solar_col_max[SCHART_PLOT_W];
  const int16_t y_top = SCHART_Y_TOP;
  const int16_t y_bottom = SCHART_Y_BOTTOM;
  const int16_t y_mid = (int16_t)(y_top + ((y_bottom - y_top) / 2));
  const uint16_t n = g_cur_count;
  char buffer[24];
  char value[16];

  /* Latest reading, top right of the graph. */
  format_fixed_float(g_current_ua, 2U, value, sizeof(value));
  snprintf(buffer, sizeof(buffer), "%suA", value);
  uint16_t reading_w = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(247U, reading_w), SCHART_READOUT_BASELINE, buffer, 1U);
  format_fixed_float(g_est_current_ua, 1U, value, sizeof(value));
  snprintf(buffer, sizeof(buffer), "%suA", value);
  reading_w = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(247U, reading_w),
                      (uint16_t)(SCHART_READOUT_BASELINE + 12U), buffer, 1U);

  /* Axis bars (2 px) and Y ticks, same look as the history charts. */
  fb_draw_rect((uint16_t)SCHART_AXIS_X, (uint16_t)y_top, 2U, (uint16_t)(y_bottom - y_top + 2), 1U);
  fb_draw_rect((uint16_t)SCHART_AXIS_X, (uint16_t)y_bottom, (uint16_t)(SCHART_PLOT_X1 - SCHART_AXIS_X), 2U, 1U);
  fb_draw_line((int16_t)(SCHART_AXIS_X - 4), y_top, SCHART_AXIS_X, y_top, 1U);
  fb_draw_line((int16_t)(SCHART_AXIS_X - 4), y_mid, SCHART_AXIS_X, y_mid, 1U);
  fb_draw_line((int16_t)(SCHART_AXIS_X - 4), y_bottom, SCHART_AXIS_X, y_bottom, 1U);
  fb_draw_rect((uint16_t)SCHART_SOLAR_AXIS_X, (uint16_t)y_top, 2U, (uint16_t)(y_bottom - y_top + 2), 1U);
  fb_draw_line(SCHART_SOLAR_AXIS_X, y_top, (int16_t)(SCHART_SOLAR_AXIS_X + 4), y_top, 1U);
  fb_draw_line(SCHART_SOLAR_AXIS_X, y_mid, (int16_t)(SCHART_SOLAR_AXIS_X + 4), y_mid, 1U);
  fb_draw_line(SCHART_SOLAR_AXIS_X, y_bottom, (int16_t)(SCHART_SOLAR_AXIS_X + 4), y_bottom, 1U);

  if (n == 0U) {
    return;
  }

  /* Pass 1: global min/max (and the per-column envelope when decimating). */
  float vmin = 1.0e9f;
  float vmax = -1.0e9f;
  float solar_vmin = 1.0e9f;
  float solar_vmax = -1.0e9f;
  if (n > SCHART_PLOT_W) {
    for (uint16_t c = 0U; c < SCHART_PLOT_W; ++c) {
      uint16_t i0 = (uint16_t)(((uint32_t)c * n) / SCHART_PLOT_W);
      uint16_t i1 = (uint16_t)(((uint32_t)(c + 1U) * n) / SCHART_PLOT_W);
      float mn = 1.0e9f;
      float mx = -1.0e9f;
      float last = 0.0f;
      for (uint16_t k = i0; k < i1; ++k) {
        last = current_history_get(k);
        if (last < mn) {
          mn = last;
        }
        if (last > mx) {
          mx = last;
        }
      }
      col_min[c] = mn;
      col_max[c] = mx;
      col_last[c] = last;
      float solar_mn = 1.0e9f;
      float solar_mx = -1.0e9f;
      for (uint16_t k = i0; k < i1; ++k) {
        float solar_v = current_solar_history_get(k);
        if (solar_v < solar_mn) {
          solar_mn = solar_v;
        }
        if (solar_v > solar_mx) {
          solar_mx = solar_v;
        }
      }
      solar_col_min[c] = solar_mn;
      solar_col_max[c] = solar_mx;
      if (mn < vmin) {
        vmin = mn;
      }
      if (mx > vmax) {
        vmax = mx;
      }
      if (solar_mn < solar_vmin) {
        solar_vmin = solar_mn;
      }
      if (solar_mx > solar_vmax) {
        solar_vmax = solar_mx;
      }
    }
  } else {
    for (uint16_t k = 0U; k < n; ++k) {
      float v = current_history_get(k);
      float solar_v = current_solar_history_get(k);
      if (v < vmin) {
        vmin = v;
      }
      if (v > vmax) {
        vmax = v;
      }
      if (solar_v < solar_vmin) {
        solar_vmin = solar_v;
      }
      if (solar_v > solar_vmax) {
        solar_vmax = solar_v;
      }
    }
  }
  if ((solar_vmax - solar_vmin) < 0.02f) {
    float centre = (solar_vmax + solar_vmin) * 0.5f;
    solar_vmin = centre - 0.01f;
    solar_vmax = centre + 0.01f;
  }
  float solar_span = solar_vmax - solar_vmin;
  if ((vmax - vmin) < SCHART_MIN_SPAN_UA) {
    float centre = (vmax + vmin) * 0.5f;
    vmin = centre - (SCHART_MIN_SPAN_UA * 0.5f);
    vmax = vmin + SCHART_MIN_SPAN_UA;
  }
  float span = vmax - vmin;

  /* Y labels (max / mid / min). uA while everything is under 1000 uA, mA
     above that. Decimals follow the span so neighbouring labels differ. */
  float max_abs = (vmax > -vmin) ? vmax : -vmin;
  bool use_ma = (max_abs >= 1000.0f);
  float unit_div = use_ma ? 1000.0f : 1.0f;
  float span_units = span / unit_div;
  uint8_t decimals;
  if (use_ma) {
    decimals = (span_units >= 10.0f) ? 1U : ((span_units >= 1.0f) ? 2U : 3U);
    if ((decimals > 1U) && (max_abs >= 100000.0f)) {
      decimals = 1U;   /* keep 3-digit mA values inside the label column */
    }
  } else {
    decimals = (span_units >= 100.0f) ? 0U : ((span_units >= 10.0f) ? 1U : 2U);
  }
  fb_draw_string_u8g2(FONT_SMALL, 2U, SCHART_READOUT_BASELINE, use_ma ? "mA" : "uA", 1U);

  float label_value[3] = { vmax, (vmax + vmin) * 0.5f, vmin };
  int16_t label_y[3] = { (int16_t)(y_top + 4), (int16_t)(y_mid + 4), y_bottom };
  for (uint8_t i = 0U; i < 3U; ++i) {
    format_fixed_float(label_value[i] / unit_div, decimals, buffer, sizeof(buffer));
    uint16_t w = fb_string_width_u8g2(FONT_SMALL, buffer);
    fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)(SCHART_AXIS_X - 6), w), (uint16_t)label_y[i], buffer, 1U);
  }

  float solar_label_value[3] = { solar_vmax, (solar_vmax + solar_vmin) * 0.5f, solar_vmin };
  for (uint8_t i = 0U; i < 3U; ++i) {
    format_fixed_float(solar_label_value[i], 1U, buffer, sizeof(buffer));
    uint16_t w = fb_string_width_u8g2(FONT_SMALL, buffer);
    fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(247U, w), (uint16_t)label_y[i], buffer, 1U);
  }

  /* X labels: window length on the left, "now" on the right. */
  if (n <= 120U) {
    snprintf(buffer, sizeof(buffer), "-%us", (unsigned)n);
  } else {
    snprintf(buffer, sizeof(buffer), "-%um", (unsigned)(n / 60U));
  }
  fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(SCHART_AXIS_X - 2), (uint16_t)(y_bottom + 12), buffer, 1U);
  //uint16_t now_w = fb_string_width_u8g2(FONT_SMALL, "now");
  //fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(247U, now_w), (uint16_t)(y_bottom + 12), "now", 1U);

  /* Dotted zero line when the trace crosses zero (charge vs discharge). */
  if ((vmin < 0.0f) && (vmax > 0.0f)) {
    int16_t y0 = schart_y(0.0f, vmin, span);
    for (int16_t x = SCHART_PLOT_X0; x <= SCHART_PLOT_X1; x += 4) {
      fb_set_pixel((uint16_t)x, (uint16_t)y0, 1U);
    }
  }

  /* Pass 2: draw the trace. */
  if (n > SCHART_PLOT_W) {
    int16_t prev_y = -1;
    for (uint16_t c = 0U; c < SCHART_PLOT_W; ++c) {
      int16_t y_hi = schart_y(col_max[c], vmin, span);   /* top of the bar */
      int16_t y_lo = schart_y(col_min[c], vmin, span);   /* bottom of the bar */
      if (prev_y >= 0) {
        if (prev_y < y_hi) {
          y_hi = prev_y;
        } else if (prev_y > y_lo) {
          y_lo = prev_y;
        }
      }
      fb_draw_rect((uint16_t)(SCHART_PLOT_X0 + c), (uint16_t)y_hi, 1U, (uint16_t)(y_lo - y_hi + 1), 1U);
      prev_y = schart_y(col_last[c], vmin, span);
    }
  } else {
    int16_t prev_x = -1;
    int16_t prev_y = -1;
    for (uint16_t k = 0U; k < n; ++k) {
      int16_t x = (n > 1U) ? (int16_t)(SCHART_PLOT_X0 + (int32_t)(((uint32_t)k * (SCHART_PLOT_W - 1)) / (n - 1U)))
                           : (int16_t)SCHART_PLOT_X0;
      int16_t y = schart_y(current_history_get(k), vmin, span);
      if (prev_x >= 0) {
        fb_draw_line(prev_x, prev_y, x, y, 1U);
      } else {
        fb_set_pixel((uint16_t)x, (uint16_t)y, 1U);
      }
      prev_x = x;
      prev_y = y;
    }
  }

  /* The solar trace uses its own right-hand voltage scale and double-width strokes. */
  if (n > SCHART_PLOT_W) {
    for (uint16_t c = 0U; c < SCHART_PLOT_W; ++c) {
      int16_t y_hi = schart_solar_y(solar_col_max[c], solar_vmin, solar_span);
      int16_t y_lo = schart_solar_y(solar_col_min[c], solar_vmin, solar_span);
      if (y_hi > y_lo) {
        int16_t temp = y_hi;
        y_hi = y_lo;
        y_lo = temp;
      }
      fb_draw_rect((uint16_t)(SCHART_PLOT_X0 + c), (uint16_t)y_hi, 2U,
                   (uint16_t)(y_lo - y_hi + 2), 1U);
    }
  } else {
    int16_t prev_x = -1;
    int16_t prev_y = -1;
    for (uint16_t k = 0U; k < n; ++k) {
      int16_t x = (n > 1U) ? (int16_t)(SCHART_PLOT_X0 + (int32_t)(((uint32_t)k * (SCHART_PLOT_W - 1)) / (n - 1U)))
                           : (int16_t)SCHART_PLOT_X0;
      int16_t y = schart_solar_y(current_solar_history_get(k), solar_vmin, solar_span);
      if (prev_x >= 0) {
        fb_draw_line(prev_x, prev_y, x, y, 1U);
        fb_draw_line(prev_x, (int16_t)(prev_y + 1), x, (int16_t)(y + 1), 1U);
      } else {
        fb_set_pixel((uint16_t)x, (uint16_t)y, 1U);
        fb_set_pixel((uint16_t)x, (uint16_t)(y + 1), 1U);
      }
      prev_x = x;
      prev_y = y;
    }
  }
}

void draw_live_current_face(void)
{
  fb_clear();
  draw_background();
  draw_battery_level_mask();
  draw_top_left_celsius(g_temperature_c);
  draw_top_right_clock();
  draw_current_chart();
  draw_readouts();
}

/* Top-right of the Seconds and Low Power faces: the last MAX17055 AvgCurrent reading in uA with
   two decimals ("20.00uA"). It only changes when the MAX17055 is read, i.e.
   once a minute, which is exactly when this face is fully redrawn. */
static void draw_top_right_current(void)
{
  char value[16];
  char buffer[20];
  format_fixed_float(g_current_ua, 2U, value, sizeof(value));
  snprintf(buffer, sizeof(buffer), "%suA", value);
  uint16_t width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(TOP_CURRENT_RIGHT_X, width), FACE_TOP_BASELINE, buffer, 1U);
}

static void draw_estimated_current(void)
{
  char value[16];
  char buffer[20];
  format_fixed_float(g_est_current_ua, 1U, value, sizeof(value));
  snprintf(buffer, sizeof(buffer), "%suA", value);
  uint16_t width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(TOP_CURRENT_RIGHT_X, width),
                      (uint16_t)(FACE_TOP_BASELINE + 15U), buffer, 1U);
}

/*
 * Seconds face: big ticking clock. Full redraw once a minute, then only the
 * seconds digits are redrawn and sent.
 */
void draw_clock_face_full(void)
{
  fb_clear();
  draw_background();
  draw_battery_level_mask();

  uint32_t hours24 = (g_epoch_seconds % 86400U) / 3600U;
  uint32_t minutes = (g_epoch_seconds % 3600U) / 60U;
  uint32_t seconds = g_epoch_seconds % 60U;

  uint8_t h12 = (uint8_t)(hours24 % 12U);
  if (h12 == 0U) {
    h12 = 12U;
  }
  bool pm = hours24 >= 12U;

  /* AM/PM tag: AM level with the digit tops, PM level with the baseline. */
  fb_draw_string_u8g2(FONT_SMALL, SEC_AMPM_X,
                      pm ? FACE_BIG_BASELINE : (uint16_t)(FACE_BIG_BASELINE - FACE_BIG_ASCENT + 10U),
                      pm ? "PM" : "AM", 1U);

  /* "H:MM:" right-justified against the fixed seconds position; the hour grows
     leftwards ("1:05:" vs "12:05:"). */
  char time_buf[12];
  char secs_buf[4];
  snprintf(time_buf, sizeof(time_buf), "%u:%02u:", (unsigned)h12, (unsigned)minutes);
  snprintf(secs_buf, sizeof(secs_buf), "%02u", (unsigned)seconds);

  uint16_t time_width = fb_string_width_u8g2(FONT_BIG, time_buf);
  fb_draw_string_u8g2(FONT_BIG, right_aligned_x(FACE_SEC_X, time_width), FACE_BIG_BASELINE, time_buf, 1U);
  fb_draw_string_u8g2(FONT_BIG, FACE_SEC_X, FACE_BIG_BASELINE, secs_buf, 1U);

  s_displayed_sec_tens = (uint8_t)(seconds / 10U);
  g_sec_partial_x = SEC_RECT_X;
  g_sec_partial_w = SEC_RECT_W;

  draw_top_left_celsius(g_temperature_c);
  draw_top_right_current();
  draw_estimated_current();
  draw_readouts();
}

void draw_clock_face_seconds(void)
{
  uint32_t seconds = g_epoch_seconds % 60U;
  uint8_t tens = (uint8_t)(seconds / 10U);
  uint8_t units = (uint8_t)(seconds % 10U);

  if (tens == s_displayed_sec_tens) {
    /* Tens digit is unchanged (50 out of 60 seconds): repaint and DMA only
       the units digit sub-rectangle. */
    char tens_buf[2] = { (char)('0' + tens), '\0' };
    char units_buf[2] = { (char)('0' + units), '\0' };
    uint16_t tens_w = fb_string_width_u8g2(FONT_BIG, tens_buf);
    uint16_t units_x = (uint16_t)(FACE_SEC_X + tens_w);
    uint16_t units_w = (uint16_t)((SEC_RECT_X + SEC_RECT_W) - units_x);

    fb_draw_rect(units_x, SEC_RECT_Y, units_w, SEC_RECT_H, 0U);
    fb_draw_string_u8g2(FONT_BIG, units_x, FACE_BIG_BASELINE, units_buf, 1U);

    g_sec_partial_x = units_x;
    g_sec_partial_w = units_w;
  } else {
    /* Tens digit rolled over (10 out of 60 seconds): repaint and DMA the
       full two-digit seconds window. */
    char secs_buf[4];
    snprintf(secs_buf, sizeof(secs_buf), "%02u", (unsigned)seconds);

    fb_draw_rect(SEC_RECT_X, SEC_RECT_Y, SEC_RECT_W, SEC_RECT_H, 0U);
    fb_draw_string_u8g2(FONT_BIG, FACE_SEC_X, FACE_BIG_BASELINE, secs_buf, 1U);

    s_displayed_sec_tens = tens;
    g_sec_partial_x = SEC_RECT_X;
    g_sec_partial_w = SEC_RECT_W;
  }
}

/*
 * Low-power clock face. Nothing ticks; redrawn once a minute (main.c):
 *   - H:MM in FONT_BIG_LP at the left with a small AM (top) / PM (bottom) tag,
 *   - temperature in the same font right-justified, with a small "°C" up and
 *     to the right of it,
 *   - humidex in the top-left slot where the seconds face shows temperature,
 *   - the usual readouts.
 * (The 384x168 layout had the temperature left and the time right; at 250 px
 * wide that order doesn't fit with AM/PM, so the time takes the left side.)
 */
void draw_clock_face_low_power(void)
{
  char buffer[32];

  fb_clear();
  draw_background();
  draw_battery_level_mask();

  uint32_t hours24 = (g_epoch_seconds % 86400U) / 3600U;
  uint32_t minutes = (g_epoch_seconds % 3600U) / 60U;
  uint8_t h12 = (uint8_t)(hours24 % 12U);
  if (h12 == 0U) {
    h12 = 12U;
  }
  bool pm = hours24 >= 12U;

  uint16_t digit_top = fb_font_glyph_top(FONT_BIG_LP, '0');
  uint16_t big_top_y = (uint16_t)(LP_BIG_BASELINE_Y - digit_top);

  /* Time, no leading zero on the hour - change "%u:%02u" to "%02u:%02u" for a
     zero-padded hour. */
  snprintf(buffer, sizeof(buffer), "%u:%02u", (unsigned)h12, (unsigned)minutes);
  uint16_t time_width = big_text_width_tracked(buffer);
  draw_big_text_tracked(LP_TIME_X, LP_BIG_BASELINE_Y, buffer);

  /* AM/PM tag right after the time: AM level with the digit tops, PM with the baseline. */
  fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(LP_TIME_X + time_width + LP_AMPM_GAP_X),
                      pm ? LP_BIG_BASELINE_Y : (uint16_t)(big_top_y + 10U), pm ? "PM" : "AM", 1U);

  /* Temperature, right-justified so its degree mark lands on LP_UNIT_X. */
  format_fixed_float(g_temperature_c, 1U, buffer, sizeof(buffer));
  uint16_t temp_width = big_text_width_tracked(buffer);
  uint16_t temp_x = right_aligned_x((uint16_t)(LP_UNIT_X - LP_UNIT_GAP_X), temp_width);
  draw_big_text_tracked(temp_x, LP_BIG_BASELINE_Y, buffer);

  fb_draw_degree_mark(LP_UNIT_X, big_top_y);
  fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(LP_UNIT_X + 6U), (uint16_t)(big_top_y + 12U), "C", 1U);

  /* Top-left: humidex where the seconds face has the temperature. */
  draw_top_left_celsius(calculate_humidex(g_temperature_c, g_humidity_pct));

  /* Top-right: same AvgCurrent readout as the Seconds face. */
  draw_top_right_current();
  draw_estimated_current();

  draw_readouts();
}

void draw_menu_screen(void)
{
  fb_clear();
  for (uint8_t i = 0U; i < MENU_VISIBLE; ++i) {
    int8_t idx = (int8_t)(g_menu_scroll_offset + i);
    if (idx >= (int8_t)NUM_MENU_ITEMS) {
      break;
    }
    uint16_t y = (uint16_t)(14U + (i * MENU_ROW_PITCH));   /* text baseline */
    fb_draw_string_u8g2(FONT_SMALL, 14U, y, menu_items[idx], 1U);
    if (idx == g_menu_selection) {
      fb_draw_frame(4U, (uint16_t)(y - 12U), 242U, 16U, 1U);
    }
  }
}

static uint32_t build_epoch_from_temp_time(uint32_t current_epoch, uint8_t hour, uint8_t minute)
{
  uint32_t day_start = current_epoch - (current_epoch % 86400U);
  return day_start + ((uint32_t)hour * 3600U) + ((uint32_t)minute * 60U);
}

void draw_set_time_screen(void)
{
  char buffer[16];
  fb_clear();
  const char *title = "Set System Time";
  uint16_t title_width = fb_string_width_u8g2(FONT_SMALL, title);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)((DISPLAY_WIDTH + title_width) / 2U), title_width), 20U, title, 1U);

  /* "HH:MM" in FONT_BIG: 4 digits x 30 px + colon 17 px = 137 px, centred. */
  snprintf(buffer, sizeof(buffer), "%02u:%02u", g_temp_hour, g_temp_minute);
  uint16_t time_width = fb_string_width_u8g2(FONT_BIG, buffer);
  uint16_t x_offset = (uint16_t)((DISPLAY_WIDTH - time_width) / 2U);
  const uint16_t baseline = 82U;
  fb_draw_string_u8g2(FONT_BIG, x_offset, baseline, buffer, 1U);

  /* Underline the field being edited: hours = first two digits (60 px),
     minutes start after the colon (60 + 17 px). */
  if (!g_editing_minutes) {
    fb_draw_rect((uint16_t)(x_offset + 2U), (uint16_t)(baseline + 8U), 56U, 4U, 1U);
  } else {
    fb_draw_rect((uint16_t)(x_offset + 77U + 2U), (uint16_t)(baseline + 8U), 56U, 4U, 1U);
  }
}

/*
 * Ported layout-for-layout from ST7305clock.cpp's drawChart(), including
 * details the first pass dropped: the filled 2px axis bars with tick marks,
 * right-aligned Y labels at per-chart-type precision, the 0/25/50/75/100%
 * X-axis ticks, and the dotted crosshair + circle + "value @ HH:MM" cursor
 * label. The .cpp bails out before drawing anything (including the title)
 * when there's no history yet, so this does too.
 */
void draw_chart_screen(void)
{
  fb_clear();

  uint16_t c_count = (g_chart_index == CHART_VOLTAGE) ? g_volt_count : g_hist_count;
  if (c_count == 0U) {
    return;
  }

  float min_value = 99999.0f;
  float max_value = -99999.0f;
  for (uint16_t i = 0U; i < c_count; ++i) {
    float value = get_history_value((uint8_t)g_chart_index, i);
    if (value < min_value) {
      min_value = value;
    }
    if (value > max_value) {
      max_value = value;
    }
  }
  /* Keep the voltage charts' small natural range from being padded like
     temperature and current charts. */
  if ((max_value - min_value) < 0.0001f) {
    /* USER CODE BEGIN VddaChartRange */
    if ((g_chart_index == CHART_VOLTAGE) || (g_chart_index == CHART_SOLAR) ||
        (g_chart_index == CHART_VDDA)) {
      max_value += 0.005f;
      min_value -= 0.005f;
    } else {
      max_value += 1.0f;
      min_value -= 1.0f;
    }
    /* USER CODE END VddaChartRange */
  }

  /* Chart title reuses the menu item strings directly (menu_items[g_chart_index]),
     same as the .cpp's menuItems[chartIndex] - keeps names like "Rel Humidity"
     consistent between the menu and its chart. */
  {
    /* The current history is stored and plotted in uA (it used to be mA), so
       say so in the title. */
    char title[24];
    /* USER CODE BEGIN VddaChartTitleUnits */
    snprintf(title, sizeof(title), "%s%s", menu_items[g_chart_index],
             (((uint8_t)g_chart_index == CHART_CURRENT) ||
              ((uint8_t)g_chart_index == CHART_EST_CURRENT)) ? " (uA)" :
             (((uint8_t)g_chart_index == CHART_SOLAR) ||
              ((uint8_t)g_chart_index == CHART_VOLTAGE) ||
              ((uint8_t)g_chart_index == CHART_VDDA)) ? " (V)" : "");
    /* USER CODE END VddaChartTitleUnits */
    fb_draw_string_u8g2(FONT_SMALL, 4U, 12U, title, 1U);
  }

  /* 250x122 layout. x_start=64 leaves room for the widest Y-axis label
     ("1013.250" = 8 x 7 px); the title and the cursor readout share the top
     row (title left, readout right-aligned); X tick labels sit under y_bottom
     with the baseline at y_bottom+12 = 118. */
  const int16_t x_start = 64;
  const int16_t x_end = 244;
  const int16_t y_top = 22;
  const int16_t y_bottom = 106;

  /* Axes as filled 2px-thick bars, matching u8g2.drawBox() in the .cpp. */
  fb_draw_rect((uint16_t)x_start, (uint16_t)y_top, 2U, (uint16_t)(y_bottom - y_top + 2), 1U);
  fb_draw_rect((uint16_t)x_start, (uint16_t)y_bottom, (uint16_t)(x_end - x_start), 2U, 1U);

  char buffer[32];
  int16_t y_mid = (int16_t)(y_top + (y_bottom - y_top) / 2);
  float mid_value = (max_value + min_value) / 2.0f;

  /* Y-axis tick marks. */
  fb_draw_line((int16_t)(x_start - 4), y_top, x_start, y_top, 1U);
  fb_draw_line((int16_t)(x_start - 4), y_mid, x_start, y_mid, 1U);
  fb_draw_line((int16_t)(x_start - 4), y_bottom, x_start, y_bottom, 1U);

  /* Y-axis labels, right-aligned to their ticks, at per-chart-type precision. */
  format_chart_value((uint8_t)g_chart_index, max_value, buffer, sizeof(buffer));
  uint16_t label_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)(x_start - 6), label_width), (uint16_t)(y_top + 4), buffer, 1U);

  format_chart_value((uint8_t)g_chart_index, mid_value, buffer, sizeof(buffer));
  label_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)(x_start - 6), label_width), (uint16_t)(y_mid + 4), buffer, 1U);

  format_chart_value((uint8_t)g_chart_index, min_value, buffer, sizeof(buffer));
  label_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  fb_draw_string_u8g2(FONT_SMALL, right_aligned_x((uint16_t)(x_start - 6), label_width), (uint16_t)y_bottom, buffer, 1U);

  /* X-axis ticks & labels: start (0), end (sample count), and 25/50/75%. */
  fb_draw_line(x_start, y_bottom, x_start, (int16_t)(y_bottom + 4), 1U);
  fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(x_start - 2), (uint16_t)(y_bottom + 12), "0", 1U);

  fb_draw_line(x_end, y_bottom, x_end, (int16_t)(y_bottom + 4), 1U);
  snprintf(buffer, sizeof(buffer), "%u", (unsigned)c_count);
  uint16_t end_width = fb_string_width_u8g2(FONT_SMALL, buffer);
  /* Centre on the tick, but keep the last label inside the 250 px canvas. */
  {
    int16_t end_x = (int16_t)(x_end - (int16_t)(end_width / 2U));
    if ((end_x + (int16_t)end_width) > (int16_t)DISPLAY_WIDTH) {
      end_x = (int16_t)(DISPLAY_WIDTH - end_width);
    }
    fb_draw_string_u8g2(FONT_SMALL, (uint16_t)end_x, (uint16_t)(y_bottom + 12), buffer, 1U);
  }

  for (int16_t i = 1; i <= 3; ++i) {
    int16_t tick_x = (int16_t)(x_start + (i * (x_end - x_start)) / 4);
    uint16_t tick_val = (uint16_t)((i * (int32_t)c_count) / 4);
    fb_draw_line(tick_x, y_bottom, tick_x, (int16_t)(y_bottom + 4), 1U);
    snprintf(buffer, sizeof(buffer), "%u", (unsigned)tick_val);
    uint16_t tick_width = fb_string_width_u8g2(FONT_SMALL, buffer);
    fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(tick_x - tick_width / 2), (uint16_t)(y_bottom + 12), buffer, 1U);
  }

  /* Data polyline. The +2/-4 padding on x and the -2/-4 on y keep the trace
     clear of the axis bars, same as the .cpp. */
  int16_t prev_x = -1;
  int16_t prev_y = -1;
  int16_t cursor_x = -1;
  int16_t cursor_y = -1;
  float cursor_value = 0.0f;

  if (g_chart_cursor >= (int16_t)c_count) {
    g_chart_cursor = (int16_t)(c_count - 1U);
  }
  if ((g_chart_cursor < 0) && (c_count > 0U)) {
    g_chart_cursor = (int16_t)(c_count - 1U);
  }

  for (uint16_t i = 0U; i < c_count; ++i) {
    float value = get_history_value((uint8_t)g_chart_index, i);
    int16_t x = (int16_t)(x_start + 2 + (int32_t)(i * (x_end - x_start - 4)) / (c_count > 1U ? (int32_t)(c_count - 1U) : 1));
    int16_t y = (int16_t)(y_bottom - 2 - (int16_t)(((value - min_value) * (float)(y_bottom - y_top - 4)) / (max_value - min_value)));

    if (prev_x != -1) {
      fb_draw_line(prev_x, prev_y, x, y, 1U);
    }
    prev_x = x;
    prev_y = y;

    if ((int16_t)i == g_chart_cursor) {
      cursor_x = x;
      cursor_y = y;
      cursor_value = value;
    }
  }

  /* Cursor: dotted crosshair + circle marker + "value @ HH:MM" label. */
  if (cursor_x != -1) {
    for (int16_t cy = y_top; cy < y_bottom; cy += 4) {
      fb_set_pixel((uint16_t)cursor_x, (uint16_t)cy, 1U);
    }
    for (int16_t cx = x_start; cx < x_end; cx += 4) {
      fb_set_pixel((uint16_t)cx, (uint16_t)cursor_y, 1U);
    }
    fb_draw_circle(cursor_x, cursor_y, 3);

    /* Real timestamp for the cursor point: Voltage samples on an hourly
       tick, everything else on a per-minute tick, matching the .cpp. */
    int32_t time_ago = (int32_t)(c_count - 1U) - (int32_t)g_chart_cursor;
    uint32_t point_time = (g_chart_index == CHART_VOLTAGE)
                             ? (g_epoch_seconds - ((uint32_t)time_ago * 3600U))
                             : (g_epoch_seconds - ((uint32_t)time_ago * 60U));
    uint32_t h24 = (point_time % 86400U) / 3600U;
    uint32_t m = (point_time % 3600U) / 60U;

    char value_str[16];
    format_chart_value_cursor((uint8_t)g_chart_index, cursor_value, value_str, sizeof(value_str));
    char label[48];
    snprintf(label, sizeof(label), "%s @ %02lu:%02lu", value_str, (unsigned long)h24, (unsigned long)m);
    uint16_t label_w = fb_string_width_u8g2(FONT_SMALL, label);

    /* Top row, right-aligned: the title owns the left side, and the crosshair
       only spans y_top..y_bottom so it can never overlap this text. */
    fb_draw_string_u8g2(FONT_SMALL, right_aligned_x(247U, label_w), 12U, label, 1U);
  }
}

/* LSBs for the info screen, derived from the shared Rsense in clock_port.h. */
#define MAX17055_UI_RSENSE_OHMS      MAX17055_RSENSE_OHMS
#define MAX17055_UI_VOLTAGE_LSB_V    MAX17055_VOLTAGE_LSB_V
#define MAX17055_UI_CURRENT_LSB_UA   MAX17055_CURRENT_LSB_UA
#define MAX17055_UI_CAPACITY_LSB_MAH (MAX17055_CAPACITY_LSB_UAH / 1000.0f)
#define MAX17055_UI_TEMP_LSB_C       (1.0f / 256.0f)
#define MAX17055_UI_TIME_LSB_S       5.625f
/* MaxMinCurrent packs two independent signed 8-bit fields (max in the high
   byte, min in the low byte) with its own, coarser LSB - 0.4mA/Rsense,
   vs. 1.5625uV/Rsense for the regular Current/AvgCurrent registers. */
#define MAX17055_UI_MAXMIN_CURRENT_LSB_MA (0.4f / MAX17055_UI_RSENSE_OHMS)
/* Cycles counts in units of 1% of a full charge/discharge cycle. */
#define MAX17055_UI_CYCLES_LSB       0.01f
/* TTE/TTF read back as this exact value while the corresponding estimate
   isn't valid (e.g. TTF with nothing charging) - show "--:--" instead of
   a meaningless multi-year countdown. */
#define MAX17055_TIME_INVALID        0xFFFFU

typedef enum {
  MAX17055_FMT_PERCENT_256,   /* value/256 = percent - RepSOC, Age */
  MAX17055_FMT_CAPACITY_MAH,
  MAX17055_FMT_CURRENT_MA,
  MAX17055_FMT_VOLTAGE,
  MAX17055_FMT_TEMP_C,
  MAX17055_FMT_TIME_HHMM,
  MAX17055_FMT_CYCLES,
  MAX17055_FMT_MAXCURRENT_MA, /* high byte of MaxMinCurrent */
  MAX17055_FMT_MINCURRENT_MA  /* low byte of MaxMinCurrent */
} max17055_reg_format_t;

typedef struct {
  uint8_t addr;
  const char *name;
  max17055_reg_format_t fmt;
} max17055_reg_info_t;

void draw_max17055_screen(void)
{
  fb_clear();
  fb_draw_string_u8g2(FONT_SMALL, 4U, 12U, "MAX17055", 1U);
  /* USER CODE BEGIN VddaMaxInfo */
  char vdda_str[12];
  char vdda_line[24];
  format_fixed_float(g_vdda_voltage, 3U, vdda_str, sizeof(vdda_str));
  snprintf(vdda_line, sizeof(vdda_line), "VDDA %sV", vdda_str);
  fb_draw_string_u8g2(FONT_SMALL, 130U, 112U, vdda_line, 1U);
  /* USER CODE END VddaMaxInfo */
  if (!g_max17055_regs_valid) {
    fb_draw_string_u8g2(FONT_SMALL, 4U, 32U, "No MAX17055 data", 1U);
    fb_draw_string_u8g2(FONT_SMALL, 4U, 48U, "Power sensors and retry", 1U);
    return;
  }

  /* Only the values someone actually reads at a glance - no raw hex,
     no config/bitfield registers (Status, RelaxCfg, VEmpty, FStat,
     HibCfg, ModelCfg, DesignCap, IChgTerm - all still used internally by
     max17055_configure(), just not shown here). MinCurrent/MaxCurrent
     both come from the single MaxMinCurrent register (high byte / low
     byte) rather than their own address. */
  static const max17055_reg_info_t regs[] = {
    { 0x06U, "RepSOC",  MAX17055_FMT_PERCENT_256 },
    { 0x05U, "RepCap",  MAX17055_FMT_CAPACITY_MAH },
    { 0x10U, "FullCap", MAX17055_FMT_CAPACITY_MAH },
    { 0x16U, "AvgTmp",  MAX17055_FMT_TEMP_C },
    { 0x0AU, "Cur",     MAX17055_FMT_CURRENT_MA },
    { 0x0BU, "AvgCur",  MAX17055_FMT_CURRENT_MA },
    { 0x11U, "TTE",     MAX17055_FMT_TIME_HHMM },
    { 0x20U, "TTF",     MAX17055_FMT_TIME_HHMM },
    { 0x17U, "Cycles",  MAX17055_FMT_CYCLES },
    { 0x07U, "Age",     MAX17055_FMT_PERCENT_256 },
    { 0x19U, "AvgVCel", MAX17055_FMT_VOLTAGE },
    { 0x1CU, "MinCur",  MAX17055_FMT_MINCURRENT_MA },
    { 0x1CU, "MaxCur",  MAX17055_FMT_MAXCURRENT_MA }
  };

  for (uint8_t i = 0U; i < (uint8_t)(sizeof(regs) / sizeof(regs[0])); ++i) {
    uint8_t row = i % 7U;
    uint8_t col = i / 7U;
    char line[24];
    char value_str[12];
    uint16_t raw = g_max17055_regs[regs[i].addr];

    switch (regs[i].fmt) {
      case MAX17055_FMT_VOLTAGE:
        format_fixed_float((float)raw * MAX17055_UI_VOLTAGE_LSB_V, 3U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %sV", regs[i].name, value_str);
        break;
      case MAX17055_FMT_CURRENT_MA:
        format_fixed_float((float)(int16_t)raw * MAX17055_UI_CURRENT_LSB_UA, 1U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %suA", regs[i].name, value_str);
        break;
      case MAX17055_FMT_CAPACITY_MAH:
        format_fixed_float((float)raw * MAX17055_UI_CAPACITY_LSB_MAH, 3U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %smAh", regs[i].name, value_str);
        break;
      case MAX17055_FMT_PERCENT_256:
        format_fixed_float((float)raw / 256.0f, 1U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %s%%", regs[i].name, value_str);
        break;
      case MAX17055_FMT_TEMP_C:
        format_fixed_float((float)(int16_t)raw * MAX17055_UI_TEMP_LSB_C, 1U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %sC", regs[i].name, value_str);
        break;
      case MAX17055_FMT_TIME_HHMM:
        if (raw == MAX17055_TIME_INVALID) {
          snprintf(line, sizeof(line), "%s --:--", regs[i].name);
        } else {
          uint32_t total_secs = (uint32_t)((float)raw * MAX17055_UI_TIME_LSB_S);
          uint32_t hh = total_secs / 3600U;
          uint32_t mm = (total_secs % 3600U) / 60U;
          snprintf(line, sizeof(line), "%s %lu:%02lu", regs[i].name, (unsigned long)hh, (unsigned long)mm);
        }
        break;
      case MAX17055_FMT_CYCLES:
        format_fixed_float((float)raw * MAX17055_UI_CYCLES_LSB, 2U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %s", regs[i].name, value_str);
        break;
      case MAX17055_FMT_MAXCURRENT_MA: {
        int8_t max_byte = (int8_t)((raw >> 8) & 0xFFU);
        format_fixed_float((float)max_byte * MAX17055_UI_MAXMIN_CURRENT_LSB_MA, 1U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %smA", regs[i].name, value_str);
        break;
      }
      case MAX17055_FMT_MINCURRENT_MA: {
        int8_t min_byte = (int8_t)(raw & 0xFFU);
        format_fixed_float((float)min_byte * MAX17055_UI_MAXMIN_CURRENT_LSB_MA, 1U, value_str, sizeof(value_str));
        snprintf(line, sizeof(line), "%s %smA", regs[i].name, value_str);
        break;
      }
      default:
        line[0] = '\0';
        break;
    }
    /* 2 columns x 7 rows: longest line ("FullCap 123.4mAh") is 112 px, column pitch 126. */
    fb_draw_string_u8g2(FONT_SMALL, (uint16_t)(4U + (col * 126U)), (uint16_t)(28U + (row * 14U)), line, 1U);
  }
}

static void reset_history_data(void)
{
  memset(g_hist_temp, 0, sizeof(g_hist_temp));
  memset(g_hist_rh, 0, sizeof(g_hist_rh));
  memset(g_hist_abs_hum, 0, sizeof(g_hist_abs_hum));
  memset(g_hist_volt, 0, sizeof(g_hist_volt));
  memset(g_hist_cur, 0, sizeof(g_hist_cur));
  memset(g_hist_est_cur, 0, sizeof(g_hist_est_cur));
  memset(g_hist_solar, 0, sizeof(g_hist_solar));
  /* USER CODE BEGIN VddaHistoryReset */
  memset(g_hist_vdda, 0, sizeof(g_hist_vdda));
  /* USER CODE END VddaHistoryReset */
  g_hist_head = 0U;
  g_hist_count = 0U;
  g_volt_head = 0U;
  g_volt_count = 0U;
  g_volt_sample_counter = 0U;
  current_history_reset();
  /* The displayed uAh is an absolute AvgVCell-derived value, so it is not
     zeroed here; only the dV/dt baseline and estimated current restart. */
  vcap_estimator_reset();
}

/* Switches between the three clock faces: Seconds (neither flag), Low Power,
   Live Current. Entering Live Current starts a fresh chart (its samples must
   be 1 s apart) and takes the MAX17055 out of Hibernate; leaving it puts the
   gauge's Hibernate setting back. */
static void set_display_mode(bool low_power, bool live_current)
{
  if (live_current && !g_live_current_mode) {
    current_history_reset();
    max17055_set_live_mode(true);
  } else if (!live_current && g_live_current_mode) {
    max17055_set_live_mode(false);
  }
  g_low_power_mode = low_power;
  g_live_current_mode = live_current;
  g_main_needs_full_redraw = true;
}

/* Back to the clock face (whichever mode g_low_power_mode selects) with a
   full redraw. */
static void return_to_main_screen(void)
{
  g_app_state = STATE_MAIN;
  g_main_needs_full_redraw = true;
  g_display_dirty = true;
}

void handle_ui(void)
{
  /* The thumbwheel is wired backwards, so left/right are swapped here (and
     in main.c's startup read): left = PA3/PB3, right = PA6/PA9. */
  bool hard_left = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_3) == GPIO_PIN_RESET);    /* hard left */
  //bool slight_left = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_RESET); /* slight left */
  bool hard_right = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_RESET);   /* hard right */
  //bool slight_right = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) == GPIO_PIN_RESET); /* slight right */

  /* Hard and slight are combined per direction for now - either switch
     registers as that direction. */
  bool up_pressed = hard_left;// || slight_left;      /* combined left: menu up / left on graph */
  bool down_pressed = hard_right;// || slight_right;  /* combined right: menu down / right on graph */
  bool right_pressed = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_RESET);   /* center pushbutton = confirm */

  /* Hard switches auto-repeat while held in menu, chart, and set-time modes;
     slight switches stay a single step per press. */
  static bool s_hard_left_prev = false;
  static bool s_slight_left_prev = false;
  static bool s_hard_right_prev = false;
  static bool s_slight_right_prev = false;
  static bool s_left_repeat_armed = false;
  static bool s_right_repeat_armed = false;
  static uint32_t s_left_next_repeat_ms = 0U;
  static uint32_t s_right_next_repeat_ms = 0U;
  static app_state_t s_prev_app_state = STATE_MAIN;
  uint32_t now_ms = millis_now();
  bool step_left = false;
  bool step_right = false;

  /* On a screen change, treat whatever is already held as seen: prev = the
     current pin state and the repeat disarmed. A switch still down when the
     new screen appears then neither fires its "fresh press" step nor
     auto-repeats; it has to be released and pressed again. */
  if (g_app_state != s_prev_app_state) {
    s_hard_left_prev = hard_left;
    s_hard_right_prev = hard_right;
    s_left_repeat_armed = false;
    s_right_repeat_armed = false;
    s_prev_app_state = g_app_state;
  }


  if (!hard_left) {
    s_left_repeat_armed = false;
  } else if (!s_hard_left_prev) {
    /* Fresh falling edge: fire immediately and arm initial hold delay. */
    step_left = true;
    s_left_repeat_armed = true;
    s_left_next_repeat_ms = now_ms + UI_REPEAT_INITIAL_MS;
  } else if (s_left_repeat_armed &&
             (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_3) == GPIO_PIN_RESET) &&
             ((int32_t)(now_ms - s_left_next_repeat_ms) >= 0)) {
    step_left = true;
    s_left_next_repeat_ms = now_ms + UI_REPEAT_INTERVAL_MS;
  }


  if (!hard_right) {
    s_right_repeat_armed = false;
  } else if (!s_hard_right_prev) {
    /* Fresh falling edge: fire immediately and arm initial hold delay. */
    step_right = true;
    s_right_repeat_armed = true;
    s_right_next_repeat_ms = now_ms + UI_REPEAT_INITIAL_MS;
  } else if (s_right_repeat_armed &&
             (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_RESET) &&
             ((int32_t)(now_ms - s_right_next_repeat_ms) >= 0)) {
    step_right = true;
    s_right_next_repeat_ms = now_ms + UI_REPEAT_INTERVAL_MS;
  }

  s_hard_left_prev = hard_left;
  //s_slight_left_prev = slight_left;
  s_hard_right_prev = hard_right;
  //s_slight_right_prev = slight_right;

  /* The thumb-wheel has no separate "back" input anymore. The only way to
     leave a non-main screen is the pushbutton, so entry/selection/exit is
     handled entirely by right_pressed (the pushbutton) below. */
  if (g_app_state == STATE_SET_TIME) {
    if (step_left) {
      g_last_input_ms = millis_now();
      if (g_editing_minutes) {
        g_temp_minute = (uint8_t)((g_temp_minute < 59U) ? (g_temp_minute + 1U) : 0U);
      } else {
        g_temp_hour = (uint8_t)((g_temp_hour < 23U) ? (g_temp_hour + 1U) : 0U);
      }
      g_display_dirty = true;
    }
    if (step_right) {
      g_last_input_ms = millis_now();
      if (g_editing_minutes) {
        g_temp_minute = (uint8_t)((g_temp_minute > 0U) ? (g_temp_minute - 1U) : 59U);
      } else {
        g_temp_hour = (uint8_t)((g_temp_hour > 0U) ? (g_temp_hour - 1U) : 23U);
      }
      g_display_dirty = true;
    }
  } else {
    if (step_left) {
      g_last_input_ms = millis_now();
      if (g_app_state == STATE_MENU) {
        if (g_menu_selection > 0) {
          --g_menu_selection;
          if (g_menu_selection < g_menu_scroll_offset) {
            g_menu_scroll_offset = g_menu_selection;
          }
        }
        g_display_dirty = true;
      } else if (g_app_state == STATE_CHART) {
        uint16_t c_count = (g_chart_index == CHART_VOLTAGE) ? g_volt_count : g_hist_count;
        int16_t stride = (int16_t)((c_count > CHART_PLOT_PIXELS) ? (c_count / CHART_PLOT_PIXELS) : 1U);
        if (g_chart_cursor == -1) {
          g_chart_cursor = (int16_t)(c_count - 1U);
        } else {
          g_chart_cursor = (int16_t)((g_chart_cursor > stride) ? (g_chart_cursor - stride) : 0);
        }
        g_display_dirty = true;
      }
    }
    if (step_right) {
      g_last_input_ms = millis_now();
      if (g_app_state == STATE_MENU) {
        if (g_menu_selection < (int8_t)(NUM_MENU_ITEMS - 1)) {
          ++g_menu_selection;
          if (g_menu_selection > g_menu_scroll_offset + (MENU_VISIBLE - 1)) {
            g_menu_scroll_offset = (int8_t)(g_menu_selection - (MENU_VISIBLE - 1));
          }
        }
        g_display_dirty = true;
      } else if (g_app_state == STATE_CHART) {
        uint16_t c_count = (g_chart_index == CHART_VOLTAGE) ? g_volt_count : g_hist_count;
        int16_t stride = (int16_t)((c_count > CHART_PLOT_PIXELS) ? (c_count / CHART_PLOT_PIXELS) : 1U);
        int16_t last = (int16_t)(c_count - 1U);
        if (g_chart_cursor == -1) {
          g_chart_cursor = last;
        } else {
          g_chart_cursor = (int16_t)(((last - g_chart_cursor) > stride) ? (g_chart_cursor + stride) : last);
        }
        g_display_dirty = true;
      }
    }
  }
  if (right_pressed && !g_right_pressed_prev) {
    g_last_input_ms = millis_now();
    if (g_app_state == STATE_MAIN) {
      g_app_state = STATE_MENU;
      g_menu_selection = 0;
      g_menu_scroll_offset = 0;
      g_display_dirty = true;
    } else if (g_app_state == STATE_MENU) {
      if (g_menu_selection < NUM_CHART_MENU_ITEMS) {
        g_app_state = STATE_CHART;
        g_chart_cursor = -1;
        g_chart_index = (int8_t)g_menu_selection;
        g_display_dirty = true;
      } else {
        switch ((menu_item_t)g_menu_selection) {
          case MENU_LOW_POWER:
            set_display_mode(true, false);
            return_to_main_screen();
            break;
          case MENU_SECONDS:
            set_display_mode(false, false);
            return_to_main_screen();
            break;
          case MENU_LIVE_CURRENT:
            set_display_mode(false, true);
            return_to_main_screen();
            break;
          case MENU_MAX17055:
            (void)max17055_read_info_regs();
            g_app_state = STATE_MAX17055;
            g_display_dirty = true;
            break;
          case MENU_SET_TIME:
            g_app_state = STATE_SET_TIME;
            g_editing_minutes = false;
            g_temp_hour = (uint8_t)((g_epoch_seconds % 86400U) / 3600U);
            g_temp_minute = (uint8_t)((g_epoch_seconds % 3600U) / 60U);
            g_display_dirty = true;
            break;
          case MENU_RESET_DATA:
            reset_history_data();
            g_boot_epoch = g_epoch_seconds;
            return_to_main_screen();
            break;
          case MENU_RESET_SYSTEM:
            NVIC_SystemReset();
            break;
          default:
            break;
        }
      }
    } else if ((g_app_state == STATE_CHART) || (g_app_state == STATE_MAX17055)) {
      g_app_state = STATE_MAIN;
      g_main_needs_full_redraw = true;
      g_display_dirty = true;
    } else if (g_app_state == STATE_SET_TIME) {
      if (!g_editing_minutes) {
        g_editing_minutes = true;
        g_display_dirty = true;
      } else {
        uint32_t new_epoch = build_epoch_from_temp_time(g_epoch_seconds, g_temp_hour, g_temp_minute);

        /* g_epoch_seconds is about to jump from whatever the DS3231 was
           reading (often a garbage power-up default the very first time
           this runs after a cold boot) to the real time the user just
           entered. g_boot_epoch and g_last_sample_epoch are both
           "elapsed since X" baselines measured against the OLD clock;
           shift them by the same delta so they stay meaningful instead
           of going wildly wrong (uptime reads as years, or the once-a-
           minute RTC calibration sees a bogus/underflowed real_elapsed
           on its very next tick - which is what was making the seconds
           display sprint ahead right after the first Set Time). */
        int32_t epoch_delta = (int32_t)(new_epoch - g_epoch_seconds);
        g_boot_epoch = (uint32_t)((int32_t)g_boot_epoch + epoch_delta);
        g_last_sample_epoch = (uint32_t)((int32_t)g_last_sample_epoch + epoch_delta);

        /* 1. Turn on sensor power and allow the DS3231 to boot */
        HAL_GPIO_WritePin(GPIOB, PWR_SENSORS_PIN, GPIO_PIN_SET);
        delay_ms(40U); 

        /* 2. Initialize the I2C bus, write the time, and clean up */
        if (i2c_init()) {
            (void)ds3231_set_epoch(new_epoch);
            i2c_deinit();
        }

        /* 3. Turn the sensor power back off */
        HAL_GPIO_WritePin(GPIOB, PWR_SENSORS_PIN, GPIO_PIN_RESET);
        rtc_set_epoch(new_epoch);
        g_epoch_seconds = new_epoch;
        g_app_state = STATE_MAIN;
        g_main_needs_full_redraw = true;
        g_display_dirty = true;
      }
    }
  }

  g_up_pressed_prev = up_pressed;
  g_down_pressed_prev = down_pressed;
  g_right_pressed_prev = right_pressed;
}
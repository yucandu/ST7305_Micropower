/*
 * clock_port_i2c3.c
 * ---------------------------------------------------------------------------
 * Always-powered I2C3 devices: MAX17055 fuel gauge (used here only as a
 * current / battery-voltage / charge meter - its ModelGauge outputs are
 * ignored) and SHT40 (temperature/humidity). Plain HAL calls, no class state.
 *
 * Every function that touches the bus expects the caller to have called
 * i2c3_begin() first and to call i2c3_end() when done. i2c3_end() de-inits
 * the peripheral (clock gated, pins analog), which is what lets the MCU go
 * into Stop 2 with I2C3 fully off.
 *
 * Waits longer than 3 ms use sleep_delay() (Stop 2 from 4 ms), never HAL_Delay().
 * ---------------------------------------------------------------------------
 */
#include "clock_port.h"
#include "i2c.h"

#define I2C3_TIMEOUT_MS   25U

/* ---- MAX17055 ---------------------------------------------------------- */
#define MAX17055_I2C_ADDR           0x36U
#define MAX17055_REG_STATUS         0x00U
#define MAX17055_REG_REPCAP         0x05U
#define MAX17055_REG_REPSOC         0x06U
#define MAX17055_REG_AGE            0x07U
#define MAX17055_REG_VCELL          0x09U   /* instantaneous cell voltage */
#define MAX17055_REG_CURRENT        0x0AU   /* instantaneous current */
#define MAX17055_REG_AVGCURRENT     0x0BU   /* the only current register read (default 5.625 s filter) */
#define MAX17055_REG_FULLCAPREP     0x10U
#define MAX17055_REG_TTE            0x11U
#define MAX17055_REG_AVGTA          0x16U
#define MAX17055_REG_CYCLES         0x17U
#define MAX17055_REG_DESIGNCAP      0x18U
#define MAX17055_REG_AVGVCELL       0x19U
#define MAX17055_REG_MAXMINCURRENT  0x1CU
#define MAX17055_REG_ICHGTERM       0x1EU
#define MAX17055_REG_TTF            0x20U
#define MAX17055_REG_VEMPTY         0x3AU
#define MAX17055_REG_FSTAT          0x3DU
#define MAX17055_REG_DQACC          0x45U
#define MAX17055_REG_DPACC          0x46U
#define MAX17055_REG_QH             0x4DU   /* coulomb counter, 16-bit, wraps */
#define MAX17055_REG_CMD            0x60U
#define MAX17055_REG_HIBCFG         0xBAU
#define MAX17055_REG_MODELCFG       0xDBU

#define MAX17055_STATUS_POR_BIT     0x0002U
#define MAX17055_DNR_TIMEOUT_MS     1500U   /* FSTAT.DNR takes ~710 ms after power-up */
#define MAX17055_POST_CONFIG_MS     200U    /* let VCell/QH start converting */

#define MAX17055_ICHGTERM_MA        1.0f
#define MAX17055_CHARGE_VOLTAGE_V   4.35f   /* picks the dPAcc/ModelCFG branch */

/* Info-screen register dump (clock_port_ui.c, draw_max17055_screen()). */
uint16_t g_max17055_regs[MAX17055_REG_COUNT];
bool g_max17055_regs_valid = false;


/* true = Live Current mode: hibernate is forced off so AvgCurrent updates at
   the full rate. */
static bool s_live_mode = false;
static uint16_t s_hibcfg_saved = 0U;
static bool s_hibcfg_saved_valid = false;

/* AvgVCell baseline for the supercap dV/dt current estimate. */
static float s_prev_avgvcell_v = 0.0f;
static uint32_t s_prev_avgvcell_epoch = 0U;
static bool s_prev_avgvcell_valid = false;

/* ---- SHT40 ------------------------------------------------------------- */
#define SHT40_I2C_ADDR        0x44U
#define SHT40_CMD_MEASURE_LP  0xE0U     /* low precision, one shot; sensor idles afterwards */
#define SHT40_MEASURE_MS      2U        /* low-precision measurement takes 2 ms max */


void i2c3_begin(void)
{
  /* After i2c3_end() the handle is back in RESET, so this re-runs MspInit
     (clock + pins). At boot main() has already initialised it, in which case
     there is nothing to do. */
  if (hi2c3.State == HAL_I2C_STATE_RESET) {
    MX_I2C3_Init();
  }
}

void i2c3_end(void)
{
  (void)HAL_I2C_DeInit(&hi2c3);
}

/* One retry after re-initialising the peripheral, same idea the MAX17055
   code used. */
static void i2c3_recover(void)
{
  (void)HAL_I2C_DeInit(&hi2c3);
  MX_I2C3_Init();
}

static bool i2c3_write(uint8_t addr, const uint8_t *data, uint16_t len)
{
  return HAL_I2C_Master_Transmit(&hi2c3, (uint16_t)(addr << 1), (uint8_t *)data, len, I2C3_TIMEOUT_MS) == HAL_OK;
}

static bool i2c3_read(uint8_t addr, uint8_t *data, uint16_t len)
{
  return HAL_I2C_Master_Receive(&hi2c3, (uint16_t)(addr << 1), data, len, I2C3_TIMEOUT_MS) == HAL_OK;
}

/* ====================================================================== */
/* MAX17055                                                                */
/* ====================================================================== */

/* Registers are 16-bit, LSB first. */
static bool max17055_read_reg16(uint8_t reg, uint16_t *value)
{
  uint8_t buf[2] = {0U, 0U};
  for (uint8_t attempt = 0U; attempt < 2U; ++attempt) {
    if (HAL_I2C_Mem_Read(&hi2c3, (uint16_t)(MAX17055_I2C_ADDR << 1), reg, I2C_MEMADD_SIZE_8BIT, buf, 2U, I2C3_TIMEOUT_MS) == HAL_OK) {
      *value = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
      return true;
    }
    if (attempt == 0U) {
      i2c3_recover();
    }
  }
  return false;
}

static bool max17055_write_reg16(uint8_t reg, uint16_t value)
{
  uint8_t buf[3] = { reg, (uint8_t)(value & 0xFFU), (uint8_t)(value >> 8U) };
  for (uint8_t attempt = 0U; attempt < 2U; ++attempt) {
    if (i2c3_write(MAX17055_I2C_ADDR, buf, 3U)) {
      return true;
    }
    if (attempt == 0U) {
      i2c3_recover();
    }
  }
  return false;
}

static bool max17055_wait_data_ready(uint32_t timeout_ms)
{
  uint16_t fstat = 0U;
  for (uint32_t waited = 0U; waited < timeout_ms; waited += 10U) {
    if (!max17055_read_reg16(MAX17055_REG_FSTAT, &fstat)) {
      return false;
    }
    if ((fstat & 0x0001U) == 0U) {
      return true;
    }
    delay_ms(10U);
  }
  return false;
}

/* Exit Hibernate - exact order per the Software Implementation Guide:
   Command = 0x90, HibCfg = 0, Command = 0. */
static bool max17055_force_active(void)
{
  return max17055_write_reg16(MAX17055_REG_CMD, 0x0090U) &&
         max17055_write_reg16(MAX17055_REG_HIBCFG, 0x0000U) &&
         max17055_write_reg16(MAX17055_REG_CMD, 0x0000U);
}

/* The EZ-config sequence only needs to run after a power-on reset, so it is
   gated on STATUS.POR. If STATUS can't be read, assume it is needed. */
static bool max17055_needs_config(void)
{
  uint16_t status = 0U;
  if (!max17055_read_reg16(MAX17055_REG_STATUS, &status)) {
    return true;
  }
  return (status & MAX17055_STATUS_POR_BIT) != 0U;
}

static bool max17055_configure(void)
{
  /* Step 2: wait for FSTAT.DNR to clear before touching anything else. */
  if (!max17055_wait_data_ready(MAX17055_DNR_TIMEOUT_MS)) {
    return false;
  }

  /* Step 3: initialise configuration. Remember HibCfg, then exit Hibernate. */
  uint16_t hibCfgOld = 0U;
  if (!max17055_read_reg16(MAX17055_REG_HIBCFG, &hibCfgOld)) {
    return false;
  }
  if (!max17055_force_active()) {
    return false;
  }

  /* Step 3.1: EZ config. DesignCap in capacity counts (5 uVh / Rsense each). */
  uint16_t designCapReg = (uint16_t)((MAX17055_DESIGNCAP_MAH * 1000.0f / MAX17055_CAPACITY_LSB_UAH) + 0.5f);
  if (!max17055_write_reg16(MAX17055_REG_DESIGNCAP, designCapReg)) {
    return false;
  }

  uint16_t dQAcc = designCapReg / 32U;
  if (!max17055_write_reg16(MAX17055_REG_DQACC, dQAcc)) {
    return false;
  }

  uint16_t ichgTermReg = (uint16_t)(MAX17055_ICHGTERM_MA * 1000.0f / MAX17055_CURRENT_LSB_UA);
  if (!max17055_write_reg16(MAX17055_REG_ICHGTERM, ichgTermReg)) {
    return false;
  }

  /* VEmpty: VE in bits[15:7] at 10 mV/count, VR in bits[6:0] at 40 mV/count. */
  uint16_t emptyBits    = (uint16_t)(2.95f / 0.01f);
  uint16_t recoveryBits = (uint16_t)(3.10f / 0.04f);
  uint16_t vEmptyReg = (uint16_t)((emptyBits << 7) | (recoveryBits & 0x7FU));
  if (!max17055_write_reg16(MAX17055_REG_VEMPTY, vEmptyReg)) {
    return false;
  }

  uint16_t dPAccReg;
  uint16_t modelCfgReg;
  if (MAX17055_CHARGE_VOLTAGE_V > 4.275f) {
    dPAccReg    = (uint16_t)(((uint32_t)dQAcc * 51200UL) / designCapReg);
    modelCfgReg = 0x8420U;
  } else {
    dPAccReg    = (uint16_t)(((uint32_t)dQAcc * 44138UL) / designCapReg);
    modelCfgReg = 0x8020U;
  }
  if (!max17055_write_reg16(MAX17055_REG_DPACC, dPAccReg)) {
    return false;
  }
  if (!max17055_write_reg16(MAX17055_REG_MODELCFG, modelCfgReg)) {
    return false;
  }

  /* Poll ModelCFG.Refresh (bit 15) until the IC clears it. */
  uint32_t start_ms = millis_now();
  for (;;) {
    uint16_t modelcfg = 0U;
    if (!max17055_read_reg16(MAX17055_REG_MODELCFG, &modelcfg)) {
      return false;
    }
    if ((modelcfg & 0x8000U) == 0U) {
      break;
    }
    if ((millis_now() - start_ms) > 10000U) {
      break;
    }
    delay_ms(10U);
  }

  /* Restore the original HibCfg (direct write, no command dance). */
  if (!max17055_write_reg16(MAX17055_REG_HIBCFG, hibCfgOld)) {
    return false;
  }

  /* Step 4: clear the POR bit and verify it stuck. */
  uint16_t status = 0U;
  if (!max17055_read_reg16(MAX17055_REG_STATUS, &status)) {
    return false;
  }
  uint16_t clearedStatus = (uint16_t)(status & (uint16_t)~MAX17055_STATUS_POR_BIT);
  for (uint8_t attempt = 0U; attempt < 3U; ++attempt) {
    uint16_t readback = 0U;
    if (!max17055_write_reg16(MAX17055_REG_STATUS, clearedStatus)) {
      return false;
    }
    if (max17055_read_reg16(MAX17055_REG_STATUS, &readback) && (readback == clearedStatus)) {
      return true;
    }
  }
  return false;
}

/* Runs the EZ config if the gauge has just powered up (or browned out).
   Called from update_sensors() every minute, which also covers boot. */
bool max17055_init_if_needed(void)
{
  if (!max17055_needs_config()) {
    return true;
  }
  if (!max17055_configure()) {
    return false;
  }
  delay_ms(MAX17055_POST_CONFIG_MS);
  if (s_live_mode) {
    (void)max17055_force_active();  /* configure() put HibCfg back */
  }
  return true;
}

/* Live Current mode on: take the gauge out of Hibernate and keep it out, so
   Current updates at the full rate. Off: put the original HibCfg back. */
void max17055_set_live_mode(bool live)
{
  i2c3_begin();
  if (live) {
    if (!s_hibcfg_saved_valid) {
      s_hibcfg_saved_valid = max17055_read_reg16(MAX17055_REG_HIBCFG, &s_hibcfg_saved);
    }
    (void)max17055_force_active();
  } else if (s_hibcfg_saved_valid) {
    (void)max17055_write_reg16(MAX17055_REG_HIBCFG, s_hibcfg_saved);
  }
  i2c3_end();
  s_live_mode = live;
}

/* Battery voltage from VCell (instantaneous, not AvgVCell). */
bool max17055_read_vcell(float *volts)
{
  uint16_t raw = 0U;
  if (!max17055_read_reg16(MAX17055_REG_VCELL, &raw)) {
    return false;
  }
  *volts = (float)raw * MAX17055_VOLTAGE_LSB_V;
  return true;
}

/* Battery voltage from AvgVCell (the gauge's filtered voltage). This is what
   the supercap charge / dV-dt current estimate is built on. */
bool max17055_read_avgvcell(float *volts)
{
  uint16_t raw = 0U;
  if (!max17055_read_reg16(MAX17055_REG_AVGVCELL, &raw)) {
    return false;
  }
  *volts = (float)raw * MAX17055_VOLTAGE_LSB_V;
  return true;
}

/* Reads the registers the info screen shows into g_max17055_regs[].
   Brackets the bus itself. */
bool max17055_read_info_regs(void)
{
  static const uint8_t regs[] = {
    MAX17055_REG_REPCAP, MAX17055_REG_REPSOC, MAX17055_REG_FULLCAPREP,
    MAX17055_REG_AVGTA, MAX17055_REG_CURRENT, MAX17055_REG_AVGCURRENT,
    MAX17055_REG_TTE, MAX17055_REG_TTF, MAX17055_REG_CYCLES, MAX17055_REG_AGE,
    MAX17055_REG_AVGVCELL, MAX17055_REG_MAXMINCURRENT
  };
  bool all_ok = true;

  i2c3_begin();
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(regs) / sizeof(regs[0])); ++i) {
    uint16_t value = 0U;
    if (max17055_read_reg16(regs[i], &value)) {
      g_max17055_regs[regs[i]] = value;
    } else {
      all_ok = false;
    }
  }
  i2c3_end();

  g_max17055_regs_valid = all_ok;
  return all_ok;
}

/* ====================================================================== */
/* SHT40                                                                   */
/* ====================================================================== */

/* Sensirion CRC-8: polynomial 0x31, init 0xFF, over each 2-byte word. */
static uint8_t sht4x_crc8(const uint8_t *data, uint8_t len)
{
  uint8_t crc = 0xFFU;
  for (uint8_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      if ((crc & 0x80U) != 0U) {
        crc = (uint8_t)((crc << 1) ^ 0x31U);
      } else {
        crc = (uint8_t)(crc << 1);
      }
    }
  }
  return crc;
}

/* One low-precision measurement. Frame: T_hi T_lo CRC RH_hi RH_lo CRC.
   The SHT40 drops into idle by itself afterwards, so there is nothing to
   shut down. */
bool sht40_read(float *temperature_c, float *humidity_pct)
{
  const uint8_t cmd = SHT40_CMD_MEASURE_LP;
  uint8_t buf[6] = {0};

  for (uint8_t attempt = 0U; attempt < 2U; ++attempt) {
    if (!i2c3_write(SHT40_I2C_ADDR, &cmd, 1U)) {
      i2c3_recover();
      continue;
    }
    sleep_delay(SHT40_MEASURE_MS);

    if (!i2c3_read(SHT40_I2C_ADDR, buf, 6U)) {
      continue;   /* sensor NACKs the read until the measurement is done */
    }
    if ((sht4x_crc8(&buf[0], 2U) != buf[2]) || (sht4x_crc8(&buf[3], 2U) != buf[5])) {
      continue;
    }

    uint16_t t_ticks = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    uint16_t rh_ticks = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);
    float rh = ((float)rh_ticks * 125.0f / 65535.0f) - 6.0f;
    if (rh < 0.0f) {
      rh = 0.0f;
    } else if (rh > 100.0f) {
      rh = 100.0f;
    }
    *temperature_c = ((float)t_ticks * 175.0f / 65535.0f) - 45.0f;
    *humidity_pct = rh;
    return true;
  }
  return false;
}

/* ====================================================================== */
/* Current sampling, supercap charge / current estimate                   */
/* ====================================================================== */

/* k = 0 is the oldest sample. Values are uA. */
float current_history_get(uint16_t k)
{
  uint16_t index = (g_cur_count < CUR_HIST_SAMPLES) ? k : (uint16_t)((g_cur_head + k) % CUR_HIST_SAMPLES);
  return g_cur_hist[index];
}

float current_solar_history_get(uint16_t k)
{
  uint16_t index = (g_cur_count < CUR_HIST_SAMPLES) ? k : (uint16_t)((g_cur_head + k) % CUR_HIST_SAMPLES);
  return g_cur_solar_hist[index];
}

void current_history_reset(void)
{
  g_cur_head = 0U;
  g_cur_count = 0U;
}

/* ---- Supercap charge + dV/dt current estimate -------------------------------
   Called once a minute from main.c after update_sensors() has refreshed
   g_battery_avg_voltage / g_avg_vcell_valid.

   Charge: g_cumulative_uah = C_eff * (AvgVCell - VCAP_MIN_V), in uAh. It is an
   absolute "stored charge above the 2.9 V floor", so 0 at 2.9 V and 687.5 uAh
   at 4.4 V - not a running total, so it never drifts and survives a missed
   sample.

   Current: I = C_eff * (V_now - V_prev) / dt, with V_prev from the previous
   good call and dt from the supplied epochs (not an assumed 60 s). Positive =
   voltage rising = charging. A missed read just stretches the next dt; a gap
   outside VCAP_MIN_DT_S..VCAP_MAX_DT_S (clock was set, long outage) re-baselines
   instead of producing a bogus number. If this minute had no AvgVCell reading,
   the previous values are left as they were. */
void vcap_estimator_reset(void)
{
  s_prev_avgvcell_valid = false;
  g_est_current_ua = 0.0f;
}

void vcap_estimator_update(uint32_t sample_epoch)
{
  if (!g_avg_vcell_valid) {
    return;
  }

  const double v = (double)g_battery_avg_voltage;
  const double c_eff = (double)SUPERCAP_EFF_F;

  double charge_c = c_eff * (v - (double)VCAP_MIN_V);
  if (charge_c < 0.0) {
    charge_c = 0.0;
  }
  g_cumulative_uah = charge_c * (1.0e6 / 3600.0);

  if (s_prev_avgvcell_valid) {
    uint32_t dt = sample_epoch - s_prev_avgvcell_epoch;   /* wraps huge if the clock went backwards */
    if (dt < VCAP_MIN_DT_S) {
      return;   /* too soon (duplicate call): keep the old baseline */
    }
    if (dt <= VCAP_MAX_DT_S) {
      g_est_current_ua = (float)((c_eff * (v - (double)s_prev_avgvcell_v) / (double)dt) * 1.0e6);
    } else {
      g_est_current_ua = 0.0f;   /* gap too long to trust; re-baseline below */
    }
  }

  s_prev_avgvcell_v = g_battery_avg_voltage;
  s_prev_avgvcell_epoch = sample_epoch;
  s_prev_avgvcell_valid = true;
}

/* Takes one current reading and updates g_current_ua (uA, float) and - when
   record_history is set (Live Current mode) - the chart ring. No averaging is
   done here.

   Current: the AvgCurrent register in every mode. Sign follows
   MAX17055_CURRENT_SIGN.

   (The cumulative uAh shown on the faces no longer comes from the gauge's QH
   register: see vcap_estimator_update().) */
bool current_sample_take(uint32_t now_epoch, bool record_history)
{
  (void)now_epoch;

  uint16_t current_raw = 0U;
  i2c3_begin();
  bool ok = max17055_read_reg16(MAX17055_REG_AVGCURRENT, &current_raw);
  i2c3_end();
  if (!ok) {
    return false;
  }

  g_current_ua = (float)(int16_t)current_raw * MAX17055_CURRENT_LSB_UA * (float)MAX17055_CURRENT_SIGN;

  if (record_history) {
    g_cur_hist[g_cur_head] = g_current_ua;
    g_cur_solar_hist[g_cur_head] = g_solar_voltage;
    g_cur_head = (uint16_t)((g_cur_head + 1U) % CUR_HIST_SAMPLES);
    if (g_cur_count < CUR_HIST_SAMPLES) {
      ++g_cur_count;
    }
  }
  return true;
}
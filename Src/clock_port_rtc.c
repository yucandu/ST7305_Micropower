#include "clock_port.h"

/* RTC resolution is 1/256 s = 3.9 ms, so anything shorter than 4 ms can't be
   timed in Stop 2. */
#define STOP2_MIN_DELAY_MS   4U

extern RTC_HandleTypeDef hrtc;

/* Same leap-year/day-count math as clock_port_sensors.c's DS3231 helpers,
   duplicated here (as static) since this file has no visibility into that
   file's static helpers. If you'd rather not duplicate it, move
   is_leap_year()/days_in_month() into a shared header-inline or a small
   clock_port_calendar.c and call from both places. */
static bool is_leap_year_rtc(uint16_t year)
{
  return (((year % 4U) == 0U) && (((year % 100U) != 0U) || ((year % 400U) == 0U)));
}

static uint8_t days_in_month_rtc(uint16_t year, uint8_t month)
{
  static const uint8_t days_table[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  if ((month == 2U) && is_leap_year_rtc(year)) {
    return 29U;
  }
  return days_table[month - 1U];
}

/* Writes epoch_s into the internal RTC's time/date registers. Call this
   right after update_sensors() has synced g_epoch_seconds from the DS3231,
   so the internal RTC (which is what the main loop reads every iteration
   and what the wake alarm is armed against) tracks the same wall time. */
void rtc_set_epoch(uint32_t epoch_s)
{
  uint32_t seconds = epoch_s % 60U;
  uint32_t minutes = (epoch_s / 60U) % 60U;
  uint32_t hours = (epoch_s / 3600U) % 24U;
  uint32_t days = epoch_s / 86400U;
  uint16_t year = 2000U;
  while (days >= (is_leap_year_rtc(year) ? 366U : 365U)) {
    days -= (is_leap_year_rtc(year) ? 366U : 365U);
    ++year;
  }
  uint8_t month = 1U;
  while (days >= days_in_month_rtc(year, month)) {
    days -= days_in_month_rtc(year, month);
    ++month;
  }
  uint8_t day = (uint8_t)(days + 1U);

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  sTime.Hours = (uint8_t)hours;
  sTime.Minutes = (uint8_t)minutes;
  sTime.Seconds = (uint8_t)seconds;
  sTime.TimeFormat = RTC_HOURFORMAT12_AM;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  (void)HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);

  sDate.WeekDay = RTC_WEEKDAY_MONDAY; /* not used by any wake/alarm logic below */
  sDate.Month = month;
  sDate.Date = day;
  sDate.Year = (uint8_t)(year - 2000U);
  (void)HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
}

/* Reads the internal RTC and returns the equivalent Unix epoch. This is
   the "now" the main loop should poll every iteration, replacing the old
   millis()-based g_epoch_seconds incrementer - it keeps ticking correctly
   across Stop2 sleep gaps since the RTC itself doesn't stop in Stop2. */
uint32_t rtc_get_epoch(void)
{
	/* Wait for shadow registers to sync after waking from Stop 2 */
	  HAL_RTC_WaitForSynchro(&hrtc);

	  RTC_TimeTypeDef sTime = {0};
	  RTC_DateTypeDef sDate = {0};

  /* Must read Time then Date, in that order - reading TR latches the
     shadow SSR/TR/DR registers, so DR must be read right after. */
  (void)HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  (void)HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

  uint16_t year = 2000U + sDate.Year;
  uint32_t days = 0U;
  static const uint16_t days_before_month[12] = {0U, 31U, 60U, 91U, 121U, 152U, 182U, 213U, 244U, 274U, 305U, 335U};
  for (uint16_t y = 2000U; y < year; ++y) {
    days += is_leap_year_rtc(y) ? 366U : 365U;
  }
  days += days_before_month[sDate.Month - 1U] + (uint32_t)sDate.Date - 1U;
  if (is_leap_year_rtc(year) && (sDate.Month > 2U)) {
    ++days;
  }
  return (days * 86400UL) + ((uint32_t)sTime.Hours * 3600UL) + ((uint32_t)sTime.Minutes * 60UL) + sTime.Seconds;
}

static void rtc_set_alarm_for_epoch(uint32_t target_epoch)
{
  /* CubeMX only offers the RTC NVIC checkbox when an RTC feature (Alarm A,
     WakeUp, Timestamp) is enabled in its Mode panel. This firmware arms Alarm A
     at run time instead, so the RTC interrupt is enabled here. It is safe to do
     on every call. */
  HAL_NVIC_SetPriority(RTC_IRQn, 3U, 0U);
  HAL_NVIC_EnableIRQ(RTC_IRQn);

  uint32_t sec = target_epoch % 60U;
  uint32_t min = (target_epoch / 60U) % 60U;
  uint32_t hr  = (target_epoch / 3600U) % 24U;

  RTC_AlarmTypeDef sAlarm = {0};
  RTC_DateTypeDef sDate = {0};
  (void)HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN); /* only feeds AlarmDateWeekDay; masked out anyway */

  sAlarm.AlarmTime.Hours = (uint8_t)hr;
  sAlarm.AlarmTime.Minutes = (uint8_t)min;
  sAlarm.AlarmTime.Seconds = (uint8_t)sec;
  sAlarm.AlarmTime.TimeFormat = RTC_HOURFORMAT12_AM;
  sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
  sAlarm.AlarmMask = RTC_ALARMMASK_DATEWEEKDAY;
  sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
  sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
  sAlarm.AlarmDateWeekDay = sDate.Date;
  sAlarm.Alarm = RTC_ALARM_A;
  (void)HAL_RTC_SetAlarm_IT(&hrtc, &sAlarm, RTC_FORMAT_BIN);
}

/* Arms Alarm A for "current time + 1 second". Mirrors stmRtc.setAlarmEpoch(
   stmRtc.getEpoch() + 1) from the .cpp, INCLUDING its get/set/verify/retry-
   once pattern immediately before LowPower.deepSleep() - HAL_RTC_SetAlarm_IT()
   isn't instantaneous (disable alarm, poll ALRAWF, write registers,
   re-enable), so if the real RTC ticks over to the target while that write
   is in flight, the alarm we just armed is already in the past and (since
   AlarmMask only matches H:M:S, date ignored) won't fire again for up to
   24h - which matches the intermittent hang exactly. */
void rtc_arm_wake_in_1s(void)
{
  rtc_arm_wake_at_epoch(rtc_get_epoch() + 1U);
}

/* Arms Alarm A for an absolute epoch (at most 24h ahead - the alarm only
   matches H:M:S). Same race protection as above: if the RTC reached the
   target while the alarm registers were being written, the alarm is already
   in the past and wouldn't fire for a day, so re-arm for "now + 1s" instead
   (an early wake is harmless; a missed one is not). Used by low-power mode to
   sleep straight through to the next minute boundary. */
void rtc_arm_wake_at_epoch(uint32_t target_epoch)
{
  rtc_set_alarm_for_epoch(target_epoch);

  if (rtc_get_epoch() >= target_epoch) {
    target_epoch = rtc_get_epoch() + 1U;
    rtc_set_alarm_for_epoch(target_epoch);
  }
}

/* HAL weak callback - fires from the RTC Alarm ISR on wake.
   Requires "RTC global interrupt" enabled in CubeMX's NVIC tab (see notes). */
void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc_handle)
{
  (void)hrtc_handle;
  g_wake_flag = true;
}

/* RTC (non-secure) interrupt vector. Written by hand because CubeMX does not
   generate it while Alarm A / WakeUp are Disabled. If stm32u3xx_it.c ever also
   contains an RTC_IRQHandler (e.g. after enabling a feature in CubeMX), delete
   one of the two or the linker will report a duplicate definition.
   HAL_RTC_AlarmIRQHandler() clears the alarm flag and calls
   HAL_RTC_AlarmAEventCallback() above. */
void RTC_IRQHandler(void)
{
  HAL_RTC_AlarmIRQHandler(&hrtc);
}

/* ---------------------------------------------------------------------- */
/* sleep_delay(): millisecond waits (Stop 2 from 4 ms, core Sleep below)   */
/* ---------------------------------------------------------------------- */
/* Below STOP2_MIN_DELAY_MS (2..3 ms): plain Sleep, not Stop 2. The core is gated by WFI (SLEEPDEEP clear) while
   clocks, SysTick and all peripherals keep running, and the 1 ms SysTick
   interrupt wakes it to re-check the time. That gives millisecond resolution
   with no RTC alarm and no clock restart. Any other interrupt (SPI DMA
   complete, EXTI) also wakes the core; the loop just goes back to sleep until
   the time is up. HAL_Delay() adds one tick so the wait is never short; the
   same is done here. */
void sleep_delay(uint32_t ms)
{
  if (ms <= 1U) {
    HAL_Delay(ms);
    return;
  }

  if (ms >= STOP2_MIN_DELAY_MS) {
    sleep_stop2_us(ms * 1000U);
    return;
  }

  uint32_t start = HAL_GetTick();
  uint32_t wait = ms + 1U;
  CLEAR_BIT(SCB->SCR, SCB_SCR_SLEEPDEEP_Msk);
  while ((HAL_GetTick() - start) < wait) {
    __WFI();
  }
}

/* ---------------------------------------------------------------------- */
/* sleep_stop2_us(): sub-second waits in Stop 2, timed by RTC Alarm A      */
/* ---------------------------------------------------------------------- */
/* The RTC runs from the LSE with PREDIV_S = 255, so SSR is a 256 Hz down
   counter (3.9 ms per tick). Alarm A is armed with every H:M:S/date field
   masked and the full SS field compared, i.e. it fires when SSR reaches the
   programmed value. SysTick is stopped in Stop 2, so HAL_GetTick() does not
   advance across these waits - that is why the wait is timed by SSR.

   Two reads of SSR that differ by d ticks are between d-1 and d+1 ticks of
   real time apart (unknown phase inside the first tick), so to guarantee
   >= us the SSR must advance by ceil(us / tick) + 1.

   Any other wake source (button EXTI) cuts a sleep short; the loop just
   re-arms for what is left. Valid up to ~0.5 s per chunk, longer requests
   are split. */
static uint32_t rtc_subsec_now(void)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* Shadow registers must be re-synchronised after Stop 2. GetTime latches
     the shadow registers, GetDate releases them again. */
  (void)HAL_RTC_WaitForSynchro(&hrtc);
  (void)HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  (void)HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  return sTime.SubSeconds;
}

static void rtc_set_alarm_subsec(uint32_t ssr_target)
{
  HAL_NVIC_SetPriority(RTC_IRQn, 3U, 0U);
  HAL_NVIC_EnableIRQ(RTC_IRQn);

  RTC_AlarmTypeDef sAlarm = {0};
  sAlarm.AlarmTime.SubSeconds = ssr_target;
  sAlarm.AlarmTime.TimeFormat = RTC_HOURFORMAT12_AM;
  sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
  sAlarm.AlarmMask = RTC_ALARMMASK_ALL;                    /* date, H, M, S ignored */
  sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_NONE; /* all SS bits compared  */
  sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
  sAlarm.AlarmDateWeekDay = 1U;                            /* masked; must still be valid */
  sAlarm.Alarm = RTC_ALARM_A;
  (void)HAL_RTC_SetAlarm_IT(&hrtc, &sAlarm, RTC_FORMAT_BIN);
}

static void sleep_stop2_ticks(uint32_t ticks)
{
  const uint32_t period = hrtc.Init.SynchPrediv + 1U;   /* SSR ticks per second */
  uint32_t start = rtc_subsec_now();

  for (;;) {
    uint32_t now_ss = rtc_subsec_now();
    uint32_t elapsed = (start + period - now_ss) % period;   /* SSR counts down */
    if (elapsed >= ticks) {
      break;
    }
    uint32_t remaining = ticks - elapsed;

    /* With < 2 ticks left the alarm could match while it is still being
       armed / before WFI (enter_stop2_sleep() clears the alarm flag), and
       the wake would then be a whole second late. Spin out the last tick. */
    if (remaining < 2U) {
      continue;
    }

    rtc_set_alarm_subsec((now_ss + period - remaining) % period);

    /* Arming takes a few tens of us; make sure the target is still ahead. */
    uint32_t chk = rtc_subsec_now();
    if (((now_ss + period - chk) % period) >= remaining) {
      break;
    }
    enter_stop2_sleep();
  }

  /* The alarm matches once per second until it is turned off. */
  (void)HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);
}

void sleep_stop2_us(uint32_t us)
{
  const uint32_t period = hrtc.Init.SynchPrediv + 1U;

  while (us > 500000U) {
    sleep_stop2_us(500000U);
    us -= 500000U;
  }
  uint32_t ticks = (uint32_t)((((uint64_t)us * period) + 999999ULL) / 1000000ULL) + 1U;
  sleep_stop2_ticks(ticks);
}

/* Suspends SysTick (so it doesn't immediately wake us back up), drops into
   Stop 2, and restores the clock tree + SysTick on wake.
   STM32U3 port notes:
     - The L4-only low-power tweaks (PWR_CR3.ENULP, PWR_CR4.FWU, the RCC
       "wake-up from stop" clock select, Flash power-down in stop) don't exist
       on the U3 - Stop 2 wake-up already restarts MSIS and Flash is handled by
       hardware - so each is compiled in only if the HAL actually defines it.
     - Wake-up flags: the L4 code cleared PWR_FLAG_WU. The U3 names differ; the
       fallback below clears the RTC alarm flag (the only wake source used) and
       any pending NVIC bits, which is what actually matters before WFI.
     - If your generated stm32u3xx_hal_pwr_ex.h calls the entry function
       something other than HAL_PWREx_EnterSTOP2Mode, change it in ONE place
       (the line marked ENTER_STOP2). */
void enter_stop2_sleep(void)
{
  /* 1. Mask interrupts */
  __disable_irq();

  /* 2. Disable SysTick AND clear any pending ticks to prevent instant wake-ups */
  HAL_SuspendTick();
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

  /* 3. Clear wake-up flags (the stop entry is refused / re-triggers if one is set) */
#if defined(PWR_FLAG_WU)
  __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
#endif
  __HAL_RTC_ALARM_CLEAR_FLAG(&hrtc, RTC_FLAG_ALRAF);

  /* 4. L4-only ultra-low-power Stop2 settings (same ones STM32LowPower sets) */
#if defined(__HAL_RCC_PWR_CLK_ENABLE)
  __HAL_RCC_PWR_CLK_ENABLE();
#endif
#if defined(PWR_CR3_ENULP)
  SET_BIT(PWR->CR3, PWR_CR3_ENULP);
#endif
#if defined(PWR_CR_FWU)
  SET_BIT(PWR->CR4, PWR_CR_FWU);
#endif
  /* Wake from Stop on HSI16 (RCC_CFGR1.STOPWUCK = 1). The system clock tree
     is HSI16 -> SYSCLK/HCLK/PCLK all /1, with the SPI and I2C kernel clocks
     also on HSI16, and the LSE (RTC) keeps running in Stop 2, so once HSI16
     has restarted the clock tree is already exactly as it was: no
     SystemClock_Config() needed on wake. */
#if defined(RCC_CFGR1_STOPWUCK)
  SET_BIT(RCC->CFGR1, RCC_CFGR1_STOPWUCK);
#else
#warning "RCC_CFGR1_STOPWUCK not defined by this CMSIS header - falling back to SystemClock_Config() after every wake. Set STOPWUCK by hand to remove that."
#define STOP_WAKE_NEEDS_CLOCK_CONFIG 1
#endif

  /* 5. Clear pending interrupts in NVIC before sleeping (Cortex-M33 has up
        to 16 ICPR words; the U3's ~100 IRQs fit in the first 4) */
  for (int i = 0; i < 4; i++) {
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }
#if defined(__HAL_FLASH_POWER_DOWN_ENABLE)
  __HAL_FLASH_POWER_DOWN_ENABLE();
#endif

  /* Stop 2: I/O pins keep the state sleep_pins() left them in (so RST/CS/DC
     stay put and the panel holds its image), and the RTC alarm and EXTI
     button lines wake the core with no extra PWR wake-up setup. (Stop 3
     would float every pin and only wake from the WKUP pins.) */
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERMODE_STOP2, PWR_STOPENTRY_WFI);

  /* === WAKE UP HERE === */

#if defined(STOP_WAKE_NEEDS_CLOCK_CONFIG)
  SystemClock_Config();
#endif
  HAL_ResumeTick();

  /* 7. Unmask interrupts */
  __enable_irq();
}
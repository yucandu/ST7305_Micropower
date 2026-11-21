/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "flash.h"
#include "gpdma.h"
#include "i2c.h"
#include "icache.h"
#include "rtc.h"
#include "spi.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "clock_port.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define AVG_CURRENT 427
#define TRUE_CURRENT 166.5f
/* Longest single Stop2 sleep in low-power mode. 60 = sleep straight to the
   next minute boundary (buttons must wake the MCU themselves via EXTI). If
   they don't, lower this (e.g. 1 = the old wake-every-second polling). */
#define LOW_POWER_MAX_SLEEP_S 60U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint32_t g_boot_ms = 0U;
volatile uint32_t g_epoch_seconds = 0U;
uint32_t g_last_sample_epoch = 0U;
uint32_t g_last_input_ms = 0U;
volatile bool g_wake_flag = false;
extern volatile uint8_t rtc_wakeup_event;
float g_temperature_c = 0.0f;
float g_humidity_pct = 0.0f;
float g_abs_humidity = 0.0f;
float g_battery_voltage = 0.0f;
float g_solar_voltage = 0.0f;
float g_vdda_voltage = 0.0f;
float g_current_ua = 0.0f;
double g_cumulative_uah = 0.0;
float g_battery_avg_voltage = 0.0f;
bool g_avg_vcell_valid = false;
float g_est_current_ua = 0.0f;
float g_cur_hist[CUR_HIST_SAMPLES];
float g_cur_solar_hist[CUR_HIST_SAMPLES];
uint16_t g_cur_head = 0U;
uint16_t g_cur_count = 0U;
bool g_display_dirty = true;
app_state_t g_app_state = STATE_MAIN;
int8_t g_menu_selection = 0;
int8_t g_menu_scroll_offset = 0;
bool g_editing_minutes = false;
uint8_t g_temp_hour = 0U;
uint8_t g_temp_minute = 0U;
bool g_left_pressed_prev = false;
bool g_up_pressed_prev = false;
bool g_down_pressed_prev = false;
bool g_right_pressed_prev = false;
bool g_button_wake = false;
float g_hist_temp[MAX_SAMPLES];
float g_hist_rh[MAX_SAMPLES];
float g_hist_abs_hum[MAX_SAMPLES];
float g_hist_volt[MAX_VOLT_SAMPLES];
float g_hist_cur[MAX_SAMPLES];
float g_hist_est_cur[MAX_SAMPLES];
float g_hist_solar[MAX_SAMPLES];
float g_hist_vdda[MAX_SAMPLES];
uint16_t g_hist_head = 0U;
uint16_t g_hist_count = 0U;
uint16_t g_volt_head = 0U;
uint16_t g_volt_count = 0U;
uint8_t g_volt_sample_counter = 0U;
int16_t g_chart_cursor = -1;
int8_t g_chart_index = 0;
uint32_t g_boot_epoch = 0U;
float g_accumulated_energy_uwh = 0.0f;
bool g_main_needs_full_redraw = true;
bool g_low_power_mode = false;
bool g_live_current_mode = false;
float g_battery_soc_pct = 100.0f;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  //HAL_DBGMCU_EnableDBGStopMode();
    //HAL_DBGMCU_EnableDBGStandbyMode();
  /* Start every boot with a clean RTC/backup domain. The time is
   re-loaded from the DS3231 right after init, so nothing is lost. */
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_BACKUPRESET_FORCE();
  __HAL_RCC_BACKUPRESET_RELEASE();

  /* Restart the LSI too; RTCSEL was just cleared, so nothing is using it. */
  __HAL_RCC_LSI_DISABLE();
  uint32_t lsi_t0 = HAL_GetTick();
  /* U3: LSIRDY lives in RCC_BDCR, not RCC_CSR as on the L4 - the flag macro
     hides that difference. */
  while ((__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) != 0U) && ((HAL_GetTick() - lsi_t0) < 100U)) {
  }
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_SPI1_Init();
  MX_FLASH_Init();
  MX_I2C1_Init();
  MX_I2C3_Init();
  MX_RTC_Init();
  MX_ICACHE_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
    Error_Handler();
  }

  sleep_delay(25U);
    /* Thumbwheel has two switches per direction (hard + slight); combine them
       so either one registers as that direction. Mirrors the combining done
       in handle_ui() - keep both in sync if this ever changes. */
    bool init_hard_left = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_3) == GPIO_PIN_RESET);    /* hard left (wired backwards: swapped) */

    //bool init_slight_left = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_RESET);  /* slight left */

    bool init_hard_right = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_RESET);   /* hard right */

    //bool init_slight_right = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) == GPIO_PIN_RESET); /* slight right */


    g_up_pressed_prev = init_hard_left ;//|| init_slight_left;      /* combined left */
    g_down_pressed_prev = init_hard_right ;//|| init_slight_right;  /* combined right */
    g_right_pressed_prev = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_RESET);  /* pushbutton */
    g_left_pressed_prev = false;  /* no longer a standalone input - folded into g_up_pressed_prev */

    HAL_GPIO_WritePin(GPIOB, PWR_SENSORS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, LCD_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, LCD_DC_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, LCD_RST_PIN, GPIO_PIN_SET);

    /* The MAX17055 is configured by the first update_sensors() below. */

    st7305_init();
    fb_clear();
    st7305_refresh();

    g_boot_ms = millis_now();
    g_last_input_ms = g_boot_ms;
    /* First DS3231 read + full sensor read, like readAllSensors() in setup().
       If the DS3231 didn't answer, leave the RTC alone and try again on the
       next once-a-minute window rather than programming it with a timestamp
       that was never read. */
    update_sensors();
    if (!solar_voltage_read(&g_solar_voltage)) {
      Error_Handler();
    }
    if (g_ds3231_valid) {
      g_epoch_seconds = g_ds3231_epoch;
      rtc_set_epoch(g_epoch_seconds);
    }
    /* First supercap charge estimate (bottom-centre uAh) and the baseline for
       the first dV/dt current estimate a minute from now. */
    vcap_estimator_update(g_epoch_seconds);
    /* First current reading: gives the top-right readout a value straight
       away. */
    (void)current_sample_take(g_epoch_seconds, false);
    g_last_sample_epoch = g_epoch_seconds;
    g_boot_epoch = g_epoch_seconds;

    sleep_pins();
    wake_pins();
    //GPIOA->MODER |= (GPIO_MODER_MODE13 | GPIO_MODER_MODE14);
    HAL_DBGMCU_DisableDBGStopMode();
    HAL_DBGMCU_DisableDBGStandbyMode();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
      uint32_t now = rtc_get_epoch();
      static uint32_t last_display_tick = 0U;
      static uint32_t last_minute_index = 0U;
      bool minute_tick = false;

      if (last_display_tick == 0U) {
        last_display_tick = now;
        last_minute_index = now / 60U;
      }

      if (now != last_display_tick) {
        last_display_tick = now;
        g_epoch_seconds = now;

        /* Live Current mode: one MAX17055 AvgCurrent read per second - updates the
           current and the chart ring. (Seconds and Low
           Power modes read the MAX17055 once a minute, in the minute branch
           below.) */
        if (g_live_current_mode) {
          if (!solar_voltage_read(&g_solar_voltage)) {
            Error_Handler();
          }
          (void)current_sample_take(now, true);
        }

        /* MAX17055 info screen: refresh its register dump once a second. */
        if (g_app_state == STATE_MAX17055) {
          (void)max17055_read_info_regs();
          g_display_dirty = true;
        }

        /* Once-a-minute DS3231 resync + full sensor read + history sample.
           The internal RTC runs off the un-trimmed LSI, so it is simply set
           from the DS3231 here and allowed to drift for the next 60 seconds.
           No calibration, no slewing: the displayed seconds will skip (or
           repeat) by however far the LSI wandered in that minute, and that
           step happens at the top of the minute.

           The one thing that must not happen is setting the clock from a
           read that failed - that is what produced the full-minute jumps. */
        /* Detect "a new minute has started" rather than testing for exactly
           :00 - a long low-power sleep (or a late wake) can land on :01. */
        if ((now / 60U) != last_minute_index) {
          update_sensors();
          if (!solar_voltage_read(&g_solar_voltage)) {
            Error_Handler();
          }

          /* Supercap charge + dV/dt current estimate. Timestamp the AvgVCell
             reading with the DS3231 when it answered (accurate), else with the
             internal RTC's pre-resync `now`. Must run before rtc_set_epoch()
             so both timestamps come from the same pre-step timeline. */
          vcap_estimator_update(g_ds3231_valid ? g_ds3231_epoch : now);

          if (g_ds3231_valid) {
            rtc_set_epoch(g_ds3231_epoch);
          }

          /* update_sensors() blocks for a couple of hundred milliseconds
             (sensor conversions) and may have just re-set the clock, so
             re-read instead of assuming `now` is still current. */
          now = rtc_get_epoch();
          last_display_tick = now;
          g_epoch_seconds = now;
          g_last_sample_epoch = now;
          last_minute_index = now / 60U;

          if (!g_live_current_mode) {
            /* The last current reading is assumed constant for the whole
               minute since the previous one. */
            (void)current_sample_take(now, false);
          }
          record_history_sample();
          g_main_needs_full_redraw = true;
          minute_tick = true;
        }

        /* Seconds mode redraws every second; low-power mode only on the
           once-a-minute tick above. (minute_tick is captured before the
           resync because `now` can land a second either side of :00 once
           the DS3231 has re-set the clock.) */
        if (g_app_state == STATE_MAIN) {
          if (!g_low_power_mode || minute_tick) {
            g_display_dirty = true;
          }
        }
      }

      handle_ui();

      if ((g_app_state != STATE_MAIN) && (millis_now() - g_last_input_ms > 60000U)) {
        g_app_state = STATE_MAIN;
        g_main_needs_full_redraw = true;
        g_display_dirty = true;
        g_last_input_ms = millis_now();
      }

      if (g_display_dirty) {
        if (g_app_state == STATE_MAIN) {
          if (g_low_power_mode) {
            /* Low-power face: always a full redraw, and we only get here
               once a minute (or on mode/screen entry). No seconds partial. */
            draw_clock_face_low_power();
            st7305_refresh();
            g_main_needs_full_redraw = false;
          } else if (g_live_current_mode) {
            /* Live Current dashboard: the chart changes every second, so the
               whole frame is redrawn and sent through the full SPI window. */
            draw_live_current_face();
            st7305_refresh();
            g_main_needs_full_redraw = false;
          } else if (g_main_needs_full_redraw) {
            /* Seconds face: full redraw (background/time/readouts, full SPI
               window) once a minute or on first entry; every other second
               just the seconds digits through the small partial SPI window. */
            draw_clock_face_full();
            st7305_refresh();
            g_main_needs_full_redraw = false;
          } else {
            draw_clock_face_seconds();
            st7305_refresh_seconds_partial();
          }
        } else {
          switch (g_app_state) {
            case STATE_MENU:
              draw_menu_screen();
              break;
            case STATE_SET_TIME:
              draw_set_time_screen();
              break;
            case STATE_CHART:
              draw_chart_screen();
              break;
            case STATE_MAX17055:
              draw_max17055_screen();
              break;
            default:
              draw_clock_face_full();
              break;
          }
          st7305_refresh();
        }
        g_display_dirty = false;
      }
      while (!spi_dma_complete) {
        /* Plain Sleep (core clock gated, SPI + GPDMA keep running) until the
           DMA-complete interrupt. Done with the core instruction rather than
           HAL_PWR_EnterSLEEPMode() because that function's signature differs
           between the L4 and U3 HALs. */
        CLEAR_BIT(SCB->SCR, SCB_SCR_SLEEPDEEP_Msk);
        __WFI();
      }
      /* Only the clock face sleeps. Menu/chart/set-time stay awake and poll
         at a normal rate so button presses feel responsive - matching the
         .cpp, which only calls LowPower.deepSleep() when currentState ==
         STATE_MAIN and just does delay(20) otherwise. */
      if (g_app_state == STATE_MAIN) {
        /* Match the PlatformIO firmware's sleep flow: shut down SPI/display
           state first, then enter Stop2 and restore the interface on wake. */
    	  //st7305_write_command(0x39U);   /* experiment: re-assert Low Power Mode after the write */
        uint32_t sleep_now = rtc_get_epoch();
        sleep_pins();
        if (g_low_power_mode) {
          /* Sleep until the next minute boundary (capped), not every second. */
          uint32_t sleep_s = 60U - (sleep_now % 60U);
          if (sleep_s > LOW_POWER_MAX_SLEEP_S) {
            sleep_s = LOW_POWER_MAX_SLEEP_S;
          }
          rtc_arm_wake_at_epoch(sleep_now + sleep_s);
        } else {
          rtc_arm_wake_in_1s();
        }
        g_wake_flag = false;
        enter_stop2_sleep();
        wake_pins();
      } else {
        /* Menu/chart/set-time: wait ~20 ms in plain Sleep, not Stop 2.
           Stop 2 suspends SysTick, and the button auto-repeat and the 60 s
           menu timeout both run on HAL_GetTick(). Other interrupts (EXTI,
           DMA) wake the WFI early; the loop just goes back to sleep until
           the 20 ms is up. */
        uint32_t wait_start = millis_now();
        CLEAR_BIT(SCB->SCR, SCB_SCR_SLEEPDEEP_Msk);
        while ((millis_now() - wait_start) < 20U) {
          __WFI();
        }
      }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @param None
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

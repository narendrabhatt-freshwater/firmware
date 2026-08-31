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
#include "dma.h"
#include "i2c.h"
#include "i2s.h"
#include "sai.h"
#include "tim.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "cs4304.h"
#include "tim.h"
#include "usart.h"
#include "audio_bridge.h"
#include "channel_console.h"
#include "usb_app.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* CS4304 DAC handle — owned here; bound into bridge + console after Init. */
CS4304_HandleTypeDef hcs4304;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Console / RS485 / LED chaser live in channel_console.c */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* I-cache keeps the NoteBank refill within the 1 ms I2S DMA callback
   * budget. I-cache only: D-cache requires non-cacheable MPU regions for
   * DMA buffers, which this MPU configuration does not provide. */
  SCB_EnableICache();
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C5_Init();
  MX_SAI4_Init();
  MX_TIM3_Init();
  MX_TIM12_Init();
  MX_UART5_Init();
  MX_I2S1_Init();
  MX_I2S2_Init();
  MX_USB_OTG_HS_PCD_Init();
  /* USER CODE BEGIN 2 */

  /* USB is owned by TinyUSB (UAC2 int16 BODY + CDC console) — see USB_APP/.
   * ST's MX_USB_DEVICE_Init() is gone with the USB_DEVICE middleware;
   * USB_App_Init() does the clocks, PHY power and NVIC itself.
   * Kept inside USER CODE so CubeMX regeneration preserves it. */
  USB_App_Init();

  /* --- Power-on status sequence: flash fixed red/yellow LEDs only.
   * The RGB package is reserved for VM scripts and stays off at boot. --- */
  {
    const GPIO_TypeDef *led_ports[] = {LED_R_GPIO_Port, LED_Y_GPIO_Port};
    const uint16_t led_pins[] = {LED_R_Pin, LED_Y_Pin};
    const uint8_t num_leds = 2;

    for (uint8_t i = 0; i < num_leds; i++)
    {
      HAL_GPIO_WritePin((GPIO_TypeDef *)led_ports[i], led_pins[i],
                        GPIO_PIN_SET);
      HAL_Delay(150);
      HAL_GPIO_WritePin((GPIO_TypeDef *)led_ports[i], led_pins[i],
                        GPIO_PIN_RESET);
    }
  }

  /* Initialize CS4304S DAC via I2C5.
   * CONFIG5 0R to GND -> I2C address 0x60 (DS1388F1 Table 4-17); the driver
   * falls back to a bus scan if that address does not ACK.
   * NON-FATAL: the channel-1 USB playback test must run even if the DAC
   * I2C link is down (LED_R stays on solid as an error indicator). */
  hcs4304.hi2c = &hi2c5;
  hcs4304.DevAddr = CS4304_I2C_ADDR_C5_GND_0R;

  if (CS4304_Init(&hcs4304) != HAL_OK)
  {
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, GPIO_PIN_SET);
  }

  /* Only CH1 (USB audio) follows the host volume slider; CH2-CH4 (test
   * tones) stay at full scale (0 dB + their Trim, if any). */
  hcs4304.MasterMask = 0x01;
  CS4304_SetVolume(&hcs4304, hcs4304.Volume); /* re-apply with the mask */

  /* Bind DAC handle for volume/mute (Audio_Bridge_SetVolume/SetMute). */
  Audio_Bridge_SetDacHandle(&hcs4304);
  ChannelConsole_SetDacHandle(&hcs4304);

  /* DAC is ready. USB audio will start when host begins streaming. */
  /* I2S DMA is started in AUDIO_Init_HS() when USB audio is activated. */

  /* Start I2S DMA for standalone playback */
  Audio_StartPlayback();

  /* Default DC level 0 for CH2-CH4 (true 0 V via the per-channel zero
   * calibration in audio_tone_dc.c) */
  Audio_SetDCLevel(2, 0);
  Audio_SetDCLevel(3, 0);
  Audio_SetDCLevel(4, 0);

  /* MX_TIM12_Init() reconfigured PB15 as TIM12_CH2 AF — override it back
   * to plain GPIO output so hp_ctl stays a GPIO. */
  {
    GPIO_InitTypeDef gi = {0};
    gi.Pin = HP_CTL_Pin;
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(HP_CTL_GPIO_Port, &gi);
    HAL_GPIO_WritePin(HP_CTL_GPIO_Port, HP_CTL_Pin,
                      GPIO_PIN_RESET); /* LOW = hp_ctl off (filter mode) */
  }

  /* filter_ctl (TIM3_CH1, PC6): keep LOW at boot. */

  /* RS485 idle Hi-Z, switch defaults, bypass/gain session, ready banner. */
  ChannelConsole_Init();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    Audio_I2S1_Poll();     /* Compatibility hook; I2S1 refills in DMA callbacks */
    USB_App_Task();        /* UAC BODY drain + CDC */
    ChannelConsole_Poll(); /* RS485 console + LED chaser */
    USB_App_Task();        /* drain UAC FIFO after a console TX */
    Audio_CpuLoad_Poll();  /* queue-mode NoteBank producer (no-op otherwise) */
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

  /** Supply configuration update enable
   */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
   */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
  }

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 3;
  RCC_OscInitStruct.PLL.PLLN = 67;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 10;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 1136;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
}

/**
 * @brief Peripherals Common Clock Configuration
 * @retval None
 */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
   */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CKPER;
  PeriphClkInitStruct.CkperClockSelection = RCC_CLKPSOURCE_HSE;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
   */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/**
 * @brief  This function is executed in case of error occurrence.
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
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

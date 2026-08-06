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
#include "gpio.h"
#include "i2c.h"
#include "sai.h"
#include "usart.h"
#include "usb_otg.h"


/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "effect_console.h"
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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Console / RS485 / ADC bring-up / LED flash live in effect_console.c */

/* ---------------- SAI TDM capture -> USB mono stream ----------------
 *
 * Both TLV320ADC6140s share BCLK/FSYNC from SAI1_A (master RX):
 *   SAI1_B (slave)  <- ADC1 (0x4C, PE3)  = usb ch 1..4  (TDM slots 0..3)
 *   SAI1_A (master) <- ADC2 (0x4D, PE6)  = usb ch 5..8  (TDM slots 0..3)
 *
 * Circular DMA fills each block's ring buffer; every half (1 ms = 96
 * frames x 4 slots) the callback picks the selected channel's slot and
 * pushes 96 mono samples into the TinyUSB audio FIFO.
 *
 * Buffers live in AXI SRAM (.axi_ram section): DMA1 cannot access the
 * default DTCM RAM.  NOLOAD section — never assume zero-init. */
/* 1 ms of frames — deliberately MATCHED to the USB frame interval so the
 * producer (this callback) and the consumer (one iso IN token per 1 ms)
 * tick at the same cadence and the FIFO stays near-constant.  With 2 ms
 * halves the FIFO swung 192->0 every cycle and any frame landing on the
 * empty phase got a short packet = audible dropout. */
#define SAI_FRAMES_PER_HALF 96 /* 1 ms at 96 kHz */
#define SAI_WORDS_PER_HALF (SAI_FRAMES_PER_HALF * 4)
static int32_t sai_rx_a[2 * SAI_WORDS_PER_HALF]
    __attribute__((section(".axi_ram"), aligned(32)));
static int32_t sai_rx_b[2 * SAI_WORDS_PER_HALF]
    __attribute__((section(".axi_ram"), aligned(32)));

/** De-interleave one DMA half-buffer and feed the USB mic FIFO. */
static void SAI_FeedUSB(SAI_HandleTypeDef *hsai, uint8_t second_half) {
  uint8_t ch = usb_adc_ch; /* snapshot: 1..8 */
  uint8_t want_a = (ch >= 5);
  uint8_t is_a = (hsai->Instance == SAI1_Block_A);
  if (want_a != is_a)
    return; /* this block doesn't carry the selected channel */

  const int32_t *buf =
      (is_a ? sai_rx_a : sai_rx_b) + (second_half ? SAI_WORDS_PER_HALF : 0);
  uint8_t slot = (uint8_t)((ch - 1) & 3);

  static int32_t mono[SAI_FRAMES_PER_HALF];
  for (uint32_t i = 0; i < SAI_FRAMES_PER_HALF; i++) {
    mono[i] = buf[i * 4 + slot];
  }
  USB_Audio_Write((const uint8_t *)mono, sizeof mono);
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai) {
  SAI_FeedUSB(hsai, 0);
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai) { SAI_FeedUSB(hsai, 1); }

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

  /* USER CODE BEGIN 1 */
  /* Same as Channel Card: fetch from flash at high SYSCLK without I-cache
   * burns cycles in USB/SAI ISRs. I-cache only — D-cache needs non-cacheable
   * MPU regions for DMA/AXI buffers, which this MPU_Config does not set up. */
  SCB_EnableICache();
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick.
   */
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
  MX_SAI1_Init();
  MX_I2C2_Init();
  MX_SAI2_Init();
  MX_UART4_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_SAI3_Init();
  /* USER CODE BEGIN 2 */

  /* RS485 idle Hi-Z, LED blink, AUDIO_EN, both ADCs configured. */
  EffectConsole_Init();

  /* TinyUSB device stack (UAC2 mic + CDC console).  MX_USB_OTG_FS_PCD_Init
   * already set up clocks/pins/NVIC via HAL; tud_init() re-owns and
   * reconfigures the OTG core registers.  HAL_PCD_Start is never called,
   * so the two stacks don't fight. */
  USB_App_Init();

  /* ISO-IN arming is timing-critical: if the USB ISR is delayed past the
   * next IN token, that frame is missed (measured ~5/s with everything at
   * equal priority).  Demote the SAI DMA interrupts below USB — their
   * deadline is a lazy 2 ms (half-buffer), so USB preempting them costs
   * nothing while letting endpoint re-arm happen immediately. */
  HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 1, 0);
  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 1, 0);

  /* Slave block (B) first so it's armed when the master starts clocking. */
  if (HAL_SAI_Receive_DMA(&hsai_BlockB1, (uint8_t *)sai_rx_b,
                          2 * SAI_WORDS_PER_HALF) != HAL_OK ||
      HAL_SAI_Receive_DMA(&hsai_BlockA1, (uint8_t *)sai_rx_a,
                          2 * SAI_WORDS_PER_HALF) != HAL_OK) {
    EffectConsole_Reply("err: SAI capture start failed\r\n");
  }

  EffectConsole_Reply("\r\n"
              "**************************************************\r\n"
              "*  Effect Card [E] ready  (multi-drop RS485)     *\r\n"
              "*  Type 'help' or 'e:help' for the console menu. *\r\n"
              "**************************************************\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    EffectConsole_Poll(); /* RS485 console + LED flash */
    USB_App_Task();        /* TinyUSB stack + CDC console */
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
   */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
   */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
  }

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType =
      RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 78;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 15;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 1024;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                                RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) {
    Error_Handler();
  }
}

/**
 * @brief Peripherals Common Clock Configuration
 * @retval None
 */
void PeriphCommonClock_Config(void) {
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
   */
  PeriphClkInitStruct.PeriphClockSelection =
      RCC_PERIPHCLK_SAI1 | RCC_PERIPHCLK_SAI2 | RCC_PERIPHCLK_SAI3;
  PeriphClkInitStruct.PLL3.PLL3M = 4;
  PeriphClkInitStruct.PLL3.PLL3N = 64;
  PeriphClkInitStruct.PLL3.PLL3P = 16;
  PeriphClkInitStruct.PLL3.PLL3Q = 16;
  PeriphClkInitStruct.PLL3.PLL3R = 16;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 0;
  PeriphClkInitStruct.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLL3;
  PeriphClkInitStruct.Sai23ClockSelection = RCC_SAI23CLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* MPU Configuration */

void MPU_Config(void) {
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
void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
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
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

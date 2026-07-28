/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define Effect_FS_Pin GPIO_PIN_0
#define Effect_FS_GPIO_Port GPIOC
#define DAC_D1_Pin GPIO_PIN_1
#define DAC_D1_GPIO_Port GPIOC
#define Effect_SD_Pin GPIO_PIN_0
#define Effect_SD_GPIO_Port GPIOA
#define RGB_B_Pin GPIO_PIN_1
#define RGB_B_GPIO_Port GPIOA
#define Effect_SCK_Pin GPIO_PIN_2
#define Effect_SCK_GPIO_Port GPIOA
#define RGB_R_Pin GPIO_PIN_3
#define RGB_R_GPIO_Port GPIOA
#define DAC_SYNC_Pin GPIO_PIN_4
#define DAC_SYNC_GPIO_Port GPIOA
#define DAC_CLK_Pin GPIO_PIN_5
#define DAC_CLK_GPIO_Port GPIOA
#define RGB_G_Pin GPIO_PIN_6
#define RGB_G_GPIO_Port GPIOA
#define DAC_D0_Pin GPIO_PIN_7
#define DAC_D0_GPIO_Port GPIOA
#define HP_SW_Pin GPIO_PIN_5
#define HP_SW_GPIO_Port GPIOC
#define BP_SW_Pin GPIO_PIN_0
#define BP_SW_GPIO_Port GPIOB
#define LP_SW_Pin GPIO_PIN_1
#define LP_SW_GPIO_Port GPIOB
#define VCA_SW_Pin GPIO_PIN_2
#define VCA_SW_GPIO_Port GPIOB
#define BYPASS_SW_Pin GPIO_PIN_10
#define BYPASS_SW_GPIO_Port GPIOB
#define DAC_S_SYNC_Pin GPIO_PIN_12
#define DAC_S_SYNC_GPIO_Port GPIOB
#define VCF_SW_Pin GPIO_PIN_13
#define VCF_SW_GPIO_Port GPIOB
#define SCF_SW_Pin GPIO_PIN_14
#define SCF_SW_GPIO_Port GPIOB
#define HP_CTL_Pin GPIO_PIN_15
#define HP_CTL_GPIO_Port GPIOB
#define Filter_CTL_Pin GPIO_PIN_6
#define Filter_CTL_GPIO_Port GPIOC
#define DAC_RST_Pin GPIO_PIN_7
#define DAC_RST_GPIO_Port GPIOC
#define DAC_MCLK_Pin GPIO_PIN_8
#define DAC_MCLK_GPIO_Port GPIOA
#define DAC_S_CLK_Pin GPIO_PIN_9
#define DAC_S_CLK_GPIO_Port GPIOA
#define RS485_CTL_Pin GPIO_PIN_3
#define RS485_CTL_GPIO_Port GPIOB
#define LED_R_Pin GPIO_PIN_8
#define LED_R_GPIO_Port GPIOB
#define LED_Y_Pin GPIO_PIN_9
#define LED_Y_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

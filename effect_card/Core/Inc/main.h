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
#define ADC1_DATA_Pin GPIO_PIN_3
#define ADC1_DATA_GPIO_Port GPIOE
#define ADC_FS_Pin GPIO_PIN_4
#define ADC_FS_GPIO_Port GPIOE
#define ADC_SCK_Pin GPIO_PIN_5
#define ADC_SCK_GPIO_Port GPIOE
#define ADC2_DATA_Pin GPIO_PIN_6
#define ADC2_DATA_GPIO_Port GPIOE
#define AUDIO_EN_Pin GPIO_PIN_7
#define AUDIO_EN_GPIO_Port GPIOA
#define ADC1_INT_Pin GPIO_PIN_4
#define ADC1_INT_GPIO_Port GPIOC
#define ADC2_INT_Pin GPIO_PIN_5
#define ADC2_INT_GPIO_Port GPIOC
#define LED_Y_Pin GPIO_PIN_12
#define LED_Y_GPIO_Port GPIOB
#define LED_R_Pin GPIO_PIN_13
#define LED_R_GPIO_Port GPIOB
#define INPUT_SW2_Pin GPIO_PIN_14
#define INPUT_SW2_GPIO_Port GPIOB
#define INPUT_SW1_Pin GPIO_PIN_15
#define INPUT_SW1_GPIO_Port GPIOB
#define CODEC_DATA4_Pin GPIO_PIN_11
#define CODEC_DATA4_GPIO_Port GPIOD
#define CODEC_FS_Pin GPIO_PIN_12
#define CODEC_FS_GPIO_Port GPIOD
#define CODEC_SCK_Pin GPIO_PIN_13
#define CODEC_SCK_GPIO_Port GPIOD
#define EN_48V_Pin GPIO_PIN_6
#define EN_48V_GPIO_Port GPIOC
#define PG_48V_Pin GPIO_PIN_7
#define PG_48V_GPIO_Port GPIOC
#define AUDIO_BUS_SCK_Pin GPIO_PIN_0
#define AUDIO_BUS_SCK_GPIO_Port GPIOD
#define AUDIO_BUS_DATA_Pin GPIO_PIN_1
#define AUDIO_BUS_DATA_GPIO_Port GPIOD
#define AUDIO_BUS_FS_Pin GPIO_PIN_4
#define AUDIO_BUS_FS_GPIO_Port GPIOD
#define CODEC_MCLK_Pin GPIO_PIN_0
#define CODEC_MCLK_GPIO_Port GPIOE
#define RS485_CTL_Pin GPIO_PIN_1
#define RS485_CTL_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

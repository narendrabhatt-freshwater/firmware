/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : SAI.c
  * Description        : This file provides code for the configuration
  *                      of the SAI instances.
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
#include "sai.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

SAI_HandleTypeDef hsai_BlockB4;

/* SAI4 init function */
void MX_SAI4_Init(void)
{

  /* USER CODE BEGIN SAI4_Init 0 */

  /* USER CODE END SAI4_Init 0 */

  /* USER CODE BEGIN SAI4_Init 1 */

  /* USER CODE END SAI4_Init 1 */

  hsai_BlockB4.Instance = SAI4_Block_B;
  hsai_BlockB4.Init.Protocol = SAI_FREE_PROTOCOL;
  hsai_BlockB4.Init.AudioMode = SAI_MODESLAVE_TX;
  hsai_BlockB4.Init.DataSize = SAI_DATASIZE_8;
  hsai_BlockB4.Init.FirstBit = SAI_FIRSTBIT_MSB;
  hsai_BlockB4.Init.ClockStrobing = SAI_CLOCKSTROBING_FALLINGEDGE;
  hsai_BlockB4.Init.Synchro = SAI_ASYNCHRONOUS;
  hsai_BlockB4.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockB4.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockB4.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
  hsai_BlockB4.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockB4.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockB4.Init.TriState = SAI_OUTPUT_NOTRELEASED;
  hsai_BlockB4.Init.PdmInit.Activation = DISABLE;
  hsai_BlockB4.Init.PdmInit.MicPairsNbr = 0;
  hsai_BlockB4.Init.PdmInit.ClockEnable = SAI_PDM_CLOCK1_ENABLE;
  hsai_BlockB4.FrameInit.FrameLength = 8;
  hsai_BlockB4.FrameInit.ActiveFrameLength = 1;
  hsai_BlockB4.FrameInit.FSDefinition = SAI_FS_STARTFRAME;
  hsai_BlockB4.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
  hsai_BlockB4.FrameInit.FSOffset = SAI_FS_FIRSTBIT;
  hsai_BlockB4.SlotInit.FirstBitOffset = 0;
  hsai_BlockB4.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
  hsai_BlockB4.SlotInit.SlotNumber = 1;
  hsai_BlockB4.SlotInit.SlotActive = 0x00000000;
  if (HAL_SAI_Init(&hsai_BlockB4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SAI4_Init 2 */

  /* USER CODE END SAI4_Init 2 */

}
static uint32_t SAI4_client =0;

void HAL_SAI_MspInit(SAI_HandleTypeDef* saiHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
/* SAI4 */
    if(saiHandle->Instance==SAI4_Block_B)
    {
      /* SAI4 clock enable */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI4B;
    PeriphClkInitStruct.Sai4BClockSelection = RCC_SAI4BCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

      if (SAI4_client == 0)
      {
       __HAL_RCC_SAI4_CLK_ENABLE();
      }
    SAI4_client ++;

    /**SAI4_B_Block_B GPIO Configuration
    PC0     ------> SAI4_FS_B
    PA0     ------> SAI4_SD_B
    PA2     ------> SAI4_SCK_B
    */
    GPIO_InitStruct.Pin = Effect_FS_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI4;
    HAL_GPIO_Init(Effect_FS_GPIO_Port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = Effect_SD_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF10_SAI4;
    HAL_GPIO_Init(Effect_SD_GPIO_Port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = Effect_SCK_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI4;
    HAL_GPIO_Init(Effect_SCK_GPIO_Port, &GPIO_InitStruct);

    }
}

void HAL_SAI_MspDeInit(SAI_HandleTypeDef* saiHandle)
{

/* SAI4 */
    if(saiHandle->Instance==SAI4_Block_B)
    {
    SAI4_client --;
      if (SAI4_client == 0)
      {
      /* Peripheral clock disable */
      __HAL_RCC_SAI4_CLK_DISABLE();
      }

    /**SAI4_B_Block_B GPIO Configuration
    PC0     ------> SAI4_FS_B
    PA0     ------> SAI4_SD_B
    PA2     ------> SAI4_SCK_B
    */
    HAL_GPIO_DeInit(Effect_FS_GPIO_Port, Effect_FS_Pin);

    HAL_GPIO_DeInit(GPIOA, Effect_SD_Pin|Effect_SCK_Pin);

    }
}

/**
  * @}
  */

/**
  * @}
  */

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  /*Configure GPIO pin Output Level: ETHRST high = KSZ8863 not held in reset.
   * The switch must keep forwarding pass-through traffic across MCU restarts;
   * a cold-boot-only reset is issued from ksz8863_boot_init(). Leaving this at
   * RESET held the switch in reset across warm reboots (dead RJ45 link). */
  HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(STAT_LED_GPIO_Port, STAT_LED_Pin, GPIO_PIN_RESET);

  /* MAX31865 chip-selects idle high (deselected). */
  HAL_GPIO_WritePin(RTD0_CS_GPIO_Port, RTD0_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(RTD1_CS_GPIO_Port, RTD1_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(RTD2_CS_GPIO_Port, RTD2_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(RTD3_CS_GPIO_Port, RTD3_CS_Pin, GPIO_PIN_SET);

  /* Range selects and status LEDs default low. */
  HAL_GPIO_WritePin(RTD0_RANG_GPIO_Port, RTD0_RANG_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RTD1_RANG_GPIO_Port, RTD1_RANG_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RTD2_RANG_GPIO_Port, RTD2_RANG_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RTD3_RANG_GPIO_Port, RTD3_RANG_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOE, RTD0_STAT_Pin|RTD1_STAT_Pin|RTD2_STAT_Pin|RTD3_STAT_Pin,
                    GPIO_PIN_RESET);

  /*Configure GPIO pin : ETHINT_Pin */
  GPIO_InitStruct.Pin = ETHINT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : ETHRST_Pin */
  GPIO_InitStruct.Pin = ETHRST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ETHRST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : STAT_LED_Pin */
  GPIO_InitStruct.Pin = STAT_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(STAT_LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : FACT_RES_Pin */
  GPIO_InitStruct.Pin = FACT_RES_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(FACT_RES_GPIO_Port, &GPIO_InitStruct);

  /* MAX31865 chip-selects (board-rev pinout, one per port):
   *   CS0 = PA8, CS1 = PC8, CS2 = PD9, CS3 = PB15.
   * RANG stays on PD10 (RTD0), PD8 (RTD1), PB14 (RTD2), PE15 (RTD3). */
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  /* CS0 (PA8) on port A. */
  GPIO_InitStruct.Pin = RTD0_CS_Pin;
  HAL_GPIO_Init(RTD0_CS_GPIO_Port, &GPIO_InitStruct);

  /* CS1 (PC8) on port C. */
  GPIO_InitStruct.Pin = RTD1_CS_Pin;
  HAL_GPIO_Init(RTD1_CS_GPIO_Port, &GPIO_InitStruct);

  /* CS2 (PD9) + RANG on port D (PD8, PD9, PD10). */
  GPIO_InitStruct.Pin = RTD2_CS_Pin|RTD0_RANG_Pin|RTD1_RANG_Pin;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* CS3 (PB15) + RANG on port B (PB14, PB15). */
  GPIO_InitStruct.Pin = RTD3_CS_Pin|RTD2_RANG_Pin;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* Channel RANG (PE15) + status LEDs (PE11..PE14) on port E. */
  GPIO_InitStruct.Pin = RTD3_RANG_Pin|RTD0_STAT_Pin|RTD1_STAT_Pin
                        |RTD2_STAT_Pin|RTD3_STAT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

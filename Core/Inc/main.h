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
#include "stm32f4xx_hal.h"

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
#define ETHINT_Pin GPIO_PIN_1
#define ETHINT_GPIO_Port GPIOB
#define ETHRST_Pin GPIO_PIN_11
#define ETHRST_GPIO_Port GPIOD
#define STAT_LED_Pin GPIO_PIN_9
#define STAT_LED_GPIO_Port GPIOE
#define FACT_RES_Pin GPIO_PIN_10
#define FACT_RES_GPIO_Port GPIOE

/* ---- 4RTD analog board HW2.1 (ADS1220 x4) ------------------------------- */
/* SPI1: SCLK = PA5, MISO = PA6, MOSI = PB5 (all AF5). Configured in spi_bus.c. */

/* Per-channel ADS1220 chip-select (active-low, idle high).
 * HW2.1 pinout: CS0=PC6, CS1=PD15, CS2=PD14, CS3=PD13. No range-select
 * pins on this revision: the measuring range is set by the ADS1220 PGA gain. */
#define RTD0_CS_Pin        GPIO_PIN_6
#define RTD0_CS_GPIO_Port  GPIOC
#define RTD1_CS_Pin        GPIO_PIN_15
#define RTD1_CS_GPIO_Port  GPIOD
#define RTD2_CS_Pin        GPIO_PIN_14
#define RTD2_CS_GPIO_Port  GPIOD
#define RTD3_CS_Pin        GPIO_PIN_13
#define RTD3_CS_GPIO_Port  GPIOD

/* Per-channel status LED (active-high). */
#define RTD0_STAT_Pin       GPIO_PIN_14
#define RTD0_STAT_GPIO_Port GPIOE
#define RTD1_STAT_Pin       GPIO_PIN_13
#define RTD1_STAT_GPIO_Port GPIOE
#define RTD2_STAT_Pin       GPIO_PIN_12
#define RTD2_STAT_GPIO_Port GPIOE
#define RTD3_STAT_Pin       GPIO_PIN_11
#define RTD3_STAT_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

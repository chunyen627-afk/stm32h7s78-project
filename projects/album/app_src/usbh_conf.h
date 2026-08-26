/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbh_conf.h
  * @author         : MCD Application Team
  * @brief          : Header for usbh_conf.c file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
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
#ifndef __USBH_CONF_H
#define __USBH_CONF_H
#ifdef __cplusplus
extern "C" {
#endif
/* Includes ------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"

#include "stm32h7rsxx.h"
#include "stm32h7rsxx_hal.h"

/* USER CODE BEGIN INCLUDE */

/* USER CODE END INCLUDE */

/** @addtogroup STM32_USB_HOST_LIBRARY
  * @{
  */

/** @defgroup USBH_CONF
  * @brief usb host low level driver configuration file
  * @{
  */

/** @defgroup USBH_CONF_Exported_Variables USBH_CONF_Exported_Variables
  * @brief Public variables.
  * @{
  */

/**
  * @}
  */

/** @defgroup USBH_CONF_Exported_Defines USBH_CONF_Exported_Defines
  * @brief Defines for configuration of the Usb host.
  * @{
  */

/*----------   -----------*/
#define USBH_MAX_NUM_ENDPOINTS      2U

/*----------   -----------*/
#define USBH_MAX_NUM_INTERFACES      10U

/*----------   -----------*/
#define USBH_MAX_NUM_CONFIGURATION      1U

/*----------   -----------*/
#define USBH_KEEP_CFG_DESCRIPTOR      1U

/*----------   -----------*/
#define USBH_MAX_NUM_SUPPORTED_CLASS      1U

/*----------   -----------*/
#define USBH_MAX_SIZE_CONFIGURATION      512U

/*----------   -----------*/
#define USBH_MAX_DATA_BUFFER      512U

/*----------   -----------*/
/* **相簿沒有 UART，也沒有 printf 轉向。** 治具那邊是 2（訊息走 COM4），
 * 這裡一定要 0 —— 不然每一行 log 都會呼叫到不存在的 __io_putchar。
 * 相簿的觀察手段是 DTCM 黑盒子（見 usbaudio.c 的 UBOX）。 */
#define USBH_DEBUG_LEVEL      0U

/*----------   -----------*/
#define USBH_USE_OS      0U

/****************************************/
/* #define for FS and HS identification */
#define HOST_HS 		0
#define HOST_FS 		1

#if (USBH_USE_OS == 1)
  #include "cmsis_os.h"
  #define USBH_PROCESS_PRIO          osPriorityNormal
  #define USBH_PROCESS_STACK_SIZE    ((uint16_t)0)
#endif /* (USBH_USE_OS == 1) */

/**
  * @}
  */

/** @defgroup USBH_CONF_Exported_Macros USBH_CONF_Exported_Macros
  * @brief Aliases.
  * @{
  */

/* Memory management macros */

/** Alias for memory allocation. */
/* **不要用 malloc。** 相簿的堆疊只有 1536 bytes（連結腳本的
 * ._user_heap_stack），而 AUDIO_HandleTypeDef 比那還大；而且 USB 的狀態機
 * 之後會從 TIM7 中斷裡跑，malloc 不是可重入的。
 * 改成一塊固定的靜態記憶體，配置失敗會在黑盒子留下記號。 */
void *usbh_static_alloc(uint32_t size);
void  usbh_static_free(void *p);
#define USBH_malloc         usbh_static_alloc

/** Alias for memory release. */
#define USBH_free           usbh_static_free

/** Alias for memory set. */
#define USBH_memset         memset

/** Alias for memory copy. */
#define USBH_memcpy         memcpy

/* DEBUG macros */

#if (USBH_DEBUG_LEVEL > 0U)
#define  USBH_UsrLog(...)   do { \
                            printf(__VA_ARGS__); \
                            printf("\n"); \
} while (0)
#else
#define USBH_UsrLog(...) do {} while (0)
#endif

#if (USBH_DEBUG_LEVEL > 1U)

#define  USBH_ErrLog(...) do { \
                            printf("ERROR: "); \
                            printf(__VA_ARGS__); \
                            printf("\n"); \
} while (0)
#else
#define USBH_ErrLog(...) do {} while (0)
#endif

#if (USBH_DEBUG_LEVEL > 2U)
#define  USBH_DbgLog(...)   do { \
                            printf("DEBUG : "); \
                            printf(__VA_ARGS__); \
                            printf("\n"); \
} while (0)
#else
#define USBH_DbgLog(...) do {} while (0)
#endif

/**
  * @}
  */

/** @defgroup USBH_CONF_Exported_Types USBH_CONF_Exported_Types
  * @brief Types.
  * @{
  */

/**
  * @}
  */

/** @defgroup USBH_CONF_Exported_FunctionsPrototype USBH_CONF_Exported_FunctionsPrototype
  * @brief Declaration of public functions for Usb host.
  * @{
  */

/* Exported functions -------------------------------------------------------*/

/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __USBH_CONF_H */


/**
 * app_config.h - 보드별 설정 (CubeMX 프로젝트에 맞게 수정)
 *
 * 기본값: STM32F4 + SPI1(W25Q32) + USART2(PC, ST-Link VCP)
 *   SPI1 : Mode 0 (CPOL=0, CPHA=0), 8-bit, MSB first, NSS = Software, <= 21 MHz
 *   CS   : PA4 (GPIO Output, 기본 High)
 *   UART : 921600 8N1 (안 되면 115200 으로 낮추고 PC 쪽 --baud 도 맞출 것)
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "main.h"   /* CubeMX 가 생성한 HAL include / 핀 라벨 */

extern SPI_HandleTypeDef  hspi1;
extern UART_HandleTypeDef huart2;

#define APP_FLASH_SPI       (&hspi1)
#define APP_PC_UART         (&huart2)

/* CubeMX 에서 CS 핀에 'FLASH_CS' 라벨을 달면 main.h 의 정의가 자동으로 사용된다. */
#ifdef FLASH_CS_Pin
#define W25Q_CS_GPIO_Port   FLASH_CS_GPIO_Port
#define W25Q_CS_Pin         FLASH_CS_Pin
#else
#define W25Q_CS_GPIO_Port   GPIOA
#define W25Q_CS_Pin         GPIO_PIN_4
#endif

/* BMP 를 저장할 Flash 시작 주소 (64 KB 정렬 권장) */
#define APP_BMP_FLASH_BASE  0x000000u

#endif /* APP_CONFIG_H */

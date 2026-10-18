/* host_sim/main.h - PC 에서 펌웨어를 테스트하기 위한 최소 STM32 HAL 대체 헤더 */
#ifndef SIM_MAIN_H
#define SIM_MAIN_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
typedef struct { int dummy; } GPIO_TypeDef;
typedef struct { int dummy; } SPI_HandleTypeDef;
typedef struct { int fd; } UART_HandleTypeDef;

extern GPIO_TypeDef sim_gpioa;
#define GPIOA       (&sim_gpioa)
#define GPIO_PIN_4  ((uint16_t)0x0010)
#define HAL_MAX_DELAY 0xFFFFFFFFu

#define __HAL_UART_CLEAR_OREFLAG(h) ((void)(h))

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *d, uint16_t n, uint32_t tmo);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo);

#endif

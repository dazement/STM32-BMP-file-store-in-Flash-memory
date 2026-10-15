#ifndef UART_FLASH_TEST_H
#define UART_FLASH_TEST_H

#include "main.h"

void UartFlashTest_Init(QSPI_HandleTypeDef *qspi, UART_HandleTypeDef *uart,
                        const uint8_t jedec[3]);
void UartFlashTest_Poll(void);

#endif

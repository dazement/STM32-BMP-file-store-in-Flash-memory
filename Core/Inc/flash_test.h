#ifndef FLASH_TEST_H
#define FLASH_TEST_H

#include "main.h"

/* One bounded test per boot. Foreign contents in the test sector are preserved. */
const char *FlashTest_Run(QSPI_HandleTypeDef *qspi, UART_HandleTypeDef *uart,
                          const uint8_t jedec[3]);

HAL_StatusTypeDef FlashTest_Read(QSPI_HandleTypeDef *qspi, uint32_t address,
                                 uint8_t *data, uint32_t count);
HAL_StatusTypeDef FlashTest_Erase(QSPI_HandleTypeDef *qspi, uint32_t address);
HAL_StatusTypeDef FlashTest_EraseBlock64(QSPI_HandleTypeDef *qspi, uint32_t address);
HAL_StatusTypeDef FlashTest_ProgramPage(QSPI_HandleTypeDef *qspi, uint32_t address,
                                       uint8_t *data, uint32_t count);

#endif

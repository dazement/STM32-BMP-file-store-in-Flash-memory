#ifndef GRAY_FLASH_H
#define GRAY_FLASH_H
#include "main.h"
void GrayFlash_Send(QSPI_HandleTypeDef *flash, UART_HandleTypeDef *uart,
                     uint32_t file_size, uint32_t expected_crc, uint8_t output_bpp);
#endif

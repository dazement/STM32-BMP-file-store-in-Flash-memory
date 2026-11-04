#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include "main.h"

/* W25Q128 on the board's QUADSPI, driven with 1-line SPI commands. */
HAL_StatusTypeDef QspiFlash_ReadId(QSPI_HandleTypeDef *qspi, uint8_t id[3]);
HAL_StatusTypeDef QspiFlash_Read(QSPI_HandleTypeDef *qspi, uint32_t address,
                                 uint8_t *data, uint32_t count);
HAL_StatusTypeDef QspiFlash_EraseSector(QSPI_HandleTypeDef *qspi, uint32_t address);
HAL_StatusTypeDef QspiFlash_EraseBlock64(QSPI_HandleTypeDef *qspi, uint32_t address);
HAL_StatusTypeDef QspiFlash_ProgramPage(QSPI_HandleTypeDef *qspi, uint32_t address,
                                        uint8_t *data, uint32_t count);

#endif

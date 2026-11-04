#ifndef UART_COMMANDS_H
#define UART_COMMANDS_H

#include "main.h"

/* Commands from the PC scripts over USART1: UFT1 (1 KiB check), UFL1 (store
   file), UFR1 (read file), UFS1 (baud), UFG1 (Gray), '?' (ID), '!' (stats). */
void UartCommands_Init(QSPI_HandleTypeDef *qspi, UART_HandleTypeDef *uart,
                       const uint8_t jedec[3]);
void UartCommands_Poll(void);

#endif

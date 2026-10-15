#include "main.h"
HAL_StatusTypeDef UartDmaTx_Begin(UART_HandleTypeDef *uart);
HAL_StatusTypeDef UartDmaTx_Write(const uint8_t *data, uint32_t count);
HAL_StatusTypeDef UartDmaTx_Flush(void);
void UartDmaTx_Abort(void);
void UartDmaTx_NotifyError(UART_HandleTypeDef *uart);

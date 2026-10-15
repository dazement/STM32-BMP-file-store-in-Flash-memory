#include "main.h"
HAL_StatusTypeDef UartDmaRx_Start(UART_HandleTypeDef *uart);
HAL_StatusTypeDef UartDmaRx_Read(uint8_t *data, uint32_t count, uint32_t timeout_ms);
void UartDmaRx_GetStats(uint32_t *a, uint32_t *b, uint32_t *overruns);

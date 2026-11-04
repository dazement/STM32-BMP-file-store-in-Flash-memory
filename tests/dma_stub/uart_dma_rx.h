#include "main.h"
typedef struct
{
  uint32_t received;
  uint32_t bank_full[2];
  uint32_t half_full;
  uint32_t line_idle;
  uint32_t sleeps;
  uint32_t overruns;
  uint32_t parity_errors, noise_errors, framing_errors, overrun_errors;
} UartDmaRxDebug;
extern volatile UartDmaRxDebug uart_rx_debug;
HAL_StatusTypeDef UartDmaRx_Start(UART_HandleTypeDef *uart);
HAL_StatusTypeDef UartDmaRx_Read(uint8_t *data, uint32_t count, uint32_t timeout_ms);
void UartDmaRx_UartIrqHandler(UART_HandleTypeDef *uart);
void UartDmaRx_GetStats(uint32_t *a, uint32_t *b, uint32_t *overruns);

#ifndef UART_DMA_RX_H
#define UART_DMA_RX_H

#include "main.h"

/* Receive counters for the debugger. Add uart_rx_debug to Live Watch.
   Event counters restart with UartDmaRx_Start; error counters do not. */
typedef struct
{
  uint32_t received;      /* bytes announced by the receive interrupts */
  uint32_t bank_full[2];  /* DMA transfer complete: bank A, bank B */
  uint32_t half_full;     /* DMA half transfer */
  uint32_t line_idle;     /* USART IDLE: the sender paused */
  uint32_t sleeps;        /* WFI entries while waiting for data */
  uint32_t overruns;
  uint32_t parity_errors, noise_errors, framing_errors, overrun_errors;
} UartDmaRxDebug;

extern volatile UartDmaRxDebug uart_rx_debug;

HAL_StatusTypeDef UartDmaRx_Start(UART_HandleTypeDef *uart);
HAL_StatusTypeDef UartDmaRx_Read(uint8_t *data, uint32_t count, uint32_t timeout_ms);
void UartDmaRx_UartIrqHandler(UART_HandleTypeDef *uart);
void UartDmaRx_GetStats(uint32_t *a, uint32_t *b, uint32_t *overruns);

#endif

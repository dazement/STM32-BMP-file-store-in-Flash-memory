#include "uart_dma_tx.h"
#include <string.h>
#define TX_BYTES 4096U

/* D-cache remains disabled in this project, as for the RX DMA buffers. */
static uint8_t tx_buffers[2][TX_BYTES] __attribute__((aligned(32)));
static UART_HandleTypeDef *tx_uart;
static volatile uint32_t tx_busy, tx_error;
static uint32_t filling, used;

static HAL_StatusTypeDef wait_tx(void)
{
  uint32_t start = HAL_GetTick();
  while (tx_busy)
  {
    if (tx_error) return HAL_ERROR;
    if ((uint32_t)(HAL_GetTick() - start) >= 3000U) return HAL_TIMEOUT;
    HAL_Delay(1);
  }
  return tx_error ? HAL_ERROR : HAL_OK;
}

static HAL_StatusTypeDef launch(void)
{
  HAL_StatusTypeDef status = wait_tx();
  if (status != HAL_OK) return status;
  tx_busy = 1;
  status = HAL_UART_Transmit_DMA(tx_uart, tx_buffers[filling], (uint16_t)used);
  if (status != HAL_OK)
  {
    tx_busy = 0;
    tx_error = 1;
    return status;
  }
  filling ^= 1U;
  used = 0;
  return HAL_OK;
}

HAL_StatusTypeDef UartDmaTx_Begin(UART_HandleTypeDef *uart)
{
  if (uart == NULL || uart->hdmatx == NULL || uart->gState != HAL_UART_STATE_READY)
    return HAL_BUSY;
  tx_uart = uart;
  tx_busy = tx_error = filling = used = 0;
  return HAL_OK;
}

HAL_StatusTypeDef UartDmaTx_Write(const uint8_t *data, uint32_t count)
{
  if (tx_uart == NULL || tx_error) return HAL_ERROR;
  while (count)
  {
    uint32_t amount = TX_BYTES - used;
    if (amount > count) amount = count;
    memcpy(tx_buffers[filling] + used, data, amount);
    used += amount;
    data += amount;
    count -= amount;
    if (used == TX_BYTES)
    {
      HAL_StatusTypeDef status = launch();
      if (status != HAL_OK) return status;
    }
  }
  return HAL_OK;
}

HAL_StatusTypeDef UartDmaTx_Flush(void)
{
  if (used)
  {
    HAL_StatusTypeDef status = launch();
    if (status != HAL_OK) return status;
  }
  return wait_tx();
}

void UartDmaTx_Abort(void)
{
  if (tx_uart != NULL) HAL_UART_AbortTransmit(tx_uart);
  tx_busy = used = 0;
  tx_error = 1;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
  if (uart == tx_uart) tx_busy = 0;
}

void UartDmaTx_NotifyError(UART_HandleTypeDef *uart)
{
  if (uart == tx_uart) tx_error = 1;
}

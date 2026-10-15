#include "uart_dma_rx.h"
#include "uart_dma_tx.h"
#include <string.h>

#define BANK_BYTES 4096U
#define TOTAL_BYTES (2U * BANK_BYTES)

/* This project's CPU D-cache is disabled. If cache is enabled later, this
   buffer must remain non-cacheable or receive proper cache maintenance. */
static uint8_t buffer_a[BANK_BYTES] __attribute__((aligned(32)));
static uint8_t buffer_b[BANK_BYTES] __attribute__((aligned(32)));
static UART_HandleTypeDef *rx_uart;
static volatile uint32_t completed_blocks;
static volatile uint32_t bank_completions[2];
static volatile uint32_t overrun_count;
static volatile uint32_t rx_error;
static uint32_t consumed;

static uint32_t produced_count(void)
{
  /* CT identifies the active memory bank. Read CT/NDTR consistently, including
     one completed bank whose IRQ has not yet run. Never accept two lost IRQs. */
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  uint32_t blocks = completed_blocks;
  DMA_HandleTypeDef *dma = rx_uart->hdmarx;
  uint32_t tc_flag = __HAL_DMA_GET_TC_FLAG_INDEX(dma);
  uint32_t ct_before, ct_after, remaining, pending_before, pending;
  do
  {
    pending_before = __HAL_DMA_GET_FLAG(dma, tc_flag);
    ct_before = (dma->Instance->CR & DMA_SxCR_CT) ? 1U : 0U;
    remaining = __HAL_DMA_GET_COUNTER(dma);
    ct_after = (dma->Instance->CR & DMA_SxCR_CT) ? 1U : 0U;
    pending = __HAL_DMA_GET_FLAG(dma, tc_flag);
  } while (ct_before != ct_after || pending_before != pending);
  if (ct_after != (blocks & 1U))
    ++blocks;
  else if (pending && remaining != 0)
  {
    rx_error = 1;
    ++overrun_count;
  }
  uint32_t result = blocks * BANK_BYTES + BANK_BYTES - remaining;
  __DMB();
  __set_PRIMASK(irq_state);
  return result;
}

static void bank_complete(DMA_HandleTypeDef *dma, uint32_t bank)
{
  if (rx_uart == NULL || dma != rx_uart->hdmarx)
    return;
  if (bank != (completed_blocks & 1U))
  {
    rx_error = 1;
    ++overrun_count;
  }
  ++completed_blocks;
  ++bank_completions[bank];
}

static void bank_a_complete(DMA_HandleTypeDef *dma) { bank_complete(dma, 0); }
static void bank_b_complete(DMA_HandleTypeDef *dma) { bank_complete(dma, 1); }

static void dma_error(DMA_HandleTypeDef *dma)
{
  if (rx_uart != NULL && dma == rx_uart->hdmarx)
  {
    rx_error = 1;
    rx_uart->ErrorCode |= HAL_UART_ERROR_DMA;
    ATOMIC_CLEAR_BIT(rx_uart->Instance->CR3, USART_CR3_DMAR);
  }
}

HAL_StatusTypeDef UartDmaRx_Start(UART_HandleTypeDef *uart)
{
  if (uart == NULL || uart->hdmarx == NULL ||
      uart->hdmarx->Init.Mode != DMA_CIRCULAR)
    return HAL_ERROR;
  HAL_StatusTypeDef status = HAL_UART_AbortReceive(uart);
  if (status != HAL_OK)
    return status;
  __HAL_UART_CLEAR_OREFLAG(uart);
  rx_uart = uart;
  completed_blocks = 0;
  bank_completions[0] = bank_completions[1] = 0;
  consumed = 0;
  rx_error = 0;
  /* HAL_UART_Receive_DMA supports only one memory address. Set up the UART
     receive state as ST's helper does, then use the DMA multibuffer API. */
  uart->pRxBuffPtr = buffer_a;
  uart->RxXferSize = BANK_BYTES;
  uart->RxXferCount = BANK_BYTES;
  uart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
  uart->ErrorCode = HAL_UART_ERROR_NONE;
  uart->RxState = HAL_UART_STATE_BUSY_RX;
  DMA_HandleTypeDef *dma = uart->hdmarx;
  dma->XferCpltCallback = bank_a_complete;
  dma->XferM1CpltCallback = bank_b_complete;
  dma->XferHalfCpltCallback = NULL;
  dma->XferM1HalfCpltCallback = NULL;
  dma->XferErrorCallback = dma_error;
  dma->XferAbortCallback = NULL;
  CLEAR_BIT(dma->Instance->CR, DMA_SxCR_CT);
  status = HAL_DMAEx_MultiBufferStart_IT(dma,
              (uint32_t)(uintptr_t)&uart->Instance->RDR,
              (uint32_t)(uintptr_t)buffer_a,
              (uint32_t)(uintptr_t)buffer_b, BANK_BYTES);
  if (status != HAL_OK)
  {
    rx_error = 1;
    uart->RxState = HAL_UART_STATE_READY;
    uart->ErrorCode = HAL_UART_ERROR_DMA;
    return status;
  }
  if (uart->Init.Parity != UART_PARITY_NONE)
    ATOMIC_SET_BIT(uart->Instance->CR1, USART_CR1_PEIE);
  ATOMIC_SET_BIT(uart->Instance->CR3, USART_CR3_EIE | USART_CR3_DMAR);
  return status;
}

HAL_StatusTypeDef UartDmaRx_Read(uint8_t *data, uint32_t count, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint32_t copied = 0;
  while (copied < count)
  {
    if (rx_uart == NULL || rx_error)
      return HAL_ERROR;
    uint32_t available = produced_count() - consumed;
    if (rx_error)
      return HAL_ERROR;
    if (available > TOTAL_BYTES)
    {
      rx_error = 1;
      ++overrun_count;
      return HAL_ERROR;
    }
    if (available)
    {
      uint32_t bank = (consumed / BANK_BYTES) & 1U;
      uint32_t index = consumed & (BANK_BYTES - 1U);
      uint32_t amount = count - copied;
      if (amount > available)
        amount = available;
      if (amount > BANK_BYTES - index)
        amount = BANK_BYTES - index;
      const uint8_t *source = bank == 0 ? buffer_a : buffer_b;
      memcpy(data + copied, source + index, amount);
      /* Fail rather than accept a copy that DMA may have overwritten. */
      if (produced_count() - consumed > TOTAL_BYTES)
      {
        rx_error = 1;
        ++overrun_count;
        return HAL_ERROR;
      }
      if (rx_error)
        return HAL_ERROR;
      consumed += amount;
      copied += amount;
      continue;
    }
    if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
      return HAL_TIMEOUT;
    HAL_Delay(1);
  }
  return HAL_OK;
}

void UartDmaRx_GetStats(uint32_t *a, uint32_t *b, uint32_t *overruns)
{
  uint32_t state = __get_PRIMASK();
  __disable_irq();
  *a = bank_completions[0];
  *b = bank_completions[1];
  *overruns = overrun_count;
  __set_PRIMASK(state);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  UartDmaTx_NotifyError(uart);
  if (uart == rx_uart)
    rx_error = 1;
}

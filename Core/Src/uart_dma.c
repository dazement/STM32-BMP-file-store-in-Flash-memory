#include "uart_dma.h"
#include <string.h>

#define BANK_BYTES 4096U
#define TOTAL_BYTES (2U * BANK_BYTES)
#define TX_BYTES 4096U

/* This project's CPU D-cache is disabled. If cache is enabled later, this
   buffer must remain non-cacheable or receive proper cache maintenance. */
static uint8_t buffer_a[BANK_BYTES] __attribute__((aligned(32)));
static uint8_t buffer_b[BANK_BYTES] __attribute__((aligned(32)));
static UART_HandleTypeDef *rx_uart;
static volatile uint32_t completed_blocks;
static volatile uint32_t rx_error;
/* Bytes the DMA has delivered. Only the receive interrupts write it. */
static volatile uint32_t received;
static uint32_t consumed;
volatile UartDmaRxDebug uart_rx_debug;

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
    ++uart_rx_debug.overruns;
  }
  uint32_t result = blocks * BANK_BYTES + BANK_BYTES - remaining;
  __DMB();
  __set_PRIMASK(irq_state);
  return result;
}

/* Every receive interrupt announces the DMA write position: bank full (TC),
   bank half full (HT) and line idle (IDLE, for data shorter than that). */
static void publish(void)
{
  received = produced_count();
  uart_rx_debug.received = received;
}

static void bank_complete(DMA_HandleTypeDef *dma, uint32_t bank)
{
  if (rx_uart == NULL || dma != rx_uart->hdmarx)
    return;
  if (bank != (completed_blocks & 1U))
  {
    rx_error = 1;
    ++uart_rx_debug.overruns;
  }
  ++completed_blocks;
  ++uart_rx_debug.bank_full[bank];
  publish();
}

static void bank_a_complete(DMA_HandleTypeDef *dma) { bank_complete(dma, 0); }
static void bank_b_complete(DMA_HandleTypeDef *dma) { bank_complete(dma, 1); }

static void bank_half(DMA_HandleTypeDef *dma)
{
  if (rx_uart == NULL || dma != rx_uart->hdmarx)
    return;
  ++uart_rx_debug.half_full;
  publish();
}

static void dma_error(DMA_HandleTypeDef *dma)
{
  if (rx_uart != NULL && dma == rx_uart->hdmarx)
  {
    rx_error = 1;
    rx_uart->ErrorCode |= HAL_UART_ERROR_DMA;
    ATOMIC_CLEAR_BIT(rx_uart->Instance->CR3, USART_CR3_DMAR);
  }
}

/* Called from USART1_IRQHandler before HAL_UART_IRQHandler. HAL leaves IDLE
   alone during standard reception, so an uncleared flag would re-enter. */
void UartDmaRx_UartIrqHandler(UART_HandleTypeDef *uart)
{
  if (uart != rx_uart || !__HAL_UART_GET_FLAG(uart, UART_FLAG_IDLE))
    return;
  __HAL_UART_CLEAR_IDLEFLAG(uart);
  if (READ_BIT(uart->Instance->CR1, USART_CR1_IDLEIE) == 0U)
    return;
  ++uart_rx_debug.line_idle;
  publish();
}

HAL_StatusTypeDef UartDmaRx_Start(UART_HandleTypeDef *uart)
{
  if (uart == NULL || uart->hdmarx == NULL ||
      uart->hdmarx->Init.Mode != DMA_CIRCULAR)
    return HAL_ERROR;
  /* No IDLE interrupt may publish a stale position during the restart. */
  ATOMIC_CLEAR_BIT(uart->Instance->CR1, USART_CR1_IDLEIE);
  HAL_StatusTypeDef status = HAL_UART_AbortReceive(uart);
  if (status != HAL_OK)
    return status;
  __HAL_UART_CLEAR_OREFLAG(uart);
  rx_uart = uart;
  completed_blocks = 0;
  received = 0;
  consumed = 0;
  rx_error = 0;
  uart_rx_debug.received = 0;
  uart_rx_debug.bank_full[0] = uart_rx_debug.bank_full[1] = 0;
  uart_rx_debug.half_full = uart_rx_debug.line_idle = uart_rx_debug.sleeps = 0;
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
  /* Non-NULL half callbacks make HAL enable the HT interrupt. */
  dma->XferHalfCpltCallback = bank_half;
  dma->XferM1HalfCpltCallback = bank_half;
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
  /* A stale IDLE flag only causes one harmless extra announcement. */
  ATOMIC_SET_BIT(uart->Instance->CR1, USART_CR1_IDLEIE);
  return status;
}

/* Sleep until an interrupt. With PRIMASK set, an interrupt that arrives after
   the check stays pending and WFI returns at once, so no wakeup is lost. */
static void wait_for_data(void)
{
  uint32_t irq_state = __get_PRIMASK();
  __disable_irq();
  if (received == consumed && !rx_error)
  {
    ++uart_rx_debug.sleeps;
    __WFI();
    /* Workaround from ST's STM32F4 errata for waking from debug Sleep with
       Flash wait states and prefetch off, both true in this project. */
    __NOP(); __NOP(); __NOP();
  }
  __set_PRIMASK(irq_state);
}

HAL_StatusTypeDef UartDmaRx_Read(uint8_t *data, uint32_t count, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint32_t copied = 0;
  while (copied < count)
  {
    if (rx_uart == NULL || rx_error)
      return HAL_ERROR;
    /* Only the receive interrupts advance 'received'; DMA is not polled. */
    uint32_t available = received - consumed;
    if (available > TOTAL_BYTES)
    {
      rx_error = 1;
      ++uart_rx_debug.overruns;
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
      /* Fail rather than accept a copy that DMA may have overwritten. The
         announced count can trail the DMA, so check the hardware position. */
      if (produced_count() - consumed > TOTAL_BYTES)
      {
        rx_error = 1;
        ++uart_rx_debug.overruns;
        return HAL_ERROR;
      }
      if (rx_error)
        return HAL_ERROR;
      consumed += amount;
      copied += amount;
      continue;
    }
    /* SysTick also wakes the CPU every 1 ms, which drives this timeout. */
    if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
      return HAL_TIMEOUT;
    wait_for_data();
  }
  return HAL_OK;
}

void UartDmaRx_GetStats(uint32_t *a, uint32_t *b, uint32_t *overruns)
{
  uint32_t state = __get_PRIMASK();
  __disable_irq();
  *a = uart_rx_debug.bank_full[0];
  *b = uart_rx_debug.bank_full[1];
  *overruns = uart_rx_debug.overruns;
  __set_PRIMASK(state);
}

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
    /* The TX complete interrupt clears tx_busy. Sleep until an interrupt;
       PRIMASK keeps one that arrives after the check pending for WFI. */
    uint32_t irq_state = __get_PRIMASK();
    __disable_irq();
    if (tx_busy && !tx_error)
    {
      __WFI();
      __NOP(); __NOP(); __NOP();  /* same errata workaround as wait_for_data() */
    }
    __set_PRIMASK(irq_state);
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

/* HAL reports errors in either direction here. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  if (uart == tx_uart) tx_error = 1;
  if (uart != rx_uart)
    return;
  rx_error = 1;
  uint32_t code = uart->ErrorCode;
  if (code & HAL_UART_ERROR_PE) ++uart_rx_debug.parity_errors;
  if (code & HAL_UART_ERROR_NE) ++uart_rx_debug.noise_errors;
  if (code & HAL_UART_ERROR_FE) ++uart_rx_debug.framing_errors;
  if (code & HAL_UART_ERROR_ORE) ++uart_rx_debug.overrun_errors;
}

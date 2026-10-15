#include "../Core/Src/uart_dma_tx.c"
#include <assert.h>
#include <stdio.h>

static DMA_HandleTypeDef dma;
static UART_HandleTypeDef uart = { .hdmatx = &dma, .gState = HAL_UART_STATE_READY };
static uint32_t tick, emitted;
static uint8_t *pending;
static uint16_t pending_size;
static uint8_t snapshot[4096], collected[65536], input[50000];
static int auto_complete = 1, fail_start;

static void complete(void)
{
  assert(pending && memcmp(pending, snapshot, pending_size) == 0);
  memcpy(collected + emitted, pending, pending_size);
  emitted += pending_size;
  pending = NULL;
  uart.gState = HAL_UART_STATE_READY;
  HAL_UART_TxCpltCallback(&uart);
}
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *handle, uint8_t *data, uint16_t size)
{
  if (fail_start) return HAL_BUSY;
  assert(!pending && handle->gState == HAL_UART_STATE_READY && size > 0 && size <= 4096);
  pending = data; pending_size = size;
  memcpy(snapshot, data, size);
  handle->gState = HAL_UART_STATE_BUSY_TX;
  return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *handle)
{
  pending = NULL;
  handle->gState = HAL_UART_STATE_READY;
  return HAL_OK;
}
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t milliseconds)
{
  tick += milliseconds;
  if (pending && auto_complete) complete();
}
int main(void)
{
  for (uint32_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i * 29U + 7U);
  const uint32_t sizes[] = {0, 1, 54, 1078, 3072, 4096, 4097, 8192, 50000};
  for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n)
  {
    emitted = tick = 0;
    assert(UartDmaTx_Begin(&uart) == HAL_OK);
    for (uint32_t offset = 0; offset < sizes[n]; offset += 777)
    {
      uint32_t count = sizes[n] - offset;
      if (count > 777) count = 777;
      assert(UartDmaTx_Write(input + offset, count) == HAL_OK);
    }
    assert(UartDmaTx_Flush() == HAL_OK);
    assert(emitted == sizes[n] && memcmp(collected, input, emitted) == 0);
  }
  assert(UartDmaTx_Begin(&uart) == HAL_OK);
  fail_start = 1;
  assert(UartDmaTx_Write(input, 4096) == HAL_BUSY);
  UartDmaTx_Abort(); fail_start = 0;
  assert(UartDmaTx_Begin(&uart) == HAL_OK);
  assert(UartDmaTx_Write(input, 4096) == HAL_OK);
  UartDmaTx_NotifyError(&uart);
  assert(UartDmaTx_Flush() == HAL_ERROR);
  UartDmaTx_Abort();
  assert(UartDmaTx_Begin(&uart) == HAL_OK);
  auto_complete = 0;
  assert(UartDmaTx_Write(input, 4096) == HAL_OK);
  assert(UartDmaTx_Flush() == HAL_TIMEOUT);
  UartDmaTx_Abort();
  puts("TX DMA tests: PASS (buffer ownership, partial flush, ordering, failure, timeout)");
  return 0;
}

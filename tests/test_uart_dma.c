/* Synthetic registers drive the actual production DBM reader. Including the
   implementation lets the hardware emulator write its private DMA buffers. */
#include "../Core/Src/uart_dma_rx.c"
#include <assert.h>
#include <stdio.h>
static DMA_Stream_TypeDef stream;
static DMA_HandleTypeDef dma = { .Init = { .Mode = DMA_CIRCULAR }, .Instance = &stream };
static USART_TypeDef usart;
static UART_HandleTypeDef uart = { .hdmarx = &dma, .Instance = &usart };
static uint32_t tick, wire_index, bank_size;
static uint8_t output[8192];
void UartDmaTx_NotifyError(UART_HandleTypeDef *handle) { (void)handle; }
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *handle)
{
  handle->hdmarx->tc_flag = 0;
  CLEAR_BIT(handle->Instance->CR3, USART_CR3_DMAR);
  return HAL_OK;
}
HAL_StatusTypeDef HAL_DMAEx_MultiBufferStart_IT(DMA_HandleTypeDef *handle,
                       uint32_t source, uint32_t first, uint32_t second, uint32_t size)
{
  assert(source == (uint32_t)(uintptr_t)&usart.RDR);
  assert(first == (uint32_t)(uintptr_t)buffer_a);
  assert(second == (uint32_t)(uintptr_t)buffer_b && first != second);
  assert(handle->XferCpltCallback && handle->XferM1CpltCallback);
  assert(!(handle->Instance->CR & DMA_SxCR_CT));
  handle->Instance->CR |= DMA_SxCR_DBM;
  handle->Instance->NDTR = bank_size = size;
  handle->tc_flag = 0;
  return HAL_OK;
}
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t milliseconds) { tick += milliseconds; }
static uint8_t value(uint32_t n) { return (uint8_t)(n * 73U + 19U); }
static void complete_irq(void)
{
  assert(dma.tc_flag);
  dma.tc_flag = 0;
  if (stream.CR & DMA_SxCR_CT) dma.XferCpltCallback(&dma);
  else dma.XferM1CpltCallback(&dma);
}
static void feed(uint32_t count, int dispatch)
{
  for (uint32_t i = 0; i < count; ++i)
  {
    uint8_t *bank = stream.CR & DMA_SxCR_CT ? buffer_b : buffer_a;
    bank[bank_size - stream.NDTR] = value(wire_index++);
    if (--stream.NDTR == 0)
    {
      stream.CR ^= DMA_SxCR_CT;
      stream.NDTR = bank_size;
      dma.tc_flag = 1;
      if (dispatch) complete_irq();
    }
  }
}
static void restart(void)
{
  tick = wire_index = 0;
  assert(UartDmaRx_Start(&uart) == HAL_OK);
  assert(stream.CR & DMA_SxCR_DBM);
  assert(usart.CR3 & USART_CR3_DMAR);
}
static void check_read(uint32_t count, uint32_t first)
{
  assert(UartDmaRx_Read(output, count, 10) == HAL_OK);
  for (uint32_t i = 0; i < count; ++i) assert(output[i] == value(first + i));
}
int main(void)
{
  restart();
  for (uint32_t block = 0; block < 40; ++block)
  {
    feed(1024, 1);
    check_read(1024, block * 1024);
  }
  uint32_t a, b, lost;
  UartDmaRx_GetStats(&a, &b, &lost);
  assert(a == 5 && b == 5 && lost == 0);
  restart();
  feed(3584, 1); check_read(3584, 0);
  feed(1024, 0); check_read(1024, 3584);
  complete_irq();
  feed(300, 1); check_read(300, 4608);
  for (int before_reload = 0; before_reload < 2; ++before_reload)
  {
    restart(); feed(4096, 0);
    if (before_reload) { stream.CR &= ~DMA_SxCR_CT; stream.NDTR = 0; }
    check_read(4096, 0);
    stream.CR |= DMA_SxCR_CT; stream.NDTR = bank_size;
    complete_irq();
    feed(1, 1); check_read(1, 4096);
  }
  /* Both banks passed without an IRQ: data must not be silently accepted. */
  restart(); feed(8192, 0);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  restart(); feed(8193, 1);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  restart(); feed(256, 1); check_read(256, 0);
  HAL_UART_ErrorCallback(&uart);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  restart(); dma.XferErrorCallback(&dma);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  assert(!(usart.CR3 & USART_CR3_DMAR));
  restart();
  assert(UartDmaRx_Read(output, 1, 5) == HAL_TIMEOUT);
  feed(256, 1); check_read(256, 0);
  puts("Double-buffer tests: PASS (A/B swaps, partial data, delayed IRQ, overwrite, recovery)");
  return 0;
}

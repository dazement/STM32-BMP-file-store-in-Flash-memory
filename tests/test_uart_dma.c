/* Synthetic registers drive the actual production DBM reader. Including the
   implementation lets the hardware emulator write its private DMA buffers. */
#include "../Core/Src/uart_dma_rx.c"
#include <assert.h>
#include <stdio.h>
static DMA_Stream_TypeDef stream;
static DMA_HandleTypeDef dma = { .Init = { .Mode = DMA_CIRCULAR }, .Instance = &stream };
static USART_TypeDef usart;
static UART_HandleTypeDef uart = { .hdmarx = &dma, .Instance = &usart };
static uint32_t tick, wire_index, bank_size, scripted_bytes, burst_per_wake;
static uint8_t output[8192];
void UartDmaTx_NotifyError(UART_HandleTypeDef *handle) { (void)handle; }
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *handle)
{
  handle->hdmarx->tc_flag = handle->hdmarx->ht_flag = 0;
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
  assert(handle->XferHalfCpltCallback && handle->XferM1HalfCpltCallback);
  assert(!(handle->Instance->CR & DMA_SxCR_CT));
  /* No IDLE interrupt may announce stale data while reception restarts. */
  assert(!(usart.CR1 & USART_CR1_IDLEIE));
  handle->Instance->CR |= DMA_SxCR_DBM;
  handle->Instance->NDTR = bank_size = size;
  handle->tc_flag = handle->ht_flag = 0;
  return HAL_OK;
}
uint32_t HAL_GetTick(void) { return tick; }
static uint8_t value(uint32_t n) { return (uint8_t)(n * 73U + 19U); }
/* DMA2 Stream2 IRQ in HAL order: half transfer, then transfer complete. */
static void dma_irq(void)
{
  if (dma.ht_flag)
  {
    dma.ht_flag = 0;
    if (stream.CR & DMA_SxCR_CT) dma.XferM1HalfCpltCallback(&dma);
    else dma.XferHalfCpltCallback(&dma);
  }
  if (dma.tc_flag)
  {
    dma.tc_flag = 0;
    if (stream.CR & DMA_SxCR_CT) dma.XferCpltCallback(&dma);
    else dma.XferM1CpltCallback(&dma);
  }
}
/* USART1 IRQ after the line goes idle. */
static void idle_irq(void)
{
  usart.ISR |= USART_ISR_IDLE;
  UartDmaRx_UartIrqHandler(&uart);
  assert(!(usart.ISR & USART_ISR_IDLE));
}
/* The DMA stores wire bytes; with dma_irqs its interrupts run on time. */
static void feed(uint32_t count, int dma_irqs)
{
  for (uint32_t i = 0; i < count; ++i)
  {
    uint8_t *bank = stream.CR & DMA_SxCR_CT ? buffer_b : buffer_a;
    bank[bank_size - stream.NDTR] = value(wire_index++);
    if (--stream.NDTR == bank_size / 2) dma.ht_flag = 1;
    if (stream.NDTR == 0)
    {
      stream.CR ^= DMA_SxCR_CT;
      stream.NDTR = bank_size;
      dma.tc_flag = 1;
    }
    if (dma_irqs) dma_irq();
  }
}
/* Normal reception: DMA interrupts on time, then the sender pauses. */
static void deliver(uint32_t count) { feed(count, 1); idle_irq(); }
/* WFI: one SysTick passes and the next scripted burst arrives with its IRQs. */
void __WFI(void)
{
  ++tick;
  if (scripted_bytes)
  {
    uint32_t count = scripted_bytes < burst_per_wake ? scripted_bytes : burst_per_wake;
    scripted_bytes -= count;
    deliver(count);
  }
}
static void restart(void)
{
  tick = wire_index = scripted_bytes = 0;
  usart.ISR = 0;
  assert(UartDmaRx_Start(&uart) == HAL_OK);
  assert(stream.CR & DMA_SxCR_DBM);
  assert(usart.CR3 & USART_CR3_DMAR);
  assert(usart.CR1 & USART_CR1_IDLEIE);
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
    deliver(1024);
    check_read(1024, block * 1024);
  }
  uint32_t a, b, lost;
  UartDmaRx_GetStats(&a, &b, &lost);
  assert(a == 5 && b == 5 && lost == 0);
  assert(uart_rx_debug.half_full == 10 && uart_rx_debug.line_idle == 40);
  assert(uart_rx_debug.received == 40960 && uart_rx_debug.sleeps == 0);
  /* Bytes already in RAM stay unread until an interrupt announces them:
     the reader sleeps in WFI and never polls the DMA counter. */
  restart(); feed(100, 0);
  assert(UartDmaRx_Read(output, 1, 5) == HAL_TIMEOUT);
  assert(uart_rx_debug.sleeps == 5 && tick == 5);
  idle_irq(); check_read(100, 0);
  /* A read waits in WFI and resumes as each burst's interrupts arrive. */
  restart(); burst_per_wake = 700; scripted_bytes = 4100;
  check_read(4100, 0);
  assert(uart_rx_debug.sleeps == 6 && tick == 6);
  /* USART1 outranks DMA2 Stream2 in the NVIC, so IDLE can run while the TC
     IRQ is still pending; CT still accounts for the finished bank. */
  restart(); deliver(3584); check_read(3584, 0);
  feed(1024, 0); idle_irq(); check_read(1024, 3584);
  dma_irq();
  deliver(300); check_read(300, 4608);
  for (int before_reload = 0; before_reload < 2; ++before_reload)
  {
    restart(); feed(4096, 0);
    if (before_reload) { stream.CR &= ~DMA_SxCR_CT; stream.NDTR = 0; }
    idle_irq(); check_read(4096, 0);
    stream.CR |= DMA_SxCR_CT; stream.NDTR = bank_size;
    assert(dma.tc_flag);
    dma_irq();
    deliver(1); check_read(1, 4096);
  }
  /* Both banks passed without an IRQ: data must not be silently accepted. */
  restart(); feed(8192, 0); idle_irq();
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  restart(); deliver(8193);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  /* DMA overwrote bank A after the last announcement: the hardware position
     check rejects the copy rather than return overwritten bytes. */
  restart(); deliver(4000); feed(4196, 1);
  assert(uart_rx_debug.received == 8192);
  assert(UartDmaRx_Read(output, 100, 10) == HAL_ERROR);
  restart(); deliver(256); check_read(256, 0);
  uart.ErrorCode = HAL_UART_ERROR_FE | HAL_UART_ERROR_NE;
  HAL_UART_ErrorCallback(&uart);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  assert(uart_rx_debug.framing_errors == 1 && uart_rx_debug.noise_errors == 1);
  assert(uart_rx_debug.parity_errors == 0 && uart_rx_debug.overrun_errors == 0);
  restart(); dma.XferErrorCallback(&dma);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  assert(!(usart.CR3 & USART_CR3_DMAR));
  assert(uart_rx_debug.framing_errors == 1);   /* kept across restarts */
  restart();
  assert(UartDmaRx_Read(output, 1, 5) == HAL_TIMEOUT);
  deliver(256); check_read(256, 0);
  puts("Double-buffer tests: PASS (IRQ-announced data, WFI wakeups, A/B swaps, "
       "delayed IRQ, overwrite, recovery)");
  return 0;
}

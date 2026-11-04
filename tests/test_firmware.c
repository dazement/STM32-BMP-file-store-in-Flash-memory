/* Firmware logic on the PC, in three groups: UART RX double buffer, UART TX
   double buffer, and the BMP Gray core. Synthetic registers drive the actual
   production DMA driver; including uart_dma.c lets the hardware emulator write
   its private DMA buffers. The BMP core links unchanged from bmp_gray.c.
   With two arguments (input.bmp output.bmp) it also converts a real BMP with
   the firmware's Gray code. */
#include "../Core/Src/uart_dma.c"
#include "bmp_gray.h"
#include <assert.h>
#include <stdio.h>

/* One USART1 with an RX DMA stream (hardware double buffer) and a TX DMA. */
static DMA_Stream_TypeDef stream;
static DMA_HandleTypeDef rx_dma = { .Init = { .Mode = DMA_CIRCULAR }, .Instance = &stream };
static DMA_HandleTypeDef tx_dma;
static USART_TypeDef usart;
static UART_HandleTypeDef uart = { .hdmarx = &rx_dma, .hdmatx = &tx_dma, .Instance = &usart,
                                   .gState = HAL_UART_STATE_READY };
static uint32_t tick;
uint32_t HAL_GetTick(void) { return tick; }

/* ---- UART RX double buffer ---- */
static uint32_t wire_index, bank_size, scripted_bytes, burst_per_wake;
static uint8_t output[8192];
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
static uint8_t value(uint32_t n) { return (uint8_t)(n * 73U + 19U); }
/* DMA2 Stream2 IRQ in HAL order: half transfer, then transfer complete. */
static void dma_irq(void)
{
  if (rx_dma.ht_flag)
  {
    rx_dma.ht_flag = 0;
    if (stream.CR & DMA_SxCR_CT) rx_dma.XferM1HalfCpltCallback(&rx_dma);
    else rx_dma.XferHalfCpltCallback(&rx_dma);
  }
  if (rx_dma.tc_flag)
  {
    rx_dma.tc_flag = 0;
    if (stream.CR & DMA_SxCR_CT) rx_dma.XferCpltCallback(&rx_dma);
    else rx_dma.XferM1CpltCallback(&rx_dma);
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
    if (--stream.NDTR == bank_size / 2) rx_dma.ht_flag = 1;
    if (stream.NDTR == 0)
    {
      stream.CR ^= DMA_SxCR_CT;
      stream.NDTR = bank_size;
      rx_dma.tc_flag = 1;
    }
    if (dma_irqs) dma_irq();
  }
}
/* Normal reception: DMA interrupts on time, then the sender pauses. */
static void deliver(uint32_t count) { feed(count, 1); idle_irq(); }
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
static void rx_tests(void)
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
    assert(rx_dma.tc_flag);
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
  restart(); rx_dma.XferErrorCallback(&rx_dma);
  assert(UartDmaRx_Read(output, 1, 10) == HAL_ERROR);
  assert(!(usart.CR3 & USART_CR3_DMAR));
  assert(uart_rx_debug.framing_errors == 1);   /* kept across restarts */
  restart();
  assert(UartDmaRx_Read(output, 1, 5) == HAL_TIMEOUT);
  deliver(256); check_read(256, 0);
}

/* ---- UART TX double buffer ---- */
static uint32_t emitted;
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
static void tx_tests(void)
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
  /* A UART error reported by HAL during transmission fails the flush. */
  HAL_UART_ErrorCallback(&uart);
  assert(UartDmaTx_Flush() == HAL_ERROR);
  UartDmaTx_Abort();
  assert(UartDmaTx_Begin(&uart) == HAL_OK);
  auto_complete = 0;
  assert(UartDmaTx_Write(input, 4096) == HAL_OK);
  assert(UartDmaTx_Flush() == HAL_TIMEOUT);
  UartDmaTx_Abort();
}

/* WFI: one SysTick passes. The next scripted RX burst arrives with its IRQs,
   and the TX complete IRQ fires if DMA finished. */
void __WFI(void)
{
  ++tick;
  if (scripted_bytes)
  {
    uint32_t count = scripted_bytes < burst_per_wake ? scripted_bytes : burst_per_wake;
    scripted_bytes -= count;
    deliver(count);
  }
  if (pending && auto_complete) complete();
}

/* ---- BMP Gray core ---- */
static void put32(uint8_t *p, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void bmp_tests(void)
{
  const uint8_t pixels[] = {0,0,0, 255,255,255, 0,0,255, 0,255,0, 255,0,0};
  const uint8_t expected[] = {0,255,77,149,29};
  uint8_t gray[5];
  BmpGray_Convert(pixels, 5, gray);
  assert(memcmp(gray, expected, 5) == 0);
  const uint8_t crc_input[] = "123456789";
  assert((BmpGray_CrcUpdate(0xFFFFFFFFU, crc_input, 9) ^ 0xFFFFFFFFU) == 0xCBF43926U);

  /* 3x2 24-bit source: 54-byte header, 12-byte rows, 78 bytes in all. */
  uint8_t header[54] = {'B', 'M'}, valid[54], gray_header[1078];
  put32(header + 2, 78); put32(header + 10, 54); put32(header + 14, 40);
  put32(header + 18, 3); put32(header + 22, 2);
  header[26] = 1; header[28] = 24;
  memcpy(valid, header, 54);
  BmpGrayInfo info;
  assert(BmpGray_Parse(header, 78, &info));
  assert(info.input_stride == 12 && info.output_stride == 4 && info.output_size == 1086);
  assert(BmpGray_MakeHeader(&info, gray_header) == 1078);
  assert(gray_header[10] == 0x36 && gray_header[11] == 0x04 && gray_header[28] == 8);
  assert(gray_header[2] == 0x3E && gray_header[3] == 0x04 && gray_header[34] == 8);
  assert(gray_header[54 + 255 * 4] == 255 && gray_header[57 + 255 * 4] == 0);
  put32(header + 22, (uint32_t)-2);
  assert(BmpGray_Parse(header, 78, &info) && info.rows == 2);
  put32(header + 22, 0x80000000U);
  assert(!BmpGray_Parse(header, 78, &info));
  memcpy(header, valid, 54); put32(header + 18, 0x7FFFFFFFU);
  assert(!BmpGray_Parse(header, 78, &info));
  memcpy(header, valid, 54); put32(header + 10, 0xFFFFFFFFU);
  assert(!BmpGray_Parse(header, 78, &info));
  memcpy(header, valid, 54); put32(header + 14, 0xFFFFFFFFU);
  assert(!BmpGray_Parse(header, 78, &info));
  memcpy(header, valid, 54); header[30] = 1;
  assert(!BmpGray_Parse(header, 78, &info));
  memcpy(header, valid, 54);
  assert(!BmpGray_Parse(header, 77, &info));
}
/* Converts a real BMP file the way the firmware does, in 1024-pixel pieces. */
static void convert_file(const char *input_path, const char *output_path)
{
  FILE *in = fopen(input_path, "rb"), *out = fopen(output_path, "wb");
  assert(in && out);
  assert(fseek(in, 0, SEEK_END) == 0);
  long size = ftell(in); rewind(in);
  uint8_t header[1078], rgb[3072], gray[1024], zeros[3] = {0};
  assert(fread(header, 1, 54, in) == 54);
  BmpGrayInfo info;
  assert(BmpGray_Parse(header, (uint32_t)size, &info));
  uint32_t h = BmpGray_MakeHeader(&info, header);
  assert(fwrite(header, 1, h, out) == h);
  for (uint32_t row = 0; row < info.rows; ++row)
  {
    for (uint32_t x = 0; x < info.width; x += 1024)
    {
      uint32_t n = info.width - x;
      if (n > 1024) n = 1024;
      assert(fseek(in, (long)(info.pixel_offset + row * info.input_stride + x * 3U), SEEK_SET) == 0);
      assert(fread(rgb, 1, n * 3U, in) == n * 3U);
      BmpGray_Convert(rgb, n, gray);
      assert(fwrite(gray, 1, n, out) == n);
    }
    uint32_t padding = info.output_stride - info.width;
    assert(fwrite(zeros, 1, padding, out) == padding);
  }
  fclose(in); assert(fclose(out) == 0);
}

int main(int argc, char **argv)
{
  rx_tests();
  puts("Double-buffer tests: PASS (IRQ-announced data, WFI wakeups, A/B swaps, "
       "delayed IRQ, overwrite, recovery)");
  tx_tests();
  puts("TX DMA tests: PASS (buffer ownership, partial flush, ordering, failure, timeout)");
  bmp_tests();
  if (argc == 3) convert_file(argv[1], argv[2]);
  puts("BMP core tests: PASS (colors, CRC, strides, orientation, malformed headers)");
  return 0;
}

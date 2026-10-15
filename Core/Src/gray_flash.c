#include "gray_flash.h"
#include "bmp_gray.h"
#include "flash_test.h"
#include "uart_dma_tx.h"
#include <stdio.h>
#include <string.h>

#define PIXELS_PER_CHUNK 1024U
static uint8_t input_rgb[PIXELS_PER_CHUNK * 3];
static uint8_t converted[PIXELS_PER_CHUNK * 3];
static uint8_t output_header[1078];

static void send_line(UART_HandleTypeDef *uart, const char *line)
{
  HAL_UART_Transmit(uart, (uint8_t *)line, (uint16_t)strlen(line), 1000);
}

static int emit(const uint8_t *data, uint32_t count, uint32_t *crc)
{
  if (UartDmaTx_Write(data, count) != HAL_OK) return 0;
  *crc = BmpGray_CrcUpdate(*crc, data, count);
  return 1;
}

void GrayFlash_Send(QSPI_HandleTypeDef *flash, UART_HandleTypeDef *uart,
                     uint32_t file_size, uint32_t expected_crc, uint8_t output_bpp)
{
  uint32_t start = HAL_GetTick();
  if (output_bpp != 8)
  {
    send_line(uart, "ERROR Gray output is fixed at 8 bits\n");
    return;
  }
  if (file_size < 54 || file_size > 0x3F0000U ||
      FlashTest_Read(flash, 0, input_rgb, 54) != HAL_OK)
  {
    send_line(uart, "ERROR gray source read or size\n");
    return;
  }
  BmpGrayInfo info;
  if (!BmpGray_Parse(input_rgb, file_size, output_bpp, &info))
  {
    send_line(uart, "ERROR unsupported or malformed stored BMP\n");
    return;
  }

  /* Verify the entire stored file, including its header, padding and trailing
     bytes. This also permits conversion after a reset with no image in RAM. */
  uint32_t source_crc = 0xFFFFFFFFU;
  for (uint32_t offset = 0; offset < file_size; offset += sizeof(input_rgb))
  {
    uint32_t count = file_size - offset;
    if (count > sizeof(input_rgb)) count = sizeof(input_rgb);
    if (FlashTest_Read(flash, offset, input_rgb, count) != HAL_OK)
    {
      send_line(uart, "ERROR stored BMP CRC read\n");
      return;
    }
    source_crc = BmpGray_CrcUpdate(source_crc, input_rgb, count);
  }
  source_crc ^= 0xFFFFFFFFU;
  if (source_crc != expected_crc)
  {
    send_line(uart, "ERROR stored BMP CRC mismatch\n");
    return;
  }
  if (UartDmaTx_Begin(uart) != HAL_OK)
  {
    send_line(uart, "ERROR gray TX DMA startup\n");
    return;
  }
  char line[96];
  snprintf(line, sizeof(line), "GRAY DATA %lu %u\n",
           (unsigned long)info.output_size, (unsigned)info.output_bpp);
  send_line(uart, line);
  uint32_t output_crc = 0xFFFFFFFFU;
  uint32_t header_size = BmpGray_MakeHeader(&info, output_header);
  if (!emit(output_header, header_size, &output_crc)) goto failure;

  for (uint32_t row = 0; row < info.rows; ++row)
  {
    for (uint32_t x = 0; x < info.width; x += PIXELS_PER_CHUNK)
    {
      uint32_t pixels = info.width - x;
      if (pixels > PIXELS_PER_CHUNK) pixels = PIXELS_PER_CHUNK;
      uint32_t address = info.pixel_offset + row * info.input_stride + x * 3U;
      /* Fresh SPI Flash read, not the prior UART receive buffer. */
      if (FlashTest_Read(flash, address, input_rgb, pixels * 3U) != HAL_OK) goto failure;
      BmpGray_Convert(input_rgb, pixels, converted, output_bpp);
      if (!emit(converted, pixels * (output_bpp / 8U), &output_crc)) goto failure;
    }
    uint32_t padding = info.output_stride - info.width * (output_bpp / 8U);
    static const uint8_t zeros[3] = {0};
    if (padding && !emit(zeros, padding, &output_crc)) goto failure;
  }
  if (UartDmaTx_Flush() != HAL_OK) goto failure;
  snprintf(line, sizeof(line), "GRAY DONE %08lX %08lX %lu\n",
           (unsigned long)(output_crc ^ 0xFFFFFFFFU), (unsigned long)source_crc,
           (unsigned long)(HAL_GetTick() - start));
  send_line(uart, line);
  return;

failure:
  /* No successful trailer is emitted for a partial or failed output. */
  UartDmaTx_Abort();
}

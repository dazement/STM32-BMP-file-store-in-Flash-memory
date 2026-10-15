#include "uart_flash_test.h"
#include "flash_test.h"
#include "uart_dma_rx.h"
#include "bmp_gray.h"
#include "gray_flash.h"
#include "uart_dma_tx.h"
#include <stdio.h>
#include <string.h>

#define MAX_PAYLOAD 1024U
#define SECTOR_BYTES 4096U
#define PAGE_BYTES 256U
#define PAYLOAD_OFFSET PAGE_BYTES
#define IMAGE_LIMIT 0x3F0000U
#define BLOCK_BYTES 65536U

static QSPI_HandleTypeDef *flash;
static UART_HandleTypeDef *serial;
static uint8_t chip_id[3];
static uint32_t sector_address;
static uint8_t received[MAX_PAYLOAD];
static uint8_t sector[SECTOR_BYTES];
static uint8_t magic_position;
static uint32_t last_magic_tick;
static const uint8_t record_magic[8] = {'U','F','T','R','E','C','0','1'};
static uint8_t file_buffer[4096];

static uint32_t get_u32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put_u32(uint8_t *data, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i)
    data[i] = (uint8_t)(value >> (8U * i));
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t count)
{
  return BmpGray_CrcUpdate(crc, data, count);
}

static uint32_t crc32(const uint8_t *data, uint32_t count)
{
  return crc32_update(0xFFFFFFFFU, data, count) ^ 0xFFFFFFFFU;
}

static void send_line(const char *line)
{
  HAL_UART_Transmit(serial, (uint8_t *)line, (uint16_t)strlen(line), 1000);
}

static int all_ff(const uint8_t *data, uint32_t count)
{
  for (uint32_t i = 0; i < count; ++i)
    if (data[i] != 0xFF)
      return 0;
  return 1;
}

static int sector_is_reusable(void)
{
  if (all_ff(sector, sizeof(sector)))
    return 1;

  /* Recognize exactly the pattern used by the preceding tutorial. */
  static const char old_signature[] = "BMP_GRAY_FLASH_TEST_V1";
  int previous_test = 1;
  for (uint32_t i = 0; i < PAGE_BYTES; ++i)
  {
    uint8_t expected = i < sizeof(old_signature) - 1 ?
                       (uint8_t)old_signature[i] : (uint8_t)(i ^ 0xA5U);
    if (sector[i] != expected)
      previous_test = 0;
  }
  if (previous_test && all_ff(sector + PAGE_BYTES, SECTOR_BYTES - PAGE_BYTES))
    return 1;

  /* Reuse only a completely verified record created by this program.
     A signature alone does not authorize erasing other contents. */
  if (memcmp(sector, record_magic, sizeof(record_magic)) != 0)
    return 0;
  uint32_t length = get_u32(sector + 8);
  if (length == 0 || length > MAX_PAYLOAD)
    return 0;
  return all_ff(sector + 16, PAGE_BYTES - 16) &&
         crc32(sector + PAYLOAD_OFFSET, length) == get_u32(sector + 12) &&
         all_ff(sector + PAYLOAD_OFFSET + length,
                SECTOR_BYTES - PAYLOAD_OFFSET - length);
}

static void handle_transfer(void)
{
  uint8_t header[8];
  if (UartDmaRx_Read(header, sizeof(header), 2000) != HAL_OK)
  {
    send_line("ERROR header timeout\n");
    return;
  }
  uint32_t length = get_u32(header);
  uint32_t input_crc = get_u32(header + 4);
  if (length == 0 || length > MAX_PAYLOAD)
  {
    send_line("ERROR length must be 1..1024\n");
    return;
  }
  if (sector_address == 0)
  {
    send_line("ERROR unsupported flash ID\n");
    return;
  }

  send_line("READY\n");
  if (UartDmaRx_Read(received, length, 5000) != HAL_OK)
  {
    send_line("ERROR payload timeout\n");
    return;
  }
  if (crc32(received, length) != input_crc)
  {
    send_line("ERROR input CRC mismatch; flash unchanged\n");
    return;
  }

  if (FlashTest_Read(flash, sector_address, sector, sizeof(sector)) != HAL_OK)
  {
    send_line("ERROR sector precheck read\n");
    return;
  }
  if (!sector_is_reusable())
  {
    send_line("ERROR existing sector data preserved\n");
    return;
  }

  uint32_t start = HAL_GetTick();
  if (FlashTest_Erase(flash, sector_address) != HAL_OK ||
      FlashTest_Read(flash, sector_address, sector, sizeof(sector)) != HAL_OK ||
      !all_ff(sector, sizeof(sector)))
  {
    send_line("ERROR erase or erase verification\n");
    return;
  }
  for (uint32_t offset = 0; offset < length; offset += PAGE_BYTES)
  {
    uint32_t count = length - offset;
    if (count > PAGE_BYTES)
      count = PAGE_BYTES;
    if (FlashTest_ProgramPage(flash, sector_address + PAYLOAD_OFFSET + offset,
                              received + offset, count) != HAL_OK)
    {
      send_line("ERROR page program\n");
      return;
    }
  }

  /* Commit ownership only after actual flash bytes match the UART input. */
  if (FlashTest_Read(flash, sector_address + PAYLOAD_OFFSET, sector, length) != HAL_OK ||
      memcmp(received, sector, length) != 0 || crc32(sector, length) != input_crc)
  {
    send_line("ERROR flash readback mismatch\n");
    return;
  }
  uint8_t record[16];
  memcpy(record, record_magic, sizeof(record_magic));
  put_u32(record + 8, length);
  put_u32(record + 12, input_crc);
  if (FlashTest_ProgramPage(flash, sector_address, record, sizeof(record)) != HAL_OK ||
      FlashTest_Read(flash, sector_address, sector, sizeof(sector)) != HAL_OK ||
      memcmp(sector, record, sizeof(record)) != 0 || !sector_is_reusable())
  {
    send_line("ERROR stored record verification\n");
    return;
  }

  char line[96];
  snprintf(line, sizeof(line), "STORED %lu %08lX %lu\n",
           (unsigned long)length, (unsigned long)input_crc,
           (unsigned long)(HAL_GetTick() - start));
  send_line(line);
  /* The outgoing bytes come from the flash read, not the UART input buffer. */
  HAL_UART_Transmit(serial, sector + PAYLOAD_OFFSET, (uint16_t)length, 2000);
}

static void stream_flash(void)
{
  uint8_t request[4];
  if (UartDmaRx_Read(request, sizeof(request), 2000) != HAL_OK)
  {
    send_line("ERROR read request timeout\n");
    return;
  }
  uint32_t length = get_u32(request);
  if (!sector_address || length == 0 || length > IMAGE_LIMIT)
  {
    send_line("ERROR invalid flash read range\n");
    return;
  }
  if (UartDmaTx_Begin(serial) != HAL_OK)
  {
    send_line("ERROR flash read TX DMA startup\n");
    return;
  }
  char line[80];
  snprintf(line, sizeof(line), "FILE DATA %lu\n", (unsigned long)length);
  send_line(line);
  uint32_t crc = 0xFFFFFFFFU;
  for (uint32_t offset = 0; offset < length; offset += sizeof(file_buffer))
  {
    uint32_t count = length - offset;
    if (count > sizeof(file_buffer))
      count = sizeof(file_buffer);
    if (FlashTest_Read(flash, offset, file_buffer, count) != HAL_OK ||
        UartDmaTx_Write(file_buffer, count) != HAL_OK)
    {
      UartDmaTx_Abort();
      return; /* No successful trailer on failure. */
    }
    crc = crc32_update(crc, file_buffer, count);
  }
  if (UartDmaTx_Flush() != HAL_OK)
  {
    UartDmaTx_Abort();
    return;
  }
  snprintf(line, sizeof(line), "FILE DATA DONE %08lX\n",
           (unsigned long)(crc ^ 0xFFFFFFFFU));
  send_line(line);
}

static void receive_file(void)
{
  uint8_t header[8];
  if (UartDmaRx_Read(header, sizeof(header), 2000) != HAL_OK)
  {
    send_line("ERROR file header timeout\n");
    return;
  }
  uint32_t length = get_u32(header);
  uint32_t expected_crc = get_u32(header + 4);
  if (!sector_address || !length || length > IMAGE_LIMIT)
  {
    send_line("ERROR invalid file size or flash ID\n");
    return;
  }
  send_line("FILE READY\n");
  uint32_t start = HAL_GetTick();
  uint32_t crc = 0xFFFFFFFFU;
  for (uint32_t offset = 0; offset < length; offset += sizeof(file_buffer))
  {
    uint32_t count = length - offset;
    if (count > sizeof(file_buffer))
      count = sizeof(file_buffer);
    uint8_t crc_header[4];
    if (UartDmaRx_Read(crc_header, 4, 5000) != HAL_OK ||
        UartDmaRx_Read(file_buffer, count, 5000) != HAL_OK)
    {
      send_line("ERROR file receive timeout or DMA overrun\n");
      return;
    }
    if (crc32(file_buffer, count) != get_u32(crc_header))
    {
      send_line("ERROR file chunk CRC\n");
      return;
    }
    if ((offset & (BLOCK_BYTES - 1U)) == 0 &&
        FlashTest_EraseBlock64(flash, offset) != HAL_OK)
    {
      send_line("ERROR file block erase\n");
      return;
    }
    for (uint32_t page = 0; page < count; page += PAGE_BYTES)
    {
      uint32_t n = count - page;
      if (n > PAGE_BYTES)
        n = PAGE_BYTES;
      if (FlashTest_ProgramPage(flash, offset + page, file_buffer + page, n) != HAL_OK)
      {
        send_line("ERROR file page program\n");
        return;
      }
    }
    if (FlashTest_Read(flash, offset, sector, count) != HAL_OK ||
        memcmp(file_buffer, sector, count) != 0)
    {
      send_line("ERROR file flash verification\n");
      return;
    }
    crc = crc32_update(crc, sector, count);
    char line[80];
    snprintf(line, sizeof(line), "ACK %lu\n", (unsigned long)(offset + count));
    send_line(line);
  }
  crc ^= 0xFFFFFFFFU;
  if (crc != expected_crc)
  {
    send_line("ERROR complete file CRC\n");
    return;
  }
  char line[96];
  snprintf(line, sizeof(line), "FILE STORED %lu %08lX %lu\n",
           (unsigned long)length, (unsigned long)crc,
           (unsigned long)(HAL_GetTick() - start));
  send_line(line);
}

static void change_baud(void)
{
  uint8_t request[4];
  if (UartDmaRx_Read(request, sizeof(request), 2000) != HAL_OK)
  {
    send_line("ERROR baud request timeout\n");
    return;
  }
  uint32_t baud = get_u32(request);
  if (baud != 115200 && baud != 921600 && baud != 2250000)
  {
    send_line("ERROR unsupported baud\n");
    return;
  }
  char line[32];
  snprintf(line, sizeof(line), "BAUD %lu\n", (unsigned long)baud);
  send_line(line);
  HAL_Delay(30);
  HAL_UART_AbortReceive(serial);
  serial->Init.BaudRate = baud;
  if (HAL_UART_Init(serial) == HAL_OK)
    UartDmaRx_Start(serial);
}

static void send_gray(void)
{
  uint8_t request[9];
  if (UartDmaRx_Read(request, sizeof(request), 2000) != HAL_OK)
  {
    send_line("ERROR gray request timeout\n");
    return;
  }
  if (!sector_address)
  {
    send_line("ERROR unsupported flash ID\n");
    return;
  }
  GrayFlash_Send(flash, serial, get_u32(request), get_u32(request + 4), request[8]);
}

void UartFlashTest_Init(QSPI_HandleTypeDef *qspi, UART_HandleTypeDef *uart,
                        const uint8_t jedec[3])
{
  flash = qspi;
  serial = uart;
  memcpy(chip_id, jedec, sizeof(chip_id));
  sector_address = 0;
  if (chip_id[0] == 0xEF && chip_id[1] == 0x40 && chip_id[2] == 0x18)
    sector_address = 0xFFF000U;
  else if (chip_id[0] == 0xEF && chip_id[1] == 0x40 && chip_id[2] == 0x16)
    sector_address = 0x3FF000U;
  magic_position = 0;
  if (UartDmaRx_Start(serial) != HAL_OK)
    send_line("ERROR UART DMA startup\r\n");
  else
    send_line("UART double-buffer Flash test ready. Run PC test.\r\n");
}

void UartFlashTest_Poll(void)
{
  uint8_t byte;
  HAL_StatusTypeDef status = UartDmaRx_Read(&byte, 1, 10);
  if (status != HAL_OK)
  {
    if (status != HAL_TIMEOUT)
    {
      UartDmaRx_Start(serial);
      magic_position = 0;
    }
    return;
  }
  if (magic_position && (uint32_t)(HAL_GetTick() - last_magic_tick) > 1000U)
    magic_position = 0;
  if (byte == '?')
  {
    char line[64];
    snprintf(line, sizeof(line), "UFT READY %02X%02X%02X DBM GRAY GRAY8\n",
             (unsigned)chip_id[0], (unsigned)chip_id[1], (unsigned)chip_id[2]);
    send_line(line);
    magic_position = 0;
    return;
  }
  if (byte == '!')
  {
    uint32_t a, b, overruns;
    UartDmaRx_GetStats(&a, &b, &overruns);
    char line[80];
    snprintf(line, sizeof(line), "DBM STATS %lu %lu %lu\n",
             (unsigned long)a, (unsigned long)b, (unsigned long)overruns);
    send_line(line);
    magic_position = 0;
    return;
  }
  static uint8_t operation;
  if (magic_position == 0)
    magic_position = byte == 'U' ? 1 : 0;
  else if (magic_position == 1)
    magic_position = byte == 'F' ? 2 : (byte == 'U' ? 1 : 0);
  else if (magic_position == 2)
  {
    operation = byte;
    magic_position = (byte == 'T' || byte == 'L' || byte == 'R' ||
                      byte == 'S' || byte == 'G') ? 3 : 0;
  }
  else
    magic_position = byte == '1' ? 4 : (byte == 'U' ? 1 : 0);
  last_magic_tick = HAL_GetTick();
  if (magic_position == 4)
  {
    magic_position = 0;
    if (operation == 'T') handle_transfer();
    else if (operation == 'R') stream_flash();
    else if (operation == 'S') change_baud();
    else if (operation == 'G') send_gray();
    else receive_file();
  }
}

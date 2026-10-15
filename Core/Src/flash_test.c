#include "flash_test.h"
#include <stdio.h>
#include <string.h>

#define PAGE_BYTES   256U
#define SECTOR_BYTES 4096U
#define IO_TIMEOUT   100U

static uint8_t test_pattern[PAGE_BYTES];
static uint8_t read_buffer[PAGE_BYTES];

static void log_line(UART_HandleTypeDef *uart, const char *line)
{
  HAL_UART_Transmit(uart, (uint8_t *)line, (uint16_t)strlen(line), IO_TIMEOUT);
}

static QSPI_CommandTypeDef make_command(uint8_t instruction, uint32_t address,
                                        uint32_t count, int has_address)
{
  QSPI_CommandTypeDef cmd = {0};
  cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
  cmd.Instruction = instruction;
  cmd.AddressMode = has_address ? QSPI_ADDRESS_1_LINE : QSPI_ADDRESS_NONE;
  cmd.AddressSize = QSPI_ADDRESS_24_BITS;
  cmd.Address = address;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd.DataMode = count ? QSPI_DATA_1_LINE : QSPI_DATA_NONE;
  cmd.NbData = count;
  cmd.DummyCycles = 0;
  cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
  cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
  return cmd;
}

static HAL_StatusTypeDef read_data(QSPI_HandleTypeDef *qspi, uint8_t instruction,
                                   uint32_t address, uint8_t *data,
                                   uint32_t count, int has_address)
{
  QSPI_CommandTypeDef cmd = make_command(instruction, address, count, has_address);
  HAL_StatusTypeDef status = HAL_QSPI_Command(qspi, &cmd, IO_TIMEOUT);
  if (status == HAL_OK)
    status = HAL_QSPI_Receive(qspi, data, IO_TIMEOUT);
  if (status != HAL_OK)
    HAL_QSPI_Abort(qspi);
  return status;
}

static HAL_StatusTypeDef send_command(QSPI_HandleTypeDef *qspi,
                                      uint8_t instruction, uint32_t address,
                                      int has_address)
{
  QSPI_CommandTypeDef cmd = make_command(instruction, address, 0, has_address);
  HAL_StatusTypeDef status = HAL_QSPI_Command(qspi, &cmd, IO_TIMEOUT);
  if (status != HAL_OK)
    HAL_QSPI_Abort(qspi);
  return status;
}

static HAL_StatusTypeDef wait_ready(QSPI_HandleTypeDef *qspi, uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  for (;;)
  {
    uint8_t sr1 = 0;
    HAL_StatusTypeDef status = read_data(qspi, 0x05, 0, &sr1, 1, 0);
    if (status != HAL_OK)
      return status;
    if ((sr1 & 0x01U) == 0)
      return HAL_OK;
    if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
      return HAL_TIMEOUT;
    HAL_Delay(1);
  }
}

static HAL_StatusTypeDef write_enable(QSPI_HandleTypeDef *qspi)
{
  HAL_StatusTypeDef status = send_command(qspi, 0x06, 0, 0);
  if (status != HAL_OK)
    return status;
  uint8_t sr1 = 0;
  status = read_data(qspi, 0x05, 0, &sr1, 1, 0);
  if (status != HAL_OK)
    return status;
  return ((sr1 & 0x03U) == 0x02U) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef erase_sector(QSPI_HandleTypeDef *qspi, uint32_t address)
{
  HAL_StatusTypeDef status = write_enable(qspi);
  if (status == HAL_OK)
    status = send_command(qspi, 0x20, address, 1);
  if (status == HAL_OK)
    status = wait_ready(qspi, 2000);
  return status;
}

HAL_StatusTypeDef FlashTest_ProgramPage(QSPI_HandleTypeDef *qspi, uint32_t address,
                                       uint8_t *data, uint32_t count)
{
  if (count == 0 || count > PAGE_BYTES || address >= 0x1000000U ||
      count > 0x1000000U - address ||
      (address & (PAGE_BYTES - 1U)) + count > PAGE_BYTES)
    return HAL_ERROR;
  HAL_StatusTypeDef status = write_enable(qspi);
  if (status != HAL_OK)
    return status;
  QSPI_CommandTypeDef cmd = make_command(0x02, address, count, 1);
  status = HAL_QSPI_Command(qspi, &cmd, IO_TIMEOUT);
  if (status == HAL_OK)
    status = HAL_QSPI_Transmit(qspi, data, IO_TIMEOUT);
  if (status != HAL_OK)
  {
    HAL_QSPI_Abort(qspi);
    return status;
  }
  return wait_ready(qspi, 100);
}

HAL_StatusTypeDef FlashTest_Read(QSPI_HandleTypeDef *qspi, uint32_t address,
                                 uint8_t *data, uint32_t count)
{
  if (count == 0 || address >= 0x1000000U || count > 0x1000000U - address)
    return HAL_ERROR;
  HAL_StatusTypeDef status = wait_ready(qspi, 2000);
  if (status != HAL_OK)
    return status;
  return read_data(qspi, 0x03, address, data, count, 1);
}

HAL_StatusTypeDef FlashTest_Erase(QSPI_HandleTypeDef *qspi, uint32_t address)
{
  if (address >= 0x1000000U || (address & (SECTOR_BYTES - 1U)) != 0)
    return HAL_ERROR;
  HAL_StatusTypeDef status = wait_ready(qspi, 2000);
  return status == HAL_OK ? erase_sector(qspi, address) : status;
}

const char *FlashTest_Run(QSPI_HandleTypeDef *qspi, UART_HandleTypeDef *uart,
                          const uint8_t jedec[3])
{
  /* Only these verified Winbond command sets/capacities are allowed to write.
     The last physical sector is used, not the image or metadata region. */
  uint32_t capacity;
  if (jedec[0] == 0xEF && jedec[1] == 0x40 && jedec[2] == 0x18)
    capacity = 16U * 1024U * 1024U;
  else if (jedec[0] == 0xEF && jedec[1] == 0x40 && jedec[2] == 0x16)
    capacity = 4U * 1024U * 1024U;
  else
    return "Flash test: SKIPPED - unsupported chip ID\r\n";

  const uint32_t address = capacity - SECTOR_BYTES;
  char line[80];
  snprintf(line, sizeof(line), "Flash test sector: 0x%06lX (4096 bytes)\r\n",
           (unsigned long)address);
  log_line(uart, line);

  if (wait_ready(qspi, 2000) != HAL_OK)
    return "Flash test: FAILED - flash not ready\r\n";

  for (uint32_t i = 0; i < PAGE_BYTES; ++i)
    test_pattern[i] = (uint8_t)(i ^ 0xA5U);
  static const char signature[] = "BMP_GRAY_FLASH_TEST_V1";
  memcpy(test_pattern, signature, sizeof(signature) - 1);

  /* Inspect every byte before any destructive instruction. Allow only blank
     contents or the exact pattern left by a previous successful run. */
  int erased = 1;
  int previous_test = 1;
  for (uint32_t offset = 0; offset < SECTOR_BYTES; offset += PAGE_BYTES)
  {
    if (read_data(qspi, 0x03, address + offset, read_buffer, PAGE_BYTES, 1) != HAL_OK)
      return "Flash test: FAILED - precheck read error\r\n";
    for (uint32_t i = 0; i < PAGE_BYTES; ++i)
    {
      if (read_buffer[i] != 0xFF)
        erased = 0;
      uint8_t expected = offset == 0 ? test_pattern[i] : 0xFF;
      if (read_buffer[i] != expected)
        previous_test = 0;
    }
  }
  if (!erased && !previous_test)
    return "Flash test: SKIPPED - existing sector data preserved\r\n";
  if (previous_test)
    log_line(uart, "Previous test pattern: PASS (stored data still present)\r\n");

  uint32_t start = HAL_GetTick();
  if (erase_sector(qspi, address) != HAL_OK)
    return "Flash test: FAILED - erase command or timeout\r\n";
  for (uint32_t offset = 0; offset < SECTOR_BYTES; offset += PAGE_BYTES)
  {
    if (read_data(qspi, 0x03, address + offset, read_buffer, PAGE_BYTES, 1) != HAL_OK)
      return "Flash test: FAILED - erase verification read error\r\n";
    for (uint32_t i = 0; i < PAGE_BYTES; ++i)
      if (read_buffer[i] != 0xFF)
        return "Flash test: FAILED - erase verification mismatch\r\n";
  }
  snprintf(line, sizeof(line), "Sector erase: PASS (%lu ms, including verification)\r\n",
           (unsigned long)(HAL_GetTick() - start));
  log_line(uart, line);

  start = HAL_GetTick();
  if (FlashTest_ProgramPage(qspi, address, test_pattern, PAGE_BYTES) != HAL_OK)
    return "Flash test: FAILED - page program command or timeout\r\n";
  snprintf(line, sizeof(line), "Page program: completed (%lu ms)\r\n",
           (unsigned long)(HAL_GetTick() - start));
  log_line(uart, line);
  if (read_data(qspi, 0x03, address, read_buffer, PAGE_BYTES, 1) != HAL_OK)
    return "Flash test: FAILED - page read error\r\n";
  if (memcmp(test_pattern, read_buffer, PAGE_BYTES) != 0)
    return "Flash test: FAILED - write/read mismatch\r\n";

  /* Confirm programming did not alter the remainder of the sector. */
  for (uint32_t offset = PAGE_BYTES; offset < SECTOR_BYTES; offset += PAGE_BYTES)
  {
    if (read_data(qspi, 0x03, address + offset, read_buffer, PAGE_BYTES, 1) != HAL_OK)
      return "Flash test: FAILED - remainder read error\r\n";
    for (uint32_t i = 0; i < PAGE_BYTES; ++i)
      if (read_buffer[i] != 0xFF)
        return "Flash test: FAILED - unexpected change outside test page\r\n";
  }
  return "Flash test: PASS (erase/write/read, 256 bytes match)\r\n";
}

HAL_StatusTypeDef FlashTest_EraseBlock64(QSPI_HandleTypeDef *qspi, uint32_t address)
{
  if (address >= 0x1000000U || (address & 0xFFFFU) != 0)
    return HAL_ERROR;
  HAL_StatusTypeDef status = wait_ready(qspi, 5000);
  if (status == HAL_OK)
    status = write_enable(qspi);
  if (status == HAL_OK)
    status = send_command(qspi, 0xD8, address, 1);
  if (status == HAL_OK)
    status = wait_ready(qspi, 5000);
  return status;
}

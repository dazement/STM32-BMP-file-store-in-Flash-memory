#include "qspi_flash.h"

#define PAGE_BYTES   256U
#define SECTOR_BYTES 4096U
#define IO_TIMEOUT   100U

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

/* Read-only probe: no erase, program, or flash status-register writes. */
HAL_StatusTypeDef QspiFlash_ReadId(QSPI_HandleTypeDef *qspi, uint8_t id[3])
{
  return read_data(qspi, 0x9F, 0, id, 3, 0);
}

HAL_StatusTypeDef QspiFlash_ProgramPage(QSPI_HandleTypeDef *qspi, uint32_t address,
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

HAL_StatusTypeDef QspiFlash_Read(QSPI_HandleTypeDef *qspi, uint32_t address,
                                 uint8_t *data, uint32_t count)
{
  if (count == 0 || address >= 0x1000000U || count > 0x1000000U - address)
    return HAL_ERROR;
  HAL_StatusTypeDef status = wait_ready(qspi, 2000);
  if (status != HAL_OK)
    return status;
  return read_data(qspi, 0x03, address, data, count, 1);
}

HAL_StatusTypeDef QspiFlash_EraseSector(QSPI_HandleTypeDef *qspi, uint32_t address)
{
  if (address >= 0x1000000U || (address & (SECTOR_BYTES - 1U)) != 0)
    return HAL_ERROR;
  HAL_StatusTypeDef status = wait_ready(qspi, 2000);
  return status == HAL_OK ? erase_sector(qspi, address) : status;
}

HAL_StatusTypeDef QspiFlash_EraseBlock64(QSPI_HandleTypeDef *qspi, uint32_t address)
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

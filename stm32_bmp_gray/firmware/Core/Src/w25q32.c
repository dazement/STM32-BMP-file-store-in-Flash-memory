/**
 * w25q32.c - Winbond W25Q32 SPI NOR Flash 드라이버 (STM32 HAL, blocking)
 */
#include "w25q32.h"
#include "app_config.h"

#define CMD_WRITE_ENABLE    0x06u
#define CMD_READ_SR1        0x05u
#define CMD_WRITE_SR1       0x01u
#define CMD_READ_DATA       0x03u
#define CMD_PAGE_PROGRAM    0x02u
#define CMD_SECTOR_ERASE    0x20u
#define CMD_BLOCK64_ERASE   0xD8u
#define CMD_CHIP_ERASE      0xC7u
#define CMD_JEDEC_ID        0x9Fu
#define CMD_ENABLE_RESET    0x66u
#define CMD_RESET           0x99u

#define SR1_BUSY            0x01u
#define SR1_WEL             0x02u
#define SR1_PROTECT_MASK    0x7Cu   /* BP0..BP2, TB, SEC */

/* 데이터시트 최대값 + 여유 (ms) */
#define TMO_PAGE_MS         10u
#define TMO_SECTOR_MS       600u
#define TMO_BLOCK64_MS      3000u
#define TMO_CHIP_MS         120000u
#define TMO_SPI_MS          1000u

#define CS_LOW()   HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET)

static w25q_status_t spi_tx(const uint8_t *buf, uint16_t len)
{
    return HAL_SPI_Transmit(APP_FLASH_SPI, (uint8_t *)buf, len, TMO_SPI_MS) == HAL_OK
               ? W25Q_OK : W25Q_ERR_SPI;
}

static w25q_status_t spi_rx(uint8_t *buf, uint16_t len)
{
    return HAL_SPI_Receive(APP_FLASH_SPI, buf, len, TMO_SPI_MS) == HAL_OK
               ? W25Q_OK : W25Q_ERR_SPI;
}

static w25q_status_t cmd_only(uint8_t cmd)
{
    CS_LOW();
    w25q_status_t st = spi_tx(&cmd, 1);
    CS_HIGH();
    return st;
}

static w25q_status_t cmd_addr(uint8_t cmd, uint32_t addr)
{
    uint8_t h[4] = { cmd, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    return spi_tx(h, sizeof h);
}

static uint8_t read_sr1(void)
{
    uint8_t cmd = CMD_READ_SR1, sr = 0xFF;
    CS_LOW();
    if (spi_tx(&cmd, 1) == W25Q_OK) (void)spi_rx(&sr, 1);
    CS_HIGH();
    return sr;
}

static w25q_status_t wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (read_sr1() & SR1_BUSY) {
        if (HAL_GetTick() - t0 > timeout_ms) return W25Q_ERR_TIMEOUT;
    }
    return W25Q_OK;
}

static w25q_status_t write_enable(void)
{
    w25q_status_t st = cmd_only(CMD_WRITE_ENABLE);
    if (st != W25Q_OK) return st;
    return (read_sr1() & SR1_WEL) ? W25Q_OK : W25Q_ERR_WEL;
}

uint32_t W25Q_ReadJedecId(void)
{
    uint8_t cmd = CMD_JEDEC_ID, id[3] = { 0 };
    CS_LOW();
    if (spi_tx(&cmd, 1) == W25Q_OK) (void)spi_rx(id, 3);
    CS_HIGH();
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

w25q_status_t W25Q_Init(void)
{
    CS_HIGH();
    HAL_Delay(5);
    (void)cmd_only(CMD_ENABLE_RESET);
    (void)cmd_only(CMD_RESET);
    HAL_Delay(1);                        /* tRST = 30 us */

    uint32_t id = W25Q_ReadJedecId();
    if ((id >> 16) != 0xEFu || (id & 0xFFu) != 0x16u) return W25Q_ERR_ID; /* Winbond, 32 Mbit */

    if (read_sr1() & SR1_PROTECT_MASK) { /* 블록 보호 해제 */
        w25q_status_t st = write_enable();
        if (st != W25Q_OK) return st;
        uint8_t w[2] = { CMD_WRITE_SR1, 0x00 };
        CS_LOW(); st = spi_tx(w, 2); CS_HIGH();
        if (st != W25Q_OK) return st;
        st = wait_ready(TMO_SECTOR_MS);
        if (st != W25Q_OK) return st;
    }
    return W25Q_OK;
}

w25q_status_t W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (addr + len > W25Q32_FLASH_SIZE) return W25Q_ERR_PARAM;
    if (len == 0) return W25Q_OK;

    CS_LOW();
    w25q_status_t st = cmd_addr(CMD_READ_DATA, addr);
    while (st == W25Q_OK && len) {        /* HAL Size 는 uint16_t */
        uint16_t n = (len > 32768u) ? 32768u : (uint16_t)len;
        st = spi_rx(buf, n);
        buf += n; len -= n;
    }
    CS_HIGH();
    return st;
}

w25q_status_t W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (addr + len > W25Q32_FLASH_SIZE) return W25Q_ERR_PARAM;

    while (len) {
        /* 페이지 경계를 넘으면 칩 내부에서 주소가 wrap 되므로 반드시 분할 */
        uint32_t room = W25Q32_PAGE_SIZE - (addr % W25Q32_PAGE_SIZE);
        uint16_t n = (uint16_t)(len < room ? len : room);

        w25q_status_t st = write_enable();
        if (st != W25Q_OK) return st;
        CS_LOW();
        st = cmd_addr(CMD_PAGE_PROGRAM, addr);
        if (st == W25Q_OK) st = spi_tx(buf, n);
        CS_HIGH();
        if (st != W25Q_OK) return st;
        st = wait_ready(TMO_PAGE_MS);
        if (st != W25Q_OK) return st;

        addr += n; buf += n; len -= n;
    }
    return W25Q_OK;
}

static w25q_status_t erase_cmd(uint8_t cmd, uint32_t addr, uint32_t timeout_ms)
{
    w25q_status_t st = write_enable();
    if (st != W25Q_OK) return st;
    CS_LOW();
    st = (cmd == CMD_CHIP_ERASE) ? spi_tx(&cmd, 1) : cmd_addr(cmd, addr);
    CS_HIGH();
    if (st != W25Q_OK) return st;
    return wait_ready(timeout_ms);
}

w25q_status_t W25Q_EraseSector(uint32_t addr)
{
    if (addr >= W25Q32_FLASH_SIZE) return W25Q_ERR_PARAM;
    return erase_cmd(CMD_SECTOR_ERASE, addr & ~(W25Q32_SECTOR_SIZE - 1u), TMO_SECTOR_MS);
}

w25q_status_t W25Q_EraseBlock64(uint32_t addr)
{
    if (addr >= W25Q32_FLASH_SIZE) return W25Q_ERR_PARAM;
    return erase_cmd(CMD_BLOCK64_ERASE, addr & ~(W25Q32_BLOCK_SIZE - 1u), TMO_BLOCK64_MS);
}

w25q_status_t W25Q_EraseChip(void)
{
    return erase_cmd(CMD_CHIP_ERASE, 0, TMO_CHIP_MS);
}

w25q_status_t W25Q_EraseRange(uint32_t addr, uint32_t len)
{
    if (addr + len > W25Q32_FLASH_SIZE) return W25Q_ERR_PARAM;

    uint32_t a   = addr & ~(W25Q32_SECTOR_SIZE - 1u);
    uint32_t end = addr + len;
    while (a < end) {
        w25q_status_t st;
        if ((a % W25Q32_BLOCK_SIZE) == 0 && end - a >= W25Q32_BLOCK_SIZE) {
            st = W25Q_EraseBlock64(a);
            a += W25Q32_BLOCK_SIZE;
        } else {
            st = W25Q_EraseSector(a);
            a += W25Q32_SECTOR_SIZE;
        }
        if (st != W25Q_OK) return st;
    }
    return W25Q_OK;
}

/**
 * w25q32.h - Winbond W25Q32 (32 Mbit / 4 MB) SPI NOR Flash 드라이버 (STM32 HAL)
 */
#ifndef W25Q32_H
#define W25Q32_H

#include <stdint.h>

#define W25Q32_FLASH_SIZE   (4u * 1024u * 1024u)
#define W25Q32_PAGE_SIZE    256u
#define W25Q32_SECTOR_SIZE  4096u
#define W25Q32_BLOCK_SIZE   65536u

typedef enum {
    W25Q_OK = 0,
    W25Q_ERR_SPI,
    W25Q_ERR_TIMEOUT,
    W25Q_ERR_ID,
    W25Q_ERR_WEL,       /* Write Enable 실패 (WP 핀/보호 비트/배선 확인) */
    W25Q_ERR_PARAM
} w25q_status_t;

w25q_status_t W25Q_Init(void);                 /* SW reset, JEDEC ID 확인, 보호 비트 해제 */
uint32_t      W25Q_ReadJedecId(void);          /* W25Q32 = 0xEF4016 (또는 0xEF7016) */
w25q_status_t W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);
w25q_status_t W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len); /* 지워진 영역에 페이지 단위로 분할 기록 */
w25q_status_t W25Q_EraseSector(uint32_t addr); /* 4 KB  */
w25q_status_t W25Q_EraseBlock64(uint32_t addr);/* 64 KB */
w25q_status_t W25Q_EraseRange(uint32_t addr, uint32_t len); /* 필요한 만큼 64K/4K 조합으로 지움 */
w25q_status_t W25Q_EraseChip(void);

#endif /* W25Q32_H */

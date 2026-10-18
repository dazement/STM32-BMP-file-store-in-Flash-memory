/*
 * sim_hal.c - 가짜 HAL: W25Q32 명령 레벨 에뮬레이터(SPI) + pty 기반 UART
 *
 * 실제 칩처럼 동작:
 *   - WREN 없이 Program/Erase 하면 무시됨
 *   - Program 은 비트를 1->0 으로만 바꿈 (지우지 않고 쓰면 데이터 깨짐)
 *   - Page Program 이 페이지 경계를 넘으면 같은 페이지 안에서 wrap
 *   - Program/Erase 후 몇 번의 SR1 읽기 동안 BUSY
 * 옵션으로 UART 바이트에 비트 오류를 주입해 재전송 로직을 시험한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "main.h"
#include "w25q32.h"

GPIO_TypeDef sim_gpioa;
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;

double sim_rx_err_rate, sim_tx_err_rate;   /* 바이트당 비트오류 확률 */
unsigned long sim_rx_errs, sim_tx_errs;
unsigned long sim_prog_violations;         /* 지워지지 않은 곳에 0->1 쓰기 시도 */

/* ------------------------------------------------------------------ time */
uint32_t HAL_GetTick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
void HAL_Delay(uint32_t ms) { usleep(ms * 1000u); }

/* ------------------------------------------------------------- flash emu */
uint8_t  sim_flash[W25Q32_FLASH_SIZE];
static int      cs_low;
static uint8_t  cmd[8];
static uint32_t cmd_n;          /* 지금까지 받은 명령/주소 바이트 수 */
static uint32_t addr;
static int      wel, busy_cnt;
static uint32_t pp_count;       /* 현재 Page Program 으로 받은 데이터 수 */

static uint32_t a24(void) { return ((uint32_t)cmd[1] << 16) | ((uint32_t)cmd[2] << 8) | cmd[3]; }

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st)
{
    (void)port; (void)pin;
    if (st == GPIO_PIN_RESET) {          /* CS 하강: 새 명령 시작 */
        cs_low = 1; cmd_n = 0; pp_count = 0;
        return;
    }
    if (!cs_low) return;
    cs_low = 0;
    if (cmd_n == 0) return;
    switch (cmd[0]) {                    /* CS 상승 시 실행되는 명령 */
    case 0x06: if (!busy_cnt) wel = 1; break;
    case 0x01: if (wel) { busy_cnt = 3; wel = 0; } break;
    case 0x02: if (wel) { busy_cnt = 3; wel = 0; } break;
    case 0x20: case 0xD8:
        if (wel && cmd_n >= 4) {
            uint32_t sz = cmd[0] == 0x20 ? W25Q32_SECTOR_SIZE : W25Q32_BLOCK_SIZE;
            uint32_t a = a24() & ~(sz - 1) & (W25Q32_FLASH_SIZE - 1);
            memset(&sim_flash[a], 0xFF, sz);
            busy_cnt = cmd[0] == 0x20 ? 5 : 20;
        }
        wel = 0; break;
    case 0xC7: case 0x60:
        if (wel) { memset(sim_flash, 0xFF, sizeof sim_flash); busy_cnt = 50; }
        wel = 0; break;
    default: break;
    }
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo)
{
    (void)h; (void)tmo;
    if (!cs_low) { fprintf(stderr, "[sim] SPI TX with CS high!\n"); return HAL_ERROR; }
    for (uint16_t i = 0; i < n; i++) {
        if (cmd_n < 4) {
            if (cmd_n == 0 && busy_cnt && d[i] != 0x05 && d[i] != 0x9F)
                fprintf(stderr, "[sim] cmd 0x%02X while BUSY (ignored by chip)\n", d[i]);
            cmd[cmd_n++] = d[i];
            if (cmd_n == 4) addr = a24();
            continue;
        }
        if (cmd[0] == 0x02 && wel) {     /* Page Program: 페이지 내 wrap */
            uint32_t a = (addr & ~0xFFu) | ((addr + pp_count) & 0xFFu);
            if (pp_count >= 256) fprintf(stderr, "[sim] page program > 256 bytes\n");
            if ((sim_flash[a] & d[i]) != d[i]) sim_prog_violations++;
            sim_flash[a] &= d[i];
            pp_count++;
        }
        cmd_n++;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo)
{
    (void)h; (void)tmo;
    if (!cs_low || cmd_n == 0) return HAL_ERROR;
    for (uint16_t i = 0; i < n; i++) {
        switch (cmd[0]) {
        case 0x05:
            d[i] = (uint8_t)((busy_cnt ? 1 : 0) | (wel ? 2 : 0));
            if (busy_cnt) busy_cnt--;
            break;
        case 0x9F: { static const uint8_t id[3] = { 0xEF, 0x40, 0x16 }; d[i] = id[(cmd_n - 1) % 3]; cmd_n++; break; }
        case 0x03:
            if (busy_cnt) { d[i] = 0xEE; break; }  /* busy 중 read 는 무효 */
            d[i] = sim_flash[addr++ & (W25Q32_FLASH_SIZE - 1)];
            break;
        default: d[i] = 0xFF; break;
        }
    }
    return HAL_OK;
}

/* ------------------------------------------------------------------ UART */
static uint8_t maybe_corrupt(uint8_t b, double rate, unsigned long *cnt)
{
    if (rate > 0 && (double)rand() / RAND_MAX < rate) { (*cnt)++; return b ^ (uint8_t)(1u << (rand() & 7)); }
    return b;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *d, uint16_t n, uint32_t tmo)
{
    (void)tmo;
    uint8_t buf[8192];
    for (uint16_t i = 0; i < n; i++) buf[i] = maybe_corrupt(d[i], sim_tx_err_rate, &sim_tx_errs);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(h->fd, buf + off, n - off);
        if (w < 0) { if (errno == EAGAIN || errno == EINTR) { usleep(100); continue; } return HAL_ERROR; }
        off += (size_t)w;
    }
    return HAL_OK;
}

/* HAL 과 동일하게 timeout 은 호출 전체에 대한 시간 */
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *d, uint16_t n, uint32_t tmo)
{
    uint32_t t0 = HAL_GetTick();
    uint16_t got = 0;
    while (got < n) {
        int wait;
        if (tmo == HAL_MAX_DELAY) wait = -1;
        else {
            uint32_t el = HAL_GetTick() - t0;
            if (el >= tmo) return HAL_TIMEOUT;
            wait = (int)(tmo - el);
        }
        struct pollfd p = { .fd = h->fd, .events = POLLIN };
        int r = poll(&p, 1, wait);
        if (r == 0) return HAL_TIMEOUT;
        if (r < 0) { if (errno == EINTR) continue; return HAL_ERROR; }
        ssize_t k = read(h->fd, d + got, n - got);
        if (k <= 0) { usleep(1000); continue; }
        for (ssize_t i = 0; i < k; i++) d[got + i] = maybe_corrupt(d[got + i], sim_rx_err_rate, &sim_rx_errs);
        got += (uint16_t)k;
    }
    return HAL_OK;
}

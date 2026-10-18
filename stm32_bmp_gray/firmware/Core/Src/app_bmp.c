/**
 * app_bmp.c - UART 로 True Color BMP 수신 -> W25Q32 저장 -> Flash 에서 읽어 8-bit Gray BMP 로
 *             변환하며 UART 로 송신.
 *
 * RAM 사용량 약 9 KB (rx 4 KB + tx 4 KB + 작업 버퍼). 이미지 전체를 RAM 에 올리지 않는다.
 * Gray 이미지는 Flash 에 다시 저장하지 않고, 송신하는 순간에 Flash 에서 읽어 즉석 변환한다
 * (3 MB 원본 + 1 MB Gray 는 4 MB Flash 에 함께 들어가지 않기 때문).
 */
#include <string.h>
#include <stdbool.h>
#include "app_bmp.h"
#include "app_config.h"
#include "bmp_link.h"
#include "w25q32.h"

#define RX_SOF_TIMEOUT_MS   HAL_MAX_DELAY
#define RX_HDR_TIMEOUT_MS   100u
#define TX_ACK_TIMEOUT_MS   1000u
#define TX_MAX_RETRY        10u

#define GRAY_HDR_SIZE       54u                      /* 14 + 40 */
#define GRAY_PAL_SIZE       1024u                    /* 256 * RGBQUAD */
#define GRAY_DATA_OFFSET    (GRAY_HDR_SIZE + GRAY_PAL_SIZE)
#define PIX_CHUNK           256u                     /* 한 번에 Flash 에서 읽는 픽셀 수 */

/* ---------- 버퍼 ---------- */
static uint8_t rx_payload[LINK_MAX_PAYLOAD + 2];
static uint8_t tx_frame[LINK_HDR_SIZE + LINK_MAX_PAYLOAD + 2];
static uint8_t work[PIX_CHUNK * 4];

/* ---------- 업로드 상태 ---------- */
static bool     up_active;
static uint32_t up_total, up_written;
static uint16_t up_next_seq;
static bool     img_valid;      /* Flash 에 CRC 검증된 이미지가 있음 */
static uint32_t img_size;

/* ---------- Gray 변환 정보 ---------- */
typedef struct {
    int32_t  width, height;     /* height < 0 이면 top-down, 그대로 유지 */
    uint32_t rows;
    uint16_t bpp;               /* 원본 24 or 32 */
    uint32_t src_off, src_stride;
    uint32_t gray_stride, gray_size;
    uint8_t  hdr[GRAY_HDR_SIZE];
} gray_info_t;
static gray_info_t gi;

/* ========================================================================== */
/*  CRC                                                                       */
/* ========================================================================== */
static uint16_t crc16_update(uint16_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint32_t crc32_table[256];
static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}
/* crc 는 0 에서 시작, zlib.crc32 와 동일한 결과 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) crc = crc32_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ========================================================================== */
/*  Little-endian helpers                                                     */
/* ========================================================================== */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ========================================================================== */
/*  UART frame layer                                                          */
/* ========================================================================== */
typedef enum { RX_OK, RX_TIMEOUT, RX_BAD_CRC, RX_BAD_LEN } rx_result_t;

static void uart_flush_rx(void)
{
    uint8_t b;
    __HAL_UART_CLEAR_OREFLAG(APP_PC_UART);
    while (HAL_UART_Receive(APP_PC_UART, &b, 1, 5) == HAL_OK) { }
    __HAL_UART_CLEAR_OREFLAG(APP_PC_UART);
}

static rx_result_t recv_frame(uint8_t *type, uint16_t *seq, uint16_t *len, uint32_t sof_timeout)
{
    uint8_t h[LINK_HDR_SIZE];

    do { /* SOF 동기화 */
        if (HAL_UART_Receive(APP_PC_UART, &h[0], 1, sof_timeout) != HAL_OK) return RX_TIMEOUT;
    } while (h[0] != LINK_SOF_PC2MCU);

    if (HAL_UART_Receive(APP_PC_UART, &h[1], LINK_HDR_SIZE - 1, RX_HDR_TIMEOUT_MS) != HAL_OK)
        return RX_TIMEOUT;

    *type = h[1];
    *seq  = rd16(&h[2]);
    *len  = rd16(&h[4]);
    if (*len > LINK_MAX_PAYLOAD) return RX_BAD_LEN;

    /* 921600 bps ≈ 92 B/ms. 여유있게 */
    uint32_t tmo = 100u + *len / 20u;
    if (HAL_UART_Receive(APP_PC_UART, rx_payload, (uint16_t)(*len + 2u), tmo) != HAL_OK)
        return RX_TIMEOUT;

    uint16_t crc = crc16_update(0xFFFFu, &h[1], LINK_HDR_SIZE - 1);
    crc = crc16_update(crc, rx_payload, *len);
    return (crc == rd16(&rx_payload[*len])) ? RX_OK : RX_BAD_CRC;
}

/* tx_frame 에 프레임 구성. payload 가 tx_frame+HDR 를 가리키면 복사 없이 그대로 사용 */
static void build_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t *f = tx_frame;
    f[0] = LINK_SOF_MCU2PC;
    f[1] = type;
    wr16(&f[2], seq);
    wr16(&f[4], len);
    if (len && payload != &f[LINK_HDR_SIZE]) memmove(&f[LINK_HDR_SIZE], payload, len);
    wr16(&f[LINK_HDR_SIZE + len], crc16_update(0xFFFFu, &f[1], LINK_HDR_SIZE - 1u + len));
}

static void transmit_frame(void)
{
    uint16_t len = rd16(&tx_frame[4]);
    (void)HAL_UART_Transmit(APP_PC_UART, tx_frame, (uint16_t)(LINK_HDR_SIZE + len + 2u), 2000);
}

static void send_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    build_frame(type, seq, payload, len);
    transmit_frame();
}

static void send_ack(uint16_t seq, uint8_t status, const uint8_t *extra, uint16_t extra_len)
{
    uint8_t p[24];
    p[0] = status;
    if (extra_len) memcpy(&p[1], extra, extra_len);
    send_frame(T_ACK, seq, p, (uint16_t)(1u + extra_len));
}

/* build_frame 으로 만든 프레임을 보내고 PC 의 ACK 를 기다림 (NAK/타임아웃 시 재전송) */
static bool send_reliable(void)
{
    uint16_t seq = rd16(&tx_frame[2]);
    uint32_t retry = 0, stale = 0;
    transmit_frame();
    while (retry < TX_MAX_RETRY && stale < 3u * TX_MAX_RETRY) {
        uint8_t type; uint16_t rseq, len;
        rx_result_t r = recv_frame(&type, &rseq, &len, TX_ACK_TIMEOUT_MS);
        if (r == RX_OK && type == T_ACK && rseq == seq) return true;
        if (r == RX_OK && type == T_ACK) { stale++; continue; } /* 이전 프레임의 지연 ACK -> 무시 */
        if (r != RX_OK) uart_flush_rx();
        retry++;
        transmit_frame();
    }
    return false;
}

/* ========================================================================== */
/*  Upload: PC -> Flash                                                       */
/* ========================================================================== */
static void handle_ping(uint16_t seq)
{
    uint8_t e[11];
    wr32(&e[0], W25Q_ReadJedecId());
    wr32(&e[4], W25Q32_FLASH_SIZE - APP_BMP_FLASH_BASE);
    wr16(&e[8], LINK_MAX_PAYLOAD);
    e[10] = LINK_PROTO_VERSION;
    send_ack(seq, ST_OK, e, sizeof e);
}

static void handle_start(uint16_t seq, uint16_t len)
{
    up_active = false;
    img_valid = false;
    if (len != 4) { send_ack(seq, ST_BAD_LEN, NULL, 0); return; }

    uint32_t total = rd32(rx_payload);
    if (total < GRAY_HDR_SIZE || total > W25Q32_FLASH_SIZE - APP_BMP_FLASH_BASE) {
        send_ack(seq, ST_TOO_BIG, NULL, 0);
        return;
    }
    /* 64 KB 블록 단위 erase: 3 MB 기준 약 48 블록, 수 초 소요 */
    if (W25Q_EraseRange(APP_BMP_FLASH_BASE, total) != W25Q_OK) {
        send_ack(seq, ST_FLASH_ERR, NULL, 0);
        return;
    }
    up_total = total;
    up_written = 0;
    up_next_seq = 0;
    up_active = true;
    send_ack(seq, ST_OK, NULL, 0);
}

static void handle_data(uint16_t seq, uint16_t len)
{
    uint8_t e[2];
    if (!up_active) { send_ack(seq, ST_BAD_STATE, NULL, 0); return; }

    if (seq == (uint16_t)(up_next_seq - 1u) && up_next_seq != 0) {
        /* 이미 기록한 프레임 재전송 (ACK 유실) -> 다시 쓰지 않고 ACK 만 */
        wr16(e, up_next_seq);
        send_ack(seq, ST_OK, e, 2);
        return;
    }
    if (seq != up_next_seq) {
        wr16(e, up_next_seq);
        send_ack(seq, ST_BAD_SEQ, e, 2);
        return;
    }
    if (len == 0 || up_written + len > up_total) {
        send_ack(seq, ST_BAD_LEN, NULL, 0);
        return;
    }
    if (W25Q_Write(APP_BMP_FLASH_BASE + up_written, rx_payload, len) != W25Q_OK) {
        send_ack(seq, ST_FLASH_ERR, NULL, 0);
        return;
    }
    up_written += len;
    up_next_seq++;
    wr16(e, up_next_seq);
    send_ack(seq, ST_OK, e, 2);
}

static void handle_end(uint16_t seq, uint16_t len)
{
    uint8_t e[4];
    if (len != 4 || !up_active || up_written != up_total) {
        send_ack(seq, ST_BAD_STATE, NULL, 0);
        return;
    }
    uint32_t want = rd32(rx_payload);

    /* Flash 에서 다시 읽어 전체 CRC 검증 */
    uint32_t crc = 0;
    for (uint32_t off = 0; off < up_total; ) {
        uint32_t n = up_total - off;
        if (n > sizeof work) n = sizeof work;
        if (W25Q_Read(APP_BMP_FLASH_BASE + off, work, n) != W25Q_OK) {
            send_ack(seq, ST_FLASH_ERR, NULL, 0);
            return;
        }
        crc = crc32_update(crc, work, n);
        off += n;
    }
    wr32(e, crc);
    up_active = false;
    img_valid = (crc == want);
    img_size  = up_total;
    send_ack(seq, img_valid ? ST_OK : ST_VERIFY_ERR, e, 4);
}

/* ========================================================================== */
/*  Gray conversion: Flash -> PC                                              */
/* ========================================================================== */
static uint8_t parse_bmp_header(void)
{
    uint8_t h[GRAY_HDR_SIZE];
    if (W25Q_Read(APP_BMP_FLASH_BASE, h, sizeof h) != W25Q_OK) return ST_FLASH_ERR;

    if (h[0] != 'B' || h[1] != 'M')          return ST_NOT_BMP;
    uint32_t off   = rd32(&h[10]);
    uint32_t isz   = rd32(&h[14]);
    int32_t  w     = (int32_t)rd32(&h[18]);
    int32_t  ht    = (int32_t)rd32(&h[22]);
    uint16_t pl    = rd16(&h[26]);
    uint16_t bpp   = rd16(&h[28]);
    uint32_t comp  = rd32(&h[30]);

    if (isz < 40 || pl != 1)                 return ST_NOT_BMP;
    if (bpp != 24 && bpp != 32)              return ST_UNSUPPORTED;  /* True Color 만 */
    if (comp != 0)                           return ST_UNSUPPORTED;  /* BI_RGB 만 */
    if (w <= 0 || ht == 0 || w > 65535 || ht < -65535 || ht > 65535) return ST_NOT_BMP;

    gi.width  = w;
    gi.height = ht;
    gi.rows   = (uint32_t)(ht < 0 ? -ht : ht);
    gi.bpp    = bpp;
    gi.src_off    = off;
    gi.src_stride = (((uint32_t)w * bpp + 31u) / 32u) * 4u;
    if ((uint64_t)off + (uint64_t)gi.src_stride * gi.rows > img_size) return ST_NOT_BMP;

    gi.gray_stride = ((uint32_t)w + 3u) & ~3u;
    uint32_t img   = gi.gray_stride * gi.rows;
    gi.gray_size   = GRAY_DATA_OFFSET + img;

    /* 8-bit 인덱스 BMP 헤더 (BITMAPFILEHEADER + BITMAPINFOHEADER) */
    uint8_t *g = gi.hdr;
    memset(g, 0, sizeof gi.hdr);
    g[0] = 'B'; g[1] = 'M';
    wr32(&g[2],  gi.gray_size);
    wr32(&g[10], GRAY_DATA_OFFSET);
    wr32(&g[14], 40);
    wr32(&g[18], (uint32_t)w);
    wr32(&g[22], (uint32_t)ht);           /* 행 순서(bottom-up/top-down) 원본과 동일 */
    wr16(&g[26], 1);
    wr16(&g[28], 8);
    wr32(&g[30], 0);                      /* BI_RGB */
    wr32(&g[34], img);
    memcpy(&g[38], &h[38], 8);            /* X/Y PelsPerMeter 원본 유지 */
    wr32(&g[46], 256);                    /* biClrUsed */
    wr32(&g[50], 0);
    return ST_OK;
}

/* ITU-R BT.601: Y = 0.299R + 0.587G + 0.114B  (정수 근사, 합 = 256) */
static inline uint8_t rgb_to_gray(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

/* Gray BMP 파일의 [off, off+n) 바이트를 생성 */
static bool gray_generate(uint32_t off, uint8_t *out, uint32_t n)
{
    while (n) {
        uint32_t k;
        if (off < GRAY_HDR_SIZE) {
            k = GRAY_HDR_SIZE - off; if (k > n) k = n;
            memcpy(out, &gi.hdr[off], k);
        } else if (off < GRAY_DATA_OFFSET) {
            k = GRAY_DATA_OFFSET - off; if (k > n) k = n;
            for (uint32_t i = 0; i < k; i++) {          /* 팔레트: (i,i,i,0) */
                uint32_t p = off - GRAY_HDR_SIZE + i;
                out[i] = ((p & 3u) == 3u) ? 0u : (uint8_t)(p >> 2);
            }
        } else {
            uint32_t p   = off - GRAY_DATA_OFFSET;
            uint32_t row = p / gi.gray_stride;
            uint32_t col = p % gi.gray_stride;
            if (col >= (uint32_t)gi.width) {             /* 행 패딩 */
                k = gi.gray_stride - col; if (k > n) k = n;
                memset(out, 0, k);
            } else {
                k = (uint32_t)gi.width - col;
                if (k > n) k = n;
                if (k > PIX_CHUNK) k = PIX_CHUNK;
                uint32_t bpp_b = gi.bpp / 8u;
                uint32_t src = APP_BMP_FLASH_BASE + gi.src_off + row * gi.src_stride + col * bpp_b;
                if (W25Q_Read(src, work, k * bpp_b) != W25Q_OK) return false;
                const uint8_t *s = work;
                for (uint32_t i = 0; i < k; i++, s += bpp_b)  /* BMP 픽셀 순서: B, G, R */
                    out[i] = rgb_to_gray(s[2], s[1], s[0]);
            }
        }
        off += k; out += k; n -= k;
    }
    return true;
}

static void handle_convert(uint16_t seq)
{
    if (!img_valid) { send_ack(seq, ST_BAD_STATE, NULL, 0); return; }

    uint8_t st = parse_bmp_header();
    if (st != ST_OK) { send_ack(seq, st, NULL, 0); return; }

    uint8_t e[14];
    wr32(&e[0], gi.gray_size);
    wr32(&e[4], (uint32_t)gi.width);
    wr32(&e[8], (uint32_t)gi.height);
    wr16(&e[12], gi.bpp);
    send_ack(seq, ST_OK, e, sizeof e);

    uint32_t crc = 0;
    uint16_t dseq = 0;
    uint8_t *payload = &tx_frame[LINK_HDR_SIZE];
    for (uint32_t off = 0; off < gi.gray_size; dseq++) {
        uint32_t n = gi.gray_size - off;
        if (n > LINK_MAX_PAYLOAD) n = LINK_MAX_PAYLOAD;
        if (!gray_generate(off, payload, n)) return;    /* Flash 오류 -> PC 타임아웃 */
        crc = crc32_update(crc, payload, n);
        build_frame(T_GRAY_DATA, dseq, payload, (uint16_t)n);
        if (!send_reliable()) return;
        off += n;
    }
    uint8_t c[4];
    wr32(c, crc);
    build_frame(T_GRAY_END, dseq, c, 4);
    (void)send_reliable();
}

/* ========================================================================== */
/*  Main loop                                                                 */
/* ========================================================================== */
static bool initialized;

static void app_init(void)
{
    crc32_init();
    (void)W25Q_Init();   /* 실패해도 PING 으로 JEDEC ID 를 확인할 수 있게 계속 진행 */
    initialized = true;
}

void App_BMP_Poll(void)
{
    if (!initialized) app_init();

    uint8_t type; uint16_t seq, len;
    rx_result_t r = recv_frame(&type, &seq, &len, RX_SOF_TIMEOUT_MS);
    if (r == RX_TIMEOUT)  { uart_flush_rx(); return; }
    if (r != RX_OK)       { uart_flush_rx(); send_frame(T_NAK, seq, (const uint8_t[]){ ST_BAD_CRC }, 1); return; }

    switch (type) {
    case T_PING:    handle_ping(seq);        break;
    case T_START:   handle_start(seq, len);  break;
    case T_DATA:    handle_data(seq, len);   break;
    case T_END:     handle_end(seq, len);    break;
    case T_CONVERT: handle_convert(seq);     break;
    case T_ACK:     /* 다운로드 종료 후 늦게 도착한 ACK */ break;
    default:        send_ack(seq, ST_UNKNOWN_CMD, NULL, 0); break;
    }
}

void App_BMP_Run(void)
{
    for (;;) App_BMP_Poll();
}

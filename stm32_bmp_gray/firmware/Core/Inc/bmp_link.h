/**
 * bmp_link.h - PC <-> MCU UART 프레임 프로토콜 정의 (pc/bmp_transfer.py 와 동일해야 함)
 *
 * Frame (little-endian):
 *   SOF(1) | TYPE(1) | SEQ(2) | LEN(2) | PAYLOAD(LEN) | CRC16(2)
 *   SOF   : PC->MCU 0xA5, MCU->PC 0x5A
 *   CRC16 : CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), TYPE..PAYLOAD 범위
 *
 * 모든 전송은 Stop-and-Wait (프레임 1개 전송 -> ACK 대기 -> 다음 프레임).
 * 수신 측이 Flash 쓰기 중일 때 데이터가 넘쳐 들어오지 않으므로 흐름제어가 필요 없다.
 *
 * 업로드  : PC  START(size) -> DATA(seq 0..N) -> END(crc32)
 * 다운로드: PC  CONVERT -> MCU ACK(info) -> MCU GRAY_DATA(seq 0..N) -> MCU GRAY_END(crc32)
 *           (MCU 가 보낸 프레임마다 PC 가 ACK/NAK 로 응답)
 */
#ifndef BMP_LINK_H
#define BMP_LINK_H

#define LINK_PROTO_VERSION      1u
#define LINK_SOF_PC2MCU         0xA5u
#define LINK_SOF_MCU2PC         0x5Au
#define LINK_HDR_SIZE           6u      /* SOF..LEN */
#define LINK_MAX_PAYLOAD        4096u   /* = W25Q32 섹터 크기 */

/* Frame types */
#define T_PING                  0x01u   /* -> ACK [st, jedec u32, flash_size u32, max_payload u16, ver u8] */
#define T_START                 0x02u   /* [total_size u32]          -> ACK [st]           */
#define T_DATA                  0x03u   /* [bytes...]                -> ACK [st, expect u16] */
#define T_END                   0x04u   /* [crc32 u32]               -> ACK [st, flash_crc32 u32] */
#define T_CONVERT               0x05u   /* []                        -> ACK [st, gray_size u32, w i32, h i32, bpp u16] */
#define T_ACK                   0x80u
#define T_NAK                   0x81u
#define T_GRAY_DATA             0x84u   /* MCU -> PC [bytes...]  */
#define T_GRAY_END              0x85u   /* MCU -> PC [crc32 u32] */

/* Status codes (ACK payload[0]) */
#define ST_OK                   0x00u
#define ST_BAD_CRC              0x01u
#define ST_BAD_SEQ              0x02u
#define ST_TOO_BIG              0x03u
#define ST_FLASH_ERR            0x04u
#define ST_VERIFY_ERR           0x05u
#define ST_NOT_BMP              0x06u
#define ST_UNSUPPORTED          0x07u
#define ST_BAD_STATE            0x08u
#define ST_BAD_LEN              0x09u
#define ST_UNKNOWN_CMD          0x0Au

#endif /* BMP_LINK_H */

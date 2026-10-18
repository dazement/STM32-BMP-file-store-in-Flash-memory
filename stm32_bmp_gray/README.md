# STM32 + W25Q32 : UART BMP → Flash 저장 → Gray 변환 → PC 반환

```
 PC (Python)                         STM32 (HAL)                      W25Q32 (4 MB)
 ─────────────                       ─────────────                    ─────────────
 color.bmp (≥3 MB, 24bpp) ── UART ─▶ 4 KB 프레임 수신 ── SPI ───────▶ 0x000000~ 저장
                                     read-back CRC32 검증 ◀────────── 다시 읽기
 gray.bmp (8bpp)  ◀───────── UART ── 즉석 Gray 변환     ◀── SPI ───── 픽셀 읽기
 verify_gray_bmp.py 로 검증
```

## 폴더 구성

| 경로 | 내용 |
|---|---|
| `firmware/Core/Inc/app_config.h` | 보드 설정 (SPI/UART 핸들, CS 핀) — **여기만 수정** |
| `firmware/Core/Inc/bmp_link.h` | UART 프레임 프로토콜 정의 |
| `firmware/Core/Src/w25q32.c/.h` | W25Q32 드라이버 (JEDEC ID, read, page program, 4K/64K/chip erase) |
| `firmware/Core/Src/app_bmp.c/.h` | 수신 → Flash 저장 → 검증 → Gray 변환/송신 |
| `pc/make_test_bmp.py` | 테스트용 1024×1024 24bpp BMP(3,145,782 B) 생성 |
| `pc/bmp_transfer.py` | 업로드/다운로드/검증 한 번에 수행 |
| `pc/verify_gray_bmp.py` | Gray BMP 구조 검사 + 원본과 픽셀 단위 비교 |
| `host_sim/` | 보드 없이 PC 에서 **실제 펌웨어 코드**를 돌려보는 시뮬레이터 (W25Q32 에뮬레이터 + 가상 시리얼) |

## 1. 하드웨어 연결 (예: NUCLEO-F401RE / F411RE, SPI1)

| W25Q32 | STM32 | 비고 |
|---|---|---|
| VCC | 3.3V | **5V 금지** |
| GND | GND | |
| CLK | PA5 (SPI1_SCK) | |
| DO (MISO) | PA6 (SPI1_MISO) | |
| DI (MOSI) | PA7 (SPI1_MOSI) | |
| CS | PA4 (GPIO Output) | 라벨 `FLASH_CS` 권장 |
| /WP, /HOLD | 3.3V | 모듈에 풀업이 없으면 직접 연결 |

PC ↔ MCU: Nucleo 는 ST-Link 가상 COM(USART2, PA2/PA3) 을 그대로 사용. 다른 보드는 USB-UART(CP2102/FT232/CH340) 의 TX↔RX 교차 연결 + GND 공통.

## 2. CubeMX 설정

- **SPI1**: Full-Duplex Master, 8 bit, MSB first, CPOL Low, CPHA 1 Edge, NSS = Software, Prescaler → 21 MHz 이하
- **PA4**: GPIO_Output, 초기값 High, User Label `FLASH_CS`
- **USART2**: Asynchronous, **921600** 8N1 (오류가 많으면 460800/115200 으로 낮추고 PC 의 `--baud` 도 동일하게)
- 인터럽트/DMA 불필요 (polling + stop-and-wait 프로토콜)

다른 SPI/UART 를 쓰면 `app_config.h` 의 `hspi1`, `huart2` 만 바꾸면 된다.

## 3. 펌웨어 통합

`firmware/Core` 의 파일들을 CubeMX 프로젝트의 `Core/Inc`, `Core/Src` 에 복사하고 `main.c` 에 두 줄 추가:

```c
/* USER CODE BEGIN Includes */
#include "app_bmp.h"
/* USER CODE END Includes */
...
  MX_SPI1_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  App_BMP_Run();          /* 반환하지 않음 */
  /* USER CODE END 2 */
```

RAM 사용 약 9 KB, 이미지를 RAM 에 올리지 않으므로 F103(20 KB RAM) 에서도 동작한다.

## 4. PC 실행

```bash
pip install pyserial pillow
```

```bash
python pc/make_test_bmp.py color.bmp
```

```bash
python pc/bmp_transfer.py --port COM5 all color.bmp gray.bmp --png gray.png
```

(macOS: `/dev/tty.usbmodemXXXX`, Linux: `/dev/ttyACM0`) — 개별 단계: `ping`, `upload color.bmp`, `download gray.bmp --ref color.bmp`

출력 예:
```
MCU 연결됨: JEDEC ID EF4016 (W25Q32), 저장 가능 4,194,304 bytes, proto v1
[1/3] Flash erase 중... → 업로드 100% → Flash 저장 + read-back CRC32 일치
[2/3] 원본 1024x1024 24bpp -> Gray 8bpp 1,049,654 bytes → CRC32 일치
[3/3] Gray BMP 검증
  [OK] bfSize == 파일 크기 / 8 bpp / BI_RGB / 팔레트 R==G==B, 0..255 램프
  [OK] 픽셀 일치 1,048,576/1,048,576
  [OK] Pillow convert('L') 대비 최대 오차 1
== 결과: 정상적인 Gray BMP 입니다.
```

예상 소요 시간 (921600 bps): erase 약 5–10 s + 업로드 약 45 s + 다운로드 약 13 s. 115200 bps 에서는 업로드 약 5.5 분.

## 5. 설계 포인트

- **Stop-and-Wait + CRC16 프레임**: 4 KB 보낼 때마다 ACK 를 기다리므로 Flash 쓰기 중 UART 오버런이 없다. CRC 오류/타임아웃이 나면 재전송하고, ACK 가 유실돼 같은 프레임이 다시 와도 seq 로 걸러서 두 번 쓰지 않는다.
- **Erase 는 START 때 한 번**: 64 KB 블록 erase 를 사용한다 (4 KB 섹터 768번보다 훨씬 빠름). Page Program 은 256 B 페이지 경계에서 나눠서 쓴다.
- **저장 검증**: 업로드가 끝나면 MCU 가 Flash 를 처음부터 다시 읽어 CRC32 를 계산하고, PC 가 보낸 CRC32 와 비교한다.
- **Gray 이미지는 Flash 에 다시 저장하지 않음**: 3 MB 원본 + 1 MB Gray 는 4 MB 칩에 함께 들어가지 않는다. 그래서 송신할 때 Flash 에서 256 픽셀씩 읽어 바로 변환한다.
- **변환 공식**: `Y = (77·R + 150·G + 29·B + 128) >> 8` (BT.601 정수 근사). 출력은 8bpp 인덱스 BMP 이고 팔레트는 256단계 그레이다. 행 패딩(4바이트 정렬)과 bottom-up/top-down 방향은 원본을 따른다.
- **입력 지원**: 24bpp/32bpp BI_RGB, 파일 크기는 4 MiB 이하 (예: 1280×1024×24bpp = 3.93 MB 까지 가능, 1920×1080 은 6.2 MB 라 불가).

## 6. 보드 없이 테스트 (host_sim)

`app_bmp.c`, `w25q32.c` 를 수정 없이 PC 에서 빌드한다. 가짜 HAL 이 W25Q32 를 명령 단위로 에뮬레이션하고 (WREN 없으면 무시, erase 없이 쓰면 비트 AND, 페이지 wrap, BUSY), UART 는 가상 시리얼(pty) 로 대신한다. macOS/Linux 에서 사용 가능.

```bash
make -C host_sim
```

```bash
./host_sim/mcu_sim --rx-err 2e-5 --tx-err 2e-5
```

출력된 `/dev/ttysNNN` 을 포트로 사용 (pty 는 표준 baud 만 받으므로 `--baud 115200`):

```bash
python pc/bmp_transfer.py --port /dev/ttys000 --baud 115200 all color.bmp gray.bmp
```

## 7. 문제 해결

| 증상 | 확인 |
|---|---|
| `응답 없음 (type=0x01)` | 포트/보레이트, TX↔RX 교차, `App_BMP_Run()` 호출 여부 |
| JEDEC ID 가 `000000`/`FFFFFF` | SPI 배선, CS 핀, VCC 3.3V, SPI Mode 0 |
| `FLASH_ERR` | /WP 핀 Low 여부 (Write Enable 실패), SPI 클럭을 낮춰 보기 |
| 재전송이 매우 많음 | 보레이트 낮추기 (클럭 오차), USB-UART 칩의 921600 지원 여부 |
| `UNSUPPORTED` | 원본이 8/16bpp 또는 압축 BMP — 24bpp 로 다시 저장 |

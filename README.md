# BMP Flash 저장 실습

STM32F746G-DISCO의 USART1과 보드 내장 QUADSPI Flash를 사용합니다.
현재 단계는 **3 MiB 이상의 24비트 BMP 전체 저장 → Flash 재읽기 → Gray BMP 변환 → PC 수신**입니다.
UART RX는 하드웨어 Double Buffer, Gray 출력 TX는 DMA 이중 버퍼를 사용합니다.

## 준비 (macOS · Windows)

| 항목 | macOS | Windows |
| --- | --- | --- |
| VS Code + STM32Cube for VS Code 확장 | 필요 (ARM GCC, CMake, Ninja, STM32CubeProgrammer를 자동 설치) | 같음 |
| Python 3.9 이상 | `python3` (Xcode Command Line Tools에 포함) | [python.org](https://www.python.org/downloads/)에서 설치, `py` 명령 사용 |
| pyserial | 선택 (없으면 표준 라이브러리로 통신) | **필수**: `py -m pip install pyserial` |
| C 컴파일러 (모의 테스트용, 선택) | Xcode Command Line Tools의 `cc` | MSYS2 MinGW `gcc` 또는 LLVM `clang` (없으면 건너뜀) |
| 보드 포트 | `/dev/cu.usbmodem…` 자동 선택 | `COM3` 같은 COM 포트 자동 선택 |

Windows에서 ST-LINK가 인식되지 않으면 ST 웹사이트의 ST-LINK USB 드라이버(STSW-LINK009)를 설치합니다.

```sh
git clone https://github.com/dazement/STM32-BMP-file-store-in-Flash-memory.git
cd STM32-BMP-file-store-in-Flash-memory
```

이 README의 명령은 저장소 폴더에서 실행합니다. **Windows에서는 `python3` 대신 `py`를 씁니다**
(예: `py pc/bmp_gray.py`). 포트를 직접 고를 때는 `--port /dev/cu.usbmodem1103`(macOS),
`--port COM5`(Windows)처럼 지정합니다.

| 작업 | macOS | Windows (PowerShell·명령 프롬프트) |
| --- | --- | --- |
| 전체 실행 | `./run_all.sh` | `py run_all.py` |
| Gray 변환 | `python3 pc/bmp_gray.py` | `py pc/bmp_gray.py` |
| 원본 저장·복원 Demo | `python3 pc/demo_roundtrip.py` | `py pc/demo_roundtrip.py` |
| PC 단위 테스트 | `python3 -m unittest discover -s pc -p "test_*.py"` | `py -m unittest discover -s pc -p "test_*.py"` |

## STM32CubeMX 설정

`bmp_gray_practice.ioc`를 STM32CubeMX 6.18.1(STM32Cube FW_F7 V1.17.4)로 열면 아래 설정이 모두 들어 있습니다.
처음부터 만들 때는 Board Selector에서 **STM32F746G-DISCO**를 고르고,
모든 주변장치를 기본 모드로 초기화할지 묻는 창에서 No를 누른 뒤 아래 항목만 설정합니다.

### Pinout & Configuration

| 항목 | 설정 |
| --- | --- |
| SYS | Debug: Serial Wire (PA13 SWDIO, PA14 SWCLK), Timebase Source: SysTick |
| RCC | HSE: Crystal/Ceramic Resonator (PH0, PH1) |
| USART1 | Mode: Asynchronous, PA9 TX, PB7 RX (ST-LINK 가상 COM 포트) |
| USART1 Parameter | 115200 Bits/s, 8 Bits, Parity None, Stop Bits 1, TX and RX, Over Sampling 16, HW Flow Control 없음 |
| QUADSPI | Bank1 with Quad SPI Lines: PB2 CLK, PB6 NCS, PD11 IO0, PD12 IO1, PE2 IO2, PD13 IO3 |
| QUADSPI Parameter | Clock Prescaler 7 (27 MHz), FIFO Threshold 4, Sample Shifting: Half Cycle, Flash Size 23 (16 MiB), Chip Select High Time 8 Cycles, Clock Mode Low |
| CORTEX_M7 | CPU ICache·DCache Disabled, MPU는 CubeMX 기본 Region 0 유지 |

USART1의 DMA Settings 탭에서 두 요청을 추가합니다. 두 요청 모두 FIFO는 사용하지 않습니다.

| DMA Request | Stream | Direction | Mode | Priority | Increment | Data Width |
| --- | --- | --- | --- | --- | --- | --- |
| USART1_RX | DMA2 Stream 2 | Peripheral To Memory | **Circular** | Low | Memory | Byte / Byte |
| USART1_TX | DMA2 Stream 7 | Memory To Peripheral | Normal | Low | Memory | Byte / Byte |

NVIC는 Priority Group을 4 bits for pre-emption priority로 두고
USART1 global interrupt, DMA2 stream2 global interrupt, DMA2 stream7 global interrupt를 켭니다.

### Clock Configuration

- HSE 25 MHz → PLL Source HSE, /M 25, ×N 432, /P 2 → SYSCLK 216 MHz (System Clock Mux: PLLCLK).
- /Q는 9로 둡니다(48 MHz).
- AHB /1 = 216 MHz, APB1 /4 = 54 MHz, APB2 /2 = 108 MHz입니다. USART1 클럭은 PCLK2 108 MHz입니다.
- Over-Drive와 Flash Latency 7 WS는 CubeMX가 216 MHz 설정에 맞춰 생성합니다.

### Project Manager

- Project Name `bmp_gray_practice`, Toolchain / IDE: **CMake**.
- Minimum Heap Size 0x200, Minimum Stack Size 0x400.
- Code Generator: Copy only the necessary library files,
  Keep User Code when re-generating 체크, Delete previously generated files when not re-generated 체크.

### 코드 재생성 시 주의

- USART1_RX DMA는 반드시 Circular여야 합니다. `UartDmaRx_Start()`는 Circular가 아니면 `HAL_ERROR`를 반환합니다.
  Double Buffer 모드는 CubeMX 항목이 아니며 펌웨어가 `HAL_DMAEx_MultiBufferStart_IT()`로 실행 중에 켭니다.
- 115200은 부팅 시 속도입니다. 921600·2250000은 PC 요청으로 펌웨어가 실행 중에 바꿉니다.
- CubeMX 기본 MPU Region 0은 0x60000000–0xDFFFFFFF 접근을 막습니다.
  QUADSPI 레지스터(0xA0001000)를 여는 Region 1은 `main.c`의 `USER CODE BEGIN SysInit` 블록에 있으므로 재생성해도 유지됩니다.
- 직접 작성한 코드는 `USER CODE BEGIN`/`END` 블록 안에만 둡니다.
  추가 소스 파일은 최상위 `CMakeLists.txt`의 `target_sources`에 등록되어 있으며,
  이 파일은 처음 한 번만 생성되므로 재생성해도 덮어쓰지 않습니다(`cmake/stm32cubemx/`는 매번 다시 생성됩니다).
- GENERATE CODE 후 CMake Debug 프리셋으로 다시 빌드합니다.

## 회색조 전체 과정 실행

새 펌웨어를 다운로드하고 Continue를 누른 뒤, Serial Monitor를 닫고 실행합니다.

```sh
python3 pc/bmp_gray.py
```

운용 출력은 8비트 Gray로 고정합니다. UART 전송 단위는 8N1이며 입력 원본은 24비트 BMP를 유지합니다.
기본 입력은 아래의 3,145,782바이트 BMP입니다. UART 기본 속도는 921600입니다.
원본 전체를 백업 없이 바로 Flash에 저장하며, 삭제·프로그램 완료를 확인한 다음
Flash 원본 전체 CRC를 검사하고 픽셀을 Flash에서 다시 읽어 변환합니다.
MCU는 전체 이미지를 RAM에 보관하지 않습니다.

결과는 `pc/results/test_1024x1024_gray8.bmp`에 저장됩니다.
PC는 출력 CRC, Flash 원본 CRC, 독립 기준 변환과의 바이트 비교를 모두 확인합니다.
8비트 출력은 256개 회색 팔레트를 포함한 정상 BMP입니다.
변환식은 `(77*R + 150*G + 29*B + 128) >> 8`입니다.
상향식·하향식 저장 순서와 행 패딩을 처리합니다.

`pc/results/timings.csv`에 입력 전송 요청부터 Gray 마지막 바이트 수신까지의 시간을 기록합니다.
삭제 시간은 본 처리 시간에 포함됩니다.
`--repeat 3`으로 8비트 출력을 3회 측정하면 중앙값을 확인할 수 있습니다.
`--format 8`이 기본이며, `--baud 2250000`으로 더 높은 링크 속도를 시험할 수 있습니다.

Flash의 저장 데이터만으로 변환하는 확인은 MCU를 리셋한 뒤 다음과 같이 실행합니다.
원본 파일은 Flash CRC와 출력 비교의 기준으로만 PC에서 사용하며 MCU로 재전송하지 않습니다.

```sh
python3 pc/bmp_gray.py --convert-only
```

## 실행

아래 명령은 회색조 변환 전의 원본 저장·재읽기 진단용입니다.

1. VS Code의 Serial Monitor를 종료합니다.
2. 새 펌웨어를 Run and Debug로 다운로드하고 Continue를 누릅니다.
3. 일반 터미널에서 실행합니다.

```sh
python3 pc/bmp_store.py
```

기본 입력은 `pc/images/test_1024x1024.bmp`입니다.
1024×1024, 24비트, 3,145,782바이트이며 없으면 자동 생성합니다.
자신의 BMP는 `--in 경로/image.bmp`로 선택합니다. `pc/images/earth_apollo17_1024x1024.bmp`도 함께 들어 있습니다.
입력은 압축되지 않은 24비트 BMP, 3 MiB 이상 4,128,768바이트 이하입니다.

프로그램은 시작 시 115200 baud로 연결한 뒤 921600 baud로 변경합니다.
`--baud 115200`으로 낮은 속도에서 비교할 수도 있습니다.
정상 종료 시 MCU 통신 속도를 115200으로 복구합니다.
오류 후 다음 연결이 실패하면 보드 RESET으로 초기화합니다.

## 처리 구조

- DMA2 Stream2 Channel4, 하드웨어 DBM, M0/M1 각각 4096바이트.
- PC는 CRC가 붙은 청크를 최대 두 개까지 미리 전송합니다.
- DMA가 B를 채우는 동안 A에서 복사한 청크를 Flash에 저장할 수 있습니다.
- CT, NDTR, 완료 콜백으로 수신 위치와 버퍼 재사용을 추적합니다.
- 부분 버퍼도 읽을 수 있어 짧은 명령이나 마지막 청크가 대기하지 않습니다.
- 현재 D-cache는 비활성화되어 있습니다. 활성화하려면 DMA 메모리의 캐시 정책을 함께 설정해야 합니다.
- 64 KiB 블록 삭제, 256바이트 Page Program, BUSY/WEL 확인을 사용합니다.

첫 4 MiB 안의 0x000000–0x3EFFFF를 이미지 영역으로 사용합니다.
실제 삭제 범위는 파일 크기를 64 KiB 경계로 올림한 크기입니다.
저장 전에 기존 Flash 내용을 백업하지 않으므로 이 범위의 기존 데이터는 그대로 덮어써집니다.
펌웨어는 청크를 받을 때마다 64 KiB 경계에서 블록을 삭제하고 바로 Page Program합니다.
실패한 재읽기는 `.partial`로 남고 완료된 파일로 취급하지 않습니다.

이미지를 전부 저장한 뒤 Flash에서 별도로 다시 읽어
`pc/results/*_flash_readback.bmp`에 저장합니다.
PC 원본과 바이트 단위 비교 및 CRC 비교가 모두 통과해야 PASS입니다.
버퍼 A/B 전환 횟수와 버퍼 초과 횟수도 출력합니다.
표시 시간은 저장 및 재읽기 구간입니다.

## 작은 데이터 반복 확인

```sh
python3 pc/uart_flash_test.py --repeat 12
```

이 테스트는 칩의 마지막 4 KiB에서 1024바이트 저장·재읽기를 반복합니다.
비어 있거나 이전 실습의 검증된 기록이 있는 경우에만 그 영역을 재사용합니다.

## 개발 검증

펌웨어는 CMake Debug 프리셋으로 빌드합니다.
PC 프로토콜 및 BMP 생성 검사는 다음과 같습니다.

```sh
python3 -m unittest discover -s pc -p "test_*.py" -v
```

아래 C 모의 검사 명령은 macOS 기준입니다. Windows에서는 `py run_all.py`가 `gcc`나 `clang`을 찾아 같은 검사를 실행합니다.
DBM 소비자 검사는 생산 코드에 모의 DMA 레지스터를 연결하여
A/B 전환, 부분 수신, 지연된 IRQ, 덮어쓰기 감지와 오류 복구를 확인합니다.

```sh
cc -std=c11 -Wall -Wextra -Werror -I tests/dma_stub tests/test_uart_dma.c -o /tmp/bmp_gray_dbm_test
/tmp/bmp_gray_dbm_test
```

TX 이중 버퍼와 BMP 변환 핵심 함수도 같은 방식으로 검사합니다.

```sh
cc -std=c11 -Wall -Wextra -Werror -I tests/dma_stub tests/test_uart_tx_dma.c -o /tmp/bmp_gray_tx_test
/tmp/bmp_gray_tx_test
cc -std=c11 -Wall -Wextra -Werror -I Core/Inc tests/test_bmp_gray.c Core/Src/bmp_gray.c -o /tmp/bmp_gray_core_test
/tmp/bmp_gray_core_test
```

모의 검사는 실제 보드에서의 통신 검증을 대신하지 않습니다.

VS Code 없이 명령줄에서 빌드·다운로드하려면 OS와 관계없이 `run_all.py`를 쓰는 것이 가장 간단합니다.
STM32Cube 확장이 설치한 도구를 `CUBE_BUNDLE_PATH` 환경변수나 기본 위치
(macOS `~/Library/Application Support/stm32cube/bundles`, Windows `%LOCALAPPDATA%\stm32cube\bundles`)에서 찾습니다.
macOS에서 직접 실행할 때는 다음과 같습니다(번들 버전은 설치된 것에 맞춰 바꿉니다).

```sh
CUBE="$HOME/Library/Application Support/stm32cube/bundles"
export PATH="$CUBE/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH"
cmake --preset Debug && cmake --build --preset Debug
"$CUBE/programmer/2.23.0/bin/STM32_Programmer_CLI" -c port=SWD mode=UR reset=HWrst \
  -d build/Debug/bmp_gray_practice.elf -v -rst
```

## 한 번에 전체 실행

Serial Monitor와 VS Code 디버그 세션을 닫고 실행합니다.
단위 테스트 → 빌드(`build/cli/`) → 다운로드 → 보드 검증 → Flash 저장 증명을 차례로 수행하며,
실패하면 그 단계에서 멈춥니다. 실제 내용은 `run_all.py` 하나이고, `run_all.sh`는 macOS용 바로가기입니다.

```sh
./run_all.sh          # macOS (python3 run_all.py와 같음)
py run_all.py         # Windows
```

시작할 때 도구 위치, 보드 포트, ST-LINK를 다른 프로그램이 쓰고 있는지를 먼저 확인합니다.
C 컴파일러가 없으면 C 모의 테스트만 건너뛰고 마지막에 알려 줍니다.

Flash 저장 증명은 두 가지입니다.

1. ST 외부 로더로 펌웨어를 거치지 않고 QSPI Flash 칩을 직접 덤프해 원본 BMP와 비교합니다.
2. SWD로 MCU RAM 320 KB 전체를 0xA5로 덮어쓴 뒤 펌웨어를 재시작하고, Flash에서만 복원·변환합니다.

이미지(3,145,782바이트)는 MCU 내부 RAM(320 KB)보다 크고, 보드의 SDRAM은 초기화하지 않으며 MPU로 막혀 있습니다.

## 보드 검증 결과 (2026-10-15)

STM32F746G-DISCO(ST-LINK V2J40M27, QSPI Flash W25Q128 / JEDEC EF4018)에서 백업을 제거한 현재 소스를
빌드·다운로드한 뒤 `run_all.py`로 확인했습니다(전체 약 4분).
macOS에서 표준 라이브러리 통신과 pyserial 통신(Windows와 같은 경로) 두 방식 모두 9단계를 통과했습니다.
Windows 실제 PC에서의 실행은 아직 확인하지 않았으며, Windows용 도구 경로 탐색은 모의 폴더로 확인했습니다.
UART는 921600 baud, 입력은 `pc/images/test_1024x1024.bmp`(3,145,782바이트, CRC32 978AB7D9)입니다.

| 항목 | 명령 | 결과 |
| --- | --- | --- |
| PC 단위 테스트 | `python3 -m unittest discover -s pc -p 'test_*.py'` | 18개 PASS |
| C 모의 테스트 | `tests/test_uart_dma.c`, `test_uart_tx_dma.c`, `test_bmp_gray.c` | 3개 PASS |
| 펌웨어 빌드 | CMake Debug, GCC 14.3.1 | 경고 0, FLASH 38,164 B (3.6%), RAM 35,320 B (10.8%) |
| 다운로드 | STM32_Programmer_CLI | Download verified successfully |
| 원본 저장·복원 | `demo_roundtrip.py` | 3,145,782바이트 전체 일치, CRC32·SHA-256 일치, 버퍼 초과 0회, 86.5 s |
| Gray 전체 과정 | `bmp_gray.py` | Flash CRC 일치, 기준 변환과 일치, 72.8 s |
| 작은 데이터 반복 | `uart_flash_test.py --repeat 12` | 12회 모두 PASS |
| Flash 칩 직접 덤프 | ST 외부 로더 `W25Q128JVEIQ_STM32F746G-DISCO.stldr` | `0x90000000`부터 3,145,782바이트 = 원본 BMP |
| RAM 전체 덮어쓴 뒤 복원 | `demo_roundtrip.py --read-only` | RAM 320 KB를 0xA5로 채운 뒤에도 전체 일치, 34.6 s |
| RAM 전체 덮어쓴 뒤 Gray 변환 | `bmp_gray.py --convert-only` | Flash CRC 일치, 기준 변환과 일치, 20.9 s |

원본과 복원 BMP의 SHA-256은 `0bfd1da377908544c1b1d2c0e10f8a3b53434448786bcac53d12b27c50f1dd8c`로 같습니다.
8비트 Gray 출력은 1,049,654바이트, CRC32 58622AEF로 이전 측정과 같습니다.
프로젝트 스크립트와 별도로 `shasum`·`cmp`로 복원 파일을 비교하고,
변환식을 다시 계산해 1,048,576개 픽셀 모두와 256단계 팔레트가 일치함을 확인했습니다.
측정 기록은 `pc/results/demo_report_20261015_*.json`과 `pc/results/timings.csv`에 있습니다.

## 10월 21일 2차 발표 필수 Demo

원본 BMP 그대로 저장·복원하는 실행 프로그램입니다.

```sh
python3 pc/demo_roundtrip.py
```

리셋 후 Flash에서 복원만 확인하려면 `--read-only`를 사용합니다.
CRC32, SHA-256, 전체 바이트 비교와 실제 칩 ID를 JSON으로 기록합니다.
발표 자료와 대본은 `presentation_oct20/output/`에 있습니다.

## 문제 해결

| 증상 | 해결 |
| --- | --- |
| `ST-LINK를 다른 프로그램이 사용 중입니다` | VS Code 디버그 세션(Shift+F5)이나 STM32CubeProgrammer를 종료합니다. |
| `시리얼 포트` 오류, 포트를 열 수 없음 | VS Code Serial Monitor 등 포트를 연 프로그램을 닫습니다. 포트가 여러 개면 `--port`로 지정합니다. |
| `Windows에서는 pyserial이 필요합니다` | `py -m pip install pyserial` |
| `도구를 찾을 수 없습니다` | VS Code에 STM32Cube 확장을 설치하고 프로젝트를 한 번 빌드합니다. 다른 위치라면 `CUBE_BUNDLE_PATH`를 번들 폴더로 지정합니다. |
| `보드 응답 시간 초과` | 보드 RESET 버튼을 누르고 다시 실행합니다. 검증 중 한 번, 3 MB 재읽기가 95.8% 지점에서 멈췄습니다(받은 부분은 원본과 일치). 이후 7회 반복에서는 재현되지 않았습니다. |

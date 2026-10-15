# BMP Flash 저장 실습

STM32F746G-DISCO의 USART1과 보드 내장 QUADSPI Flash를 사용합니다.
현재 단계는 **3 MiB 이상의 24비트 BMP 전체 저장 → Flash 재읽기 → Gray BMP 변환 → PC 수신**입니다.
UART RX는 하드웨어 Double Buffer, Gray 출력 TX는 DMA 이중 버퍼를 사용합니다.

## 회색조 전체 과정 실행

새 펌웨어를 다운로드하고 Continue를 누른 뒤, Serial Monitor를 닫고 실행합니다.

```sh
python3 /Users/minho/gradproj/bmp_gray_practice/pc/bmp_gray.py
```

운용 출력은 8비트 Gray로 고정합니다. UART 전송 단위는 8N1이며 입력 원본은 24비트 BMP를 유지합니다.
기본 입력은 아래의 3,145,782바이트 BMP입니다. UART 기본 속도는 921600입니다.
기존 Flash 백업 후 원본 전체를 저장하며, 삭제·프로그램 완료를 확인한 다음
Flash 원본 전체 CRC를 검사하고 픽셀을 Flash에서 다시 읽어 변환합니다.
MCU는 전체 이미지를 RAM에 보관하지 않습니다.

결과는 `pc/results/test_1024x1024_gray8.bmp`에 저장됩니다. 기존 24비트 결과와 비교 CSV는 이전 실험 자료로 보존합니다.
PC는 출력 CRC, Flash 원본 CRC, 독립 기준 변환과의 바이트 비교를 모두 확인합니다.
8비트 출력은 256개 회색 팔레트를 포함한 정상 BMP입니다.
변환식은 `(77*R + 150*G + 29*B + 128) >> 8`입니다.
상향식·하향식 저장 순서와 행 패딩을 처리합니다.

`pc/results/timings.csv`에 입력 전송 요청부터 Gray 마지막 바이트 수신까지의 시간을 기록합니다.
백업 준비 시간은 별도 열에 기록합니다. 삭제 시간은 본 처리 시간에 포함됩니다.
`--repeat 3`으로 8비트 출력을 3회 측정하면 중앙값을 확인할 수 있습니다.
`--format 8`이 기본이며, `--baud 2250000`으로 더 높은 링크 속도를 시험할 수 있습니다.

Flash의 저장 데이터만으로 변환하는 확인은 MCU를 리셋한 뒤 다음과 같이 실행합니다.
원본 파일은 Flash CRC와 출력 비교의 기준으로만 PC에서 사용하며 MCU로 재전송하지 않습니다.

```sh
python3 /Users/minho/gradproj/bmp_gray_practice/pc/bmp_gray.py --convert-only
```

## 실행

아래 명령은 회색조 변환 전의 원본 저장·재읽기 진단용입니다.

1. VS Code의 Serial Monitor를 종료합니다.
2. 새 펌웨어를 Run and Debug로 다운로드하고 Continue를 누릅니다.
3. 일반 터미널에서 실행합니다.

```sh
python3 /Users/minho/gradproj/bmp_gray_practice/pc/bmp_store.py
```

기본 입력은 `pc/images/test_1024x1024.bmp`입니다.
1024×1024, 24비트, 3,145,782바이트이며 없으면 자동 생성합니다.
자신의 BMP는 `--in /absolute/path/image.bmp`로 선택합니다.
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
삭제 전에 그 전체 범위를 PC의 `pc/backups/`에 읽고 CRC를 확인해 저장합니다.
펌웨어는 백업 범위를 보내지 않은 상태에서는 큰 파일 쓰기를 거부합니다.
실패한 읽기는 `.partial`로 남고 완료된 백업으로 취급하지 않습니다.

이미지를 전부 저장한 뒤 Flash에서 별도로 다시 읽어
`pc/results/*_flash_readback.bmp`에 저장합니다.
PC 원본과 바이트 단위 비교 및 CRC 비교가 모두 통과해야 PASS입니다.
버퍼 A/B 전환 횟수와 버퍼 초과 횟수도 출력합니다.
표시 시간은 저장 및 재읽기 구간이며, 백업 준비 시간은 별도입니다.

## 작은 데이터 반복 확인

```sh
python3 /Users/minho/gradproj/bmp_gray_practice/pc/uart_flash_test.py --repeat 12
```

이 테스트는 칩의 마지막 4 KiB에서 1024바이트 저장·재읽기를 반복합니다.
비어 있거나 이전 실습의 검증된 기록이 있는 경우에만 그 영역을 재사용합니다.

## 개발 검증

펌웨어는 CMake Debug 프리셋으로 빌드합니다.
PC 프로토콜 및 BMP 생성 검사는 다음과 같습니다.

```sh
python3 -m unittest discover -s pc -p 'test_*.py' -v
```

DBM 소비자 검사는 생산 코드에 모의 DMA 레지스터를 연결하여
A/B 전환, 부분 수신, 지연된 IRQ, 덮어쓰기 감지와 오류 복구를 확인합니다.

```sh
cc -std=c11 -Wall -Wextra -Werror -I tests/dma_stub tests/test_uart_dma.c -o /tmp/bmp_gray_dbm_test
/tmp/bmp_gray_dbm_test
```

모의 검사는 실제 보드에서의 통신 검증을 대신하지 않습니다.

## 10월 20일 2차 발표 필수 Demo

원본 BMP 그대로 저장·복원하는 실행 프로그램입니다.

```sh
python3 /Users/minho/gradproj/bmp_gray_practice/pc/demo_roundtrip.py
```

리셋 후 Flash에서 복원만 확인하려면 `--read-only`를 사용합니다.
CRC32, SHA-256, 전체 바이트 비교와 실제 칩 ID를 JSON으로 기록합니다.
발표 자료와 대본은 `presentation_oct20/output/`에 있습니다.

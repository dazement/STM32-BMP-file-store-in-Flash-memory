#!/usr/bin/env bash
# 테스트 → 빌드 → 다운로드 → 보드 검증 → Flash 저장 증명을 한 번에 실행합니다.
# 사용법: ./run_all.sh   (VS Code Serial Monitor를 닫고 실행)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

CUBE="$HOME/Library/Application Support/stm32cube/bundles"
CMAKE="$CUBE/cmake/4.4.0+st.1/CMake.app/Contents/bin/cmake"
NINJA="$CUBE/ninja/1.13.2+st.1/bin/ninja"
PROG="$CUBE/programmer/2.23.0/bin/STM32_Programmer_CLI"
LOADER="$CUBE/programmer/2.23.0/bin/ExternalLoader/W25Q128JVEIQ_STM32F746G-DISCO.stldr"
export PATH="$CUBE/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH"

BUILD="$ROOT/build/cli"              # VS Code가 쓰는 build/Debug와 분리
ELF="$BUILD/bmp_gray_practice.elf"
INPUT="$ROOT/pc/images/test_1024x1024.bmp"
TMP="$(mktemp -d)"
LOG="$TMP/programmer.log"
trap 'rm -rf "$TMP"' EXIT
START=$SECONDS

step() { printf '\n\033[1;36m[%s] %s\033[0m\n' "$1" "$2"; }
ok()   { printf '\033[1;32mPASS\033[0m %s\n' "$1"; }
fail() { printf '\033[1;31mFAIL\033[0m %s\n' "$1"; exit 1; }

# STM32_Programmer_CLI 출력은 길어서 로그로 보내고, 실패할 때만 보여 줍니다.
prog() {
  if ! "$PROG" "$@" >"$LOG" 2>&1; then
    sed -E 's/\x1b\[[0-9;]*m//g' "$LOG" | tail -20
    fail "STM32_Programmer_CLI $*"
  fi
}

for tool in "$CMAKE" "$NINJA" "$PROG" "$LOADER" "$(command -v arm-none-eabi-gcc || true)"; do
  [[ -n "$tool" && -e "$tool" ]] || fail "도구를 찾을 수 없습니다: ${tool:-arm-none-eabi-gcc}"
done
if lsof /dev/cu.usbmodem* /dev/tty.usbmodem* >/dev/null 2>&1; then
  fail "시리얼 포트를 다른 프로그램이 사용 중입니다. VS Code Serial Monitor를 닫으세요."
fi
if pgrep -f "ST-LINK_gdbserver|STM32CubeProgrammer.app|openocd" >/dev/null; then
  fail "ST-LINK를 다른 프로그램이 사용 중입니다. VS Code 디버그 세션(Shift+F5)이나 CubeProgrammer를 종료하세요."
fi

step 1/9 "PC 단위 테스트"
python3 -m unittest discover -s pc -p 'test_*.py'
ok "Python 단위 테스트"

step 2/9 "C 모의 테스트"
cc -std=c11 -Wall -Wextra -Werror -I tests/dma_stub tests/test_uart_dma.c -o "$TMP/t_dma" && "$TMP/t_dma"
cc -std=c11 -Wall -Wextra -Werror -I tests/dma_stub tests/test_uart_tx_dma.c -o "$TMP/t_tx" && "$TMP/t_tx"
cc -std=c11 -Wall -Wextra -Werror -I Core/Inc tests/test_bmp_gray.c Core/Src/bmp_gray.c -o "$TMP/t_gray" && "$TMP/t_gray"
ok "C 모의 테스트 3개"

step 3/9 "펌웨어 빌드"
"$CMAKE" -S . -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug >/dev/null
if ! "$CMAKE" --build "$BUILD" >"$TMP/build.log" 2>&1; then
  tail -30 "$TMP/build.log"
  fail "펌웨어 빌드"
fi
grep -E "warning" "$TMP/build.log" || true
arm-none-eabi-size "$ELF"
ok "$ELF"

step 4/9 "보드에 다운로드"
prog -c port=SWD mode=UR reset=HWrst -d "$ELF" -v -rst
grep -q "Download verified successfully" "$LOG" || fail "다운로드 검증"
sleep 1
ok "다운로드 후 읽어서 대조 일치"

step 5/9 "원본 BMP 저장 → 복원 (CRC32·SHA-256)"
python3 pc/demo_roundtrip.py

step 6/9 "Gray 전체 과정"
python3 pc/bmp_gray.py

step 7/9 "1 KiB 반복 저장·재읽기 12회"
python3 pc/uart_flash_test.py --repeat 12

step 8/9 "증명 1: 펌웨어 없이 ST 외부 로더로 QSPI Flash 칩 직접 덤프"
SIZE=$(stat -f%z "$INPUT")
prog -c port=SWD mode=UR -el "$LOADER" -u 0x90000000 "$SIZE" "$TMP/qspi_dump.bin"
shasum -a 256 "$TMP/qspi_dump.bin" "$INPUT"
cmp "$TMP/qspi_dump.bin" "$INPUT" || fail "Flash 칩 내용이 원본과 다릅니다"
ok "Flash 칩 0x90000000부터 ${SIZE}바이트 = 원본 BMP"

step 9/9 "증명 2: RAM 320 KB 전체를 0xA5로 덮어쓴 뒤 Flash에서만 복원·변환"
prog -c port=SWD mode=UR -fillmemory 0x20000000 size=0x50000 pattern=0xA5A5A5A5 dataWidth=32 \
  -r32 0x20000000 0x10 -r32 0x20027FF0 0x10 -r32 0x2004FFF0 0x10
[[ $(grep -c "A5A5A5A5 A5A5A5A5 A5A5A5A5 A5A5A5A5" "$LOG") -eq 3 ]] || fail "RAM 덮어쓰기 확인"
ok "RAM 처음·중간·끝 모두 0xA5"
prog -c port=SWD mode=HOTPLUG -rst
sleep 1
python3 pc/demo_roundtrip.py --read-only
python3 pc/bmp_gray.py --convert-only

printf '\n\033[1;32m모든 단계 PASS (%d초)\033[0m\n' $((SECONDS - START))

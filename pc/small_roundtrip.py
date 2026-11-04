#!/usr/bin/env python3
"""Small UART -> flash -> UART check: 1 KiB in the chip's last 4 KiB sector."""
import argparse
import sys

from board import TestError, auto_port, open_port, synchronize, transfer


def main(argv=None):
    parser = argparse.ArgumentParser(description="PC → UART → Flash → UART → PC 테스트")
    parser.add_argument("--port", help="생략하면 연결된 STM32 포트를 자동 선택")
    parser.add_argument("--size", type=int, default=1024, help="1~1024, 기본 1024바이트")
    parser.add_argument("--repeat", type=int, default=1, help="반복 횟수, DMA 버퍼 순환 검증은 12회 권장")
    args = parser.parse_args(argv)
    port = None
    try:
        if not 1 <= args.size <= 1024:
            raise TestError("--size는 1~1024 사이여야 합니다.")
        if not 1 <= args.repeat <= 1000:
            raise TestError("--repeat는 1~1000 사이여야 합니다.")
        path = args.port or auto_port()
        print("연결:", path, "(115200 baud)", flush=True)
        port = open_port(path)
        chip_id = synchronize(port)
        print("Flash ID:", chip_id, flush=True)
        if port.rx_mode != "DBM":
            raise TestError("새 Double Buffer 펌웨어를 다운로드하고 ▶ Continue를 누르세요.")
        print("UART 수신: Double Buffer (4 KiB × 2)", flush=True)
        total_seconds = 0.0
        for run in range(args.repeat):
            payload = bytes((i * 73 + 19 + run * 11) & 0xFF for i in range(args.size))
            storage_ms, elapsed = transfer(port, payload)
            total_seconds += elapsed
            print(f"[{run + 1}/{args.repeat}] PASS: {len(payload):,}바이트 모두 일치 "
                  f"(Flash {storage_ms} ms / 전체 {elapsed:.3f} s)", flush=True)
        print(f"PASS: UART DMA → Flash 저장 → Flash 재읽기 → PC, {args.repeat}회 모두 성공")
        print(f"총 검증 데이터: {args.size * args.repeat:,}바이트 / 총시간: {total_seconds:.3f} s")
        return 0
    except (OSError, TestError) as exc:
        print("실패:", exc, file=sys.stderr)
        return 1
    finally:
        if port is not None:
            port.close()


if __name__ == "__main__":
    sys.exit(main())

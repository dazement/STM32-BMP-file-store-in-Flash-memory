#!/usr/bin/env python3
"""Store a >=3 MiB true-color BMP, then compare a complete flash readback."""
import argparse
from pathlib import Path
import sys
import time
import zlib

from bmp_image import ROOT, make_test_bmp, validate_bmp
from board import (TestError, auto_port, change_baud, dma_stats, open_port, read_flash_file,
                   send_file, synchronize)


def main(argv=None):
    parser = argparse.ArgumentParser(description="3 MiB BMP Double Buffer 수신·Flash 저장 검증")
    parser.add_argument("--port", help="생략하면 자동 선택")
    parser.add_argument("--in", dest="input_path", type=Path, help="생략하면 테스트 BMP 자동 생성")
    parser.add_argument("--baud", type=int, choices=(115200, 921600), default=921600)
    args = parser.parse_args(argv)
    port = None
    high_baud = False
    try:
        source = args.input_path or ROOT / "images" / "test_1024x1024.bmp"
        if args.input_path is None and not source.exists():
            make_test_bmp(source)
        data = source.read_bytes()
        validate_bmp(data)
        print(f"입력: {source}\n크기: {len(data):,}바이트", flush=True)
        port = open_port(args.port or auto_port())
        chip = synchronize(port)
        if port.rx_mode != "DBM":
            raise TestError("새 Double Buffer 펌웨어를 다운로드하고 ▶ Continue를 누르세요.")
        print(f"Flash ID: {chip} / 수신: Double Buffer, 4 KiB × 2", flush=True)
        if args.baud != 115200:
            change_baud(port, args.baud)
            high_baud = True
        print(f"UART: {args.baud:,} baud", flush=True)
        start = time.monotonic()
        milliseconds = send_file(port, data)
        destination = ROOT / "results" / (source.stem + "_flash_readback.bmp")
        returned_crc = read_flash_file(port, len(data), destination)
        elapsed = time.monotonic() - start
        if returned_crc != (zlib.crc32(data) & 0xFFFFFFFF) or destination.read_bytes() != data:
            raise TestError("Flash 재읽기 파일이 원본 BMP와 다릅니다.")
        a, b, overruns = dma_stats(port)
        if a == 0 or b == 0 or overruns != 0:
            raise TestError(f"버퍼 전환 확인 실패: A={a}, B={b}, overrun={overruns}")
        print(f"PASS: {len(data):,}바이트 BMP 전체 저장·재읽기 일치")
        print(f"Double Buffer: A {a}회 / B {b}회 / 버퍼 초과 {overruns}회")
        print(f"저장 {milliseconds / 1000:.2f} s / 저장+재읽기 {elapsed:.2f} s")
        print("재읽기 BMP:", destination)
        return 0
    except (OSError, TestError) as exc:
        print("실패:", exc, file=sys.stderr)
        return 1
    finally:
        if port is not None:
            if high_baud:
                try:
                    change_baud(port, 115200)
                except (OSError, TestError):
                    print("다음 실행 전에 보드 RESET을 눌러 UART 속도를 초기화하세요.", file=sys.stderr)
            port.close()


if __name__ == "__main__":
    sys.exit(main())

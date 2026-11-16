#!/usr/bin/env python3
"""Store a >=3 MiB true-color BMP, then compare a complete flash readback."""
import argparse
from pathlib import Path
import sys
import time
import zlib

from bmp_image import ROOT, load_input_bmp
from board import TestError, connect, dma_stats, read_flash_file, send_file


def main(argv=None):
    parser = argparse.ArgumentParser(description="3 MiB BMP Double Buffer 수신·Flash 저장 검증")
    parser.add_argument("--port", help="생략하면 자동 선택")
    parser.add_argument("--in", dest="input_path", type=Path, help="생략하면 테스트 BMP 자동 생성")
    parser.add_argument("--baud", type=int, choices=(115200, 921600), default=921600)
    args = parser.parse_args(argv)
    try:
        source, data = load_input_bmp(args.input_path)
        print(f"입력: {source}\n크기: {len(data):,}바이트", flush=True)
        with connect(args.port, args.baud) as (port, chip):
            print(f"Flash ID: {chip} / 수신: Double Buffer, 4 KiB × 2", flush=True)
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


if __name__ == "__main__":
    sys.exit(main())

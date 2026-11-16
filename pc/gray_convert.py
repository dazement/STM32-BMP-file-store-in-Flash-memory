#!/usr/bin/env python3
"""Full BMP -> UART DBM -> flash -> grayscale -> UART DMA workflow."""
import argparse
import csv
from datetime import datetime
from pathlib import Path
import statistics
import sys
import time
import zlib

from bmp_image import ROOT, gray_reference, load_input_bmp
from board import TestError, connect, dma_stats, read_gray, send_file


CSV_FIELDS = ["timestamp", "mode", "input_file", "input_bytes", "input_crc", "output_bpp",
              "output_bytes", "output_crc", "baud", "store_mcu_ms", "gray_mcu_ms",
              "total_seconds", "buffer_a", "buffer_b", "overruns", "output_file"]


def record_timing(path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    new = not path.exists() or path.stat().st_size == 0
    if not new:
        with path.open(newline="", encoding="utf-8") as source:
            if next(csv.reader(source), None) != CSV_FIELDS:
                raise TestError("기존 CSV 형식이 다릅니다. --csv로 다른 파일을 지정하세요.")
    with path.open("a", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
        if new: writer.writeheader()
        writer.writerow(row)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Flash 재읽기 Gray BMP 변환 및 전체 시간 측정")
    parser.add_argument("--port")
    parser.add_argument("--in", dest="input_path", type=Path)
    parser.add_argument("--baud", type=int, choices=(115200, 921600, 2250000), default=921600)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--convert-only", action="store_true", help="전송 없이 기존 Flash 이미지만 변환")
    parser.add_argument("--csv", type=Path, default=ROOT / "results" / "timings.csv")
    args = parser.parse_args(argv)
    try:
        if not 1 <= args.repeat <= 100:
            raise TestError("--repeat는 1~100 사이여야 합니다.")
        source_path, source = load_input_bmp(args.input_path)
        source_crc = zlib.crc32(source) & 0xFFFFFFFF
        reference = gray_reference(source)
        print(f"입력: {source_path}\n크기: {len(source):,}바이트", flush=True)
        results = []
        with connect(args.port, args.baud) as (port, chip):
            print(f"Flash: {chip} / RX: Double Buffer / TX: DMA", flush=True)
            print(f"UART: {args.baud:,} baud", flush=True)
            for repetition in range(args.repeat):
                print(f"\n[{repetition + 1}/{args.repeat}] 8비트 Gray", flush=True)
                store_ms = 0
                start = time.monotonic()
                if not args.convert_only: store_ms = send_file(port, source)
                destination = ROOT / "results" / f"{source_path.stem}_gray8.bmp"
                gray_ms, last_byte, output_crc = read_gray(port, source, destination, reference)
                total_seconds = last_byte - start
                a, b, overruns = dma_stats(port)
                if overruns or (not args.convert_only and (a == 0 or b == 0)):
                    raise TestError(f"Double Buffer 상태 오류: A={a}, B={b}, overrun={overruns}")
                row = dict(timestamp=datetime.now().isoformat(timespec="seconds"),
                           mode="convert_only" if args.convert_only else "full_pipeline",
                           input_file=str(source_path), input_bytes=len(source), input_crc=f"{source_crc:08X}",
                           output_bpp=8, output_bytes=len(reference), output_crc=f"{output_crc:08X}",
                           baud=args.baud, store_mcu_ms=store_ms, gray_mcu_ms=gray_ms,
                           total_seconds=f"{total_seconds:.6f}",
                           buffer_a=a, buffer_b=b, overruns=overruns, output_file=str(destination))
                record_timing(args.csv, row)
                results.append(total_seconds)
                print("PASS: Flash 원본 CRC 일치 / Gray 변환 결과 일치")
                print(f"입력 {len(source):,}바이트 → Gray {len(reference):,}바이트")
                label = "Flash 단독 변환" if args.convert_only else "입력 전송 시작 → Gray 마지막 바이트 수신"
                print(f"{label}: {total_seconds:.2f} s")
                print("Gray BMP:", destination)
        if args.repeat > 1:
            print(f"\n8비트 Gray {args.repeat}회 측정 중앙값: {statistics.median(results):.2f} s")
        print("측정 CSV:", args.csv)
        return 0
    except (OSError, TestError) as exc:
        print("실패:", exc, file=sys.stderr)
        return 1


if __name__ == "__main__": sys.exit(main())

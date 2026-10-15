#!/usr/bin/env python3
"""Full BMP -> UART DBM -> flash -> grayscale -> UART DMA workflow."""
import argparse
import csv
from datetime import datetime
import os
from pathlib import Path
import statistics
import struct
import sys
import time
import zlib

from bmp_store import (ROOT, change_baud, dma_stats, make_test_bmp, send_file,
                       validate_bmp)
from uart_flash_test import SerialPort, TestError, auto_port, synchronize


def gray_reference(data, bpp=8):
    """Independent PC reference; handles stored row order and row padding."""
    if bpp not in (8, 24) or len(data) < 54 or data[:2] != b"BM":
        raise TestError("지원되지 않는 BMP 또는 출력 형식")
    size, = struct.unpack_from("<I", data, 2)
    offset, = struct.unpack_from("<I", data, 10)
    dib, width, height, planes, source_bpp, compression = struct.unpack_from("<IiiHHI", data, 14)
    if (size != len(data) or dib < 40 or dib > len(data) - 14 or
            not 14 + dib <= offset <= len(data) or width <= 0 or
            height in (0, -2147483648) or planes != 1 or source_bpp != 24 or compression != 0):
        raise TestError("잘못된 True Color BMP 헤더")
    rows = abs(height)
    source_stride = (width * 3 + 3) & ~3
    target_stride = (width * (bpp // 8) + 3) & ~3
    if offset + source_stride * rows > len(data):
        raise TestError("BMP 픽셀 데이터 부족")
    header_size = 1078 if bpp == 8 else 54
    image_size = target_stride * rows
    x_ppm, y_ppm = struct.unpack_from("<ii", data, 38)
    header = struct.pack("<2sIHHI", b"BM", header_size + image_size, 0, 0, header_size)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, bpp, 0,
                          image_size, x_ppm, y_ppm, 256 if bpp == 8 else 0, 0)
    if bpp == 8:
        header += bytes(component for level in range(256) for component in (level, level, level, 0))
    result = bytearray(header + bytes(image_size))
    for row in range(rows):
        source_row = offset + row * source_stride
        target_row = header_size + row * target_stride
        for x in range(width):
            pixel = source_row + x * 3
            blue, green, red = data[pixel:pixel + 3]
            gray = (29 * blue + 150 * green + 77 * red + 128) // 256
            if bpp == 8:
                result[target_row + x] = gray
            else:
                position = target_row + x * 3
                result[position:position + 3] = bytes((gray, gray, gray))
    return bytes(result)


def read_gray(port, source, bpp, destination, reference):
    source_crc = zlib.crc32(source) & 0xFFFFFFFF
    port.write_all(b"UFG1" + struct.pack("<IIB", len(source), source_crc, bpp))
    line = port.read_line(timeout=30)
    if line != f"GRAY DATA {len(reference)} {bpp}":
        raise TestError("MCU: " + line)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".partial")
    count = 0
    crc = 0
    last_percent = -1
    last_byte_time = 0.0
    with partial.open("wb") as output:
        while count < len(reference):
            chunk = port.read_exact(min(4096, len(reference) - count))
            count += len(chunk)
            if count == len(reference):
                last_byte_time = time.monotonic()
            output.write(chunk)
            crc = zlib.crc32(chunk, crc)
            percent = count * 100 // len(reference)
            if percent // 10 != last_percent // 10:
                print(f"  Gray BMP 수신: {percent}%", flush=True)
                last_percent = percent
        # Assignment timer ends when the final BMP byte reaches the PC.
        fields = port.read_line().split()
        if len(fields) != 5 or fields[:2] != ["GRAY", "DONE"]:
            raise TestError("Gray 전송 완료 응답이 올바르지 않습니다.")
        try:
            output_crc = int(fields[2], 16)
            stored_source_crc = int(fields[3], 16)
            gray_ms = int(fields[4])
        except ValueError as exc:
            raise TestError("Gray CRC 응답이 올바르지 않습니다.") from exc
        if output_crc != (crc & 0xFFFFFFFF) or stored_source_crc != source_crc:
            raise TestError("Gray 출력 CRC 또는 Flash 원본 CRC가 다릅니다.")
        output.flush()
        os.fsync(output.fileno())
    if partial.read_bytes() != reference:
        raise TestError("MCU 회색조 결과가 PC 기준 변환과 다릅니다.")
    partial.replace(destination)
    return gray_ms, last_byte_time, output_crc


CSV_FIELDS = ["timestamp", "mode", "input_file", "input_bytes", "input_crc", "output_bpp",
              "output_bytes", "output_crc", "baud", "store_mcu_ms", "gray_mcu_ms",
              "total_seconds", "buffer_a", "buffer_b", "overruns", "output_file"]


def record_timing(path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    new = not path.exists() or path.stat().st_size == 0
    if not new:
        with path.open(newline="") as source:
            if next(csv.reader(source), None) != CSV_FIELDS:
                raise TestError("기존 CSV 형식이 다릅니다. --csv로 다른 파일을 지정하세요.")
    with path.open("a", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_FIELDS)
        if new: writer.writeheader()
        writer.writerow(row)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Flash 재읽기 Gray BMP 변환 및 전체 시간 측정")
    parser.add_argument("--port")
    parser.add_argument("--in", dest="input_path", type=Path)
    parser.add_argument("--format", type=int, choices=(8,), default=8, help="8비트 Gray 출력 고정")
    parser.add_argument("--baud", type=int, choices=(115200, 921600, 2250000), default=921600)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--convert-only", action="store_true", help="전송 없이 기존 Flash 이미지만 변환")
    parser.add_argument("--csv", type=Path, default=ROOT / "results" / "timings.csv")
    args = parser.parse_args(argv)
    port = None
    high_baud = False
    try:
        if not 1 <= args.repeat <= 100:
            raise TestError("--repeat는 1~100 사이여야 합니다.")
        source_path = args.input_path or ROOT / "images" / "test_1024x1024.bmp"
        if args.input_path is None and not source_path.exists(): make_test_bmp(source_path)
        source = source_path.read_bytes()
        validate_bmp(source)
        source_crc = zlib.crc32(source) & 0xFFFFFFFF
        formats = (8,)
        references = {bpp: gray_reference(source, bpp) for bpp in formats}
        print(f"입력: {source_path}\n크기: {len(source):,}바이트", flush=True)
        port = SerialPort(args.port or auto_port())
        chip = synchronize(port)
        if port.rx_mode != "DBM" or "GRAY8" not in port.capabilities:
            raise TestError("새 Gray 변환 펌웨어를 다운로드하고 ▶ Continue를 누르세요.")
        print(f"Flash: {chip} / RX: Double Buffer / TX: DMA", flush=True)
        if args.baud != 115200:
            high_baud = True
            change_baud(port, args.baud)
        print(f"UART: {args.baud:,} baud", flush=True)
        results = {bpp: [] for bpp in formats}
        for repetition in range(args.repeat):
            order = formats if repetition % 2 == 0 else tuple(reversed(formats))
            for bpp in order:
                print(f"\n[{repetition + 1}/{args.repeat}] {bpp}비트 Gray", flush=True)
                store_ms = 0
                start = time.monotonic()
                if not args.convert_only: store_ms = send_file(port, source)
                destination = ROOT / "results" / f"{source_path.stem}_gray{bpp}.bmp"
                gray_ms, last_byte, output_crc = read_gray(port, source, bpp, destination, references[bpp])
                total_seconds = last_byte - start
                a, b, overruns = dma_stats(port)
                if overruns or (not args.convert_only and (a == 0 or b == 0)):
                    raise TestError(f"Double Buffer 상태 오류: A={a}, B={b}, overrun={overruns}")
                row = dict(timestamp=datetime.now().isoformat(timespec="seconds"),
                           mode="convert_only" if args.convert_only else "full_pipeline",
                           input_file=str(source_path), input_bytes=len(source), input_crc=f"{source_crc:08X}",
                           output_bpp=bpp, output_bytes=len(references[bpp]), output_crc=f"{output_crc:08X}",
                           baud=args.baud, store_mcu_ms=store_ms, gray_mcu_ms=gray_ms,
                           total_seconds=f"{total_seconds:.6f}",
                           buffer_a=a, buffer_b=b, overruns=overruns, output_file=str(destination))
                record_timing(args.csv, row)
                results[bpp].append(total_seconds)
                print("PASS: Flash 원본 CRC 일치 / Gray 변환 결과 일치")
                print(f"입력 {len(source):,}바이트 → Gray {len(references[bpp]):,}바이트")
                label = "Flash 단독 변환" if args.convert_only else "입력 전송 시작 → Gray 마지막 바이트 수신"
                print(f"{label}: {total_seconds:.2f} s")
                print("Gray BMP:", destination)
        if args.repeat > 1:
            print(f"\n8비트 Gray {args.repeat}회 측정 중앙값: {statistics.median(results[8]):.2f} s")
        print("측정 CSV:", args.csv)
        return 0
    except (OSError, TestError) as exc:
        print("실패:", exc, file=sys.stderr)
        return 1
    finally:
        if port is not None:
            if high_baud:
                try: change_baud(port, 115200)
                except (OSError, TestError):
                    print("다음 실행 전에 보드 RESET을 눌러 UART 속도를 초기화하세요.", file=sys.stderr)
            port.close()


if __name__ == "__main__": sys.exit(main())

#!/usr/bin/env python3
"""Store a >=3 MiB true-color BMP, then compare a complete flash readback."""
import argparse
from collections import deque
import os
from pathlib import Path
import struct
import sys
import time
import zlib

from uart_flash_test import SerialPort, TestError, auto_port, synchronize

ROOT = Path(__file__).resolve().parent
IMAGE_LIMIT = 0x3F0000
CHUNK_BYTES = 4096


def make_test_bmp(path):
    width = height = 1024
    stride = (width * 3 + 3) & ~3
    image_size = stride * height
    header = struct.pack("<2sIHHI", b"BM", image_size + 54, 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                          image_size, 2835, 2835, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(header)
        for y in range(height):
            row = bytearray(stride)
            for x in range(width):
                row[x * 3:x * 3 + 3] = bytes((((x + y) >> 3) & 255,
                                               (y >> 2) & 255, (x >> 2) & 255))
            output.write(row)


def validate_bmp(data):
    if not 3 * 1024 * 1024 <= len(data) <= IMAGE_LIMIT:
        raise TestError("입력 BMP는 3 MiB 이상, 4,128,768바이트 이하여야 합니다.")
    if len(data) < 54 or data[:2] != b"BM":
        raise TestError("BMP 파일이 아닙니다.")
    declared = struct.unpack_from("<I", data, 2)[0]
    offset = struct.unpack_from("<I", data, 10)[0]
    dib, width, height, planes, bpp, compression = struct.unpack_from("<IiiHHI", data, 14)
    if declared != len(data) or dib < 40 or offset < 14 + dib or offset > len(data):
        raise TestError("BMP 헤더 또는 파일 크기가 올바르지 않습니다.")
    if width <= 0 or height == 0 or planes != 1 or bpp != 24 or compression != 0:
        raise TestError("이번 단계는 압축되지 않은 24비트 True Color BMP를 사용합니다.")
    stride = (width * 3 + 3) & ~3
    if offset + stride * abs(height) > len(data):
        raise TestError("BMP 픽셀 데이터가 부족합니다.")


def change_baud(port, baud):
    port.write_all(b"UFS1" + struct.pack("<I", baud))
    line = port.read_line()
    if line != f"BAUD {baud}":
        raise TestError("속도 변경 실패: " + line)
    port.set_baud(baud)
    time.sleep(0.08)
    synchronize(port)


def read_flash_file(port, length, destination):
    port.write_all(b"UFR1" + struct.pack("<I", length))
    line = port.read_line()
    if line != f"FILE DATA {length}":
        raise TestError("MCU: " + line)
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".partial")
    crc = 0
    done = 0
    last_percent = -1
    with partial.open("wb") as output:
        while done < length:
            chunk = port.read_exact(min(CHUNK_BYTES, length - done))
            output.write(chunk)
            crc = zlib.crc32(chunk, crc)
            done += len(chunk)
            percent = done * 100 // length
            if percent // 10 != last_percent // 10:
                print(f"  Flash 재읽기: {percent}%", flush=True)
                last_percent = percent
        line = port.read_line()
        if line != f"FILE DATA DONE {crc & 0xFFFFFFFF:08X}":
            raise TestError("Flash 읽기 CRC가 다릅니다: " + line)
        output.flush()
        os.fsync(output.fileno())
    partial.replace(destination)
    return crc & 0xFFFFFFFF


def send_file(port, data):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    port.write_all(b"UFL1" + struct.pack("<II", len(data), crc))
    line = port.read_line()
    if line != "FILE READY":
        raise TestError("MCU: " + line)
    pending = deque()
    sent = 0
    last_percent = -1
    while sent < len(data) or pending:
        # Two chunks in flight: receive buffer B can fill while A is stored.
        while sent < len(data) and len(pending) < 2:
            chunk = data[sent:sent + CHUNK_BYTES]
            port.write_all(struct.pack("<I", zlib.crc32(chunk) & 0xFFFFFFFF) + chunk)
            sent += len(chunk)
            pending.append(sent)
        line = port.read_line(timeout=8)
        expected = pending.popleft()
        if line != f"ACK {expected}":
            raise TestError("MCU 저장 응답 오류: " + line)
        percent = expected * 100 // len(data)
        if percent // 10 != last_percent // 10:
            print(f"  BMP 저장: {percent}%", flush=True)
            last_percent = percent
    line = port.read_line()
    fields = line.split()
    if len(fields) != 5 or fields[:2] != ["FILE", "STORED"]:
        raise TestError("파일 저장 실패: " + line)
    try:
        size, stored_crc, milliseconds = int(fields[2]), int(fields[3], 16), int(fields[4])
    except ValueError as exc:
        raise TestError("파일 저장 응답이 올바르지 않습니다.") from exc
    if size != len(data) or stored_crc != crc:
        raise TestError("전체 BMP 저장 CRC 또는 크기가 다릅니다.")
    return milliseconds


def dma_stats(port):
    port.write_all(b"!")
    fields = port.read_line().split()
    if len(fields) != 5 or fields[:2] != ["DBM", "STATS"]:
        raise TestError("Double Buffer 상태 응답이 올바르지 않습니다.")
    try:
        return tuple(int(value) for value in fields[2:])
    except ValueError as exc:
        raise TestError("잘못된 Double Buffer 통계") from exc


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
        port = SerialPort(args.port or auto_port())
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

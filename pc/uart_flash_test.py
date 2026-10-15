#!/usr/bin/env python3
"""Small UART -> flash -> UART check for macOS/Linux; standard library only."""

import argparse
import glob
import os
import select
import struct
import sys
import time
import zlib


class TestError(Exception):
    pass


class SerialPort:
    def __init__(self, path):
        import termios

        self.termios = termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.original = None
        try:
            self.original = termios.tcgetattr(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[3] = 0
            attrs[4] = termios.B115200
            attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIFLUSH)
        except Exception:
            self.close()
            raise

    def close(self):
        if self.fd is not None:
            try:
                if self.original is not None:
                    self.termios.tcsetattr(self.fd, self.termios.TCSANOW, self.original)
            finally:
                os.close(self.fd)
                self.fd = None

    def set_baud(self, baud):
        import fcntl

        if sys.platform == "darwin":
            # Apple IOSSIOSPEED_64: _IOW('T', 2, user_speed_t).
            fcntl.ioctl(self.fd, 0x80085402, struct.pack("=Q", baud))
        else:
            speed = getattr(self.termios, "B" + str(baud), None)
            if speed is None:
                raise TestError("지원되지 않는 UART 속도: " + str(baud))
            attrs = self.termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = speed
            self.termios.tcsetattr(self.fd, self.termios.TCSANOW, attrs)

    def write_all(self, data, timeout=3.0):
        deadline = time.monotonic() + timeout
        offset = 0
        while offset < len(data):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TestError("UART 전송 시간 초과")
            try:
                count = os.write(self.fd, data[offset:])
            except BlockingIOError:
                continue
            if count <= 0:
                raise TestError("UART 연결이 끊겼습니다.")
            offset += count

    def read_exact(self, size, timeout=5.0):
        deadline = time.monotonic() + timeout
        result = bytearray()
        while len(result) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise TestError("보드 응답 시간 초과: ▶ Continue와 시리얼 모니터 종료를 확인하세요.")
            try:
                chunk = os.read(self.fd, size - len(result))
            except BlockingIOError:
                continue
            if not chunk:
                raise TestError("UART 연결이 끊겼습니다.")
            result.extend(chunk)
        return bytes(result)

    def read_line(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        result = bytearray()
        while len(result) < 256:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TestError("보드 응답 시간 초과")
            byte = self.read_exact(1, remaining)
            if byte == b"\n":
                return result.decode("ascii", errors="replace").strip()
            result.extend(byte)
        raise TestError("예상하지 못한 응답: UART 속도 115200을 확인하세요.")


def auto_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        ports = sorted(glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        raise TestError("보드 포트를 하나로 선택할 수 없습니다. --port /dev/cu.usbmodem… 을 지정하세요.")
    return ports[0]


def synchronize(port):
    port.write_all(b"?")
    deadline = time.monotonic() + 3.0
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TestError("새 UART 테스트 펌웨어를 다운로드하고 ▶ Continue를 누르세요.")
        line = port.read_line(remaining)
        if line.startswith("UFT READY "):
            fields = line.split()
            chip_id = fields[2]
            if chip_id not in ("EF4018", "EF4016"):
                raise TestError("지원되지 않는 Flash ID: " + chip_id)
            port.rx_mode = fields[3] if len(fields) > 3 else "POLLING"
            port.capabilities = tuple(fields[4:])
            return chip_id


def transfer(port, payload):
    if not 1 <= len(payload) <= 1024:
        raise TestError("이번 단계의 데이터 크기는 1~1024바이트입니다.")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    start = time.monotonic()
    port.write_all(b"UFT1" + struct.pack("<II", len(payload), crc))
    line = port.read_line()
    if line != "READY":
        raise TestError("MCU: " + line)
    port.write_all(payload)
    line = port.read_line()
    if line.startswith("ERROR "):
        raise TestError("MCU: " + line)
    fields = line.split()
    if len(fields) != 4 or fields[0] != "STORED":
        raise TestError("예상하지 못한 MCU 응답: " + line)
    try:
        stored_length = int(fields[1])
        stored_crc = int(fields[2], 16)
        storage_ms = int(fields[3])
    except ValueError as exc:
        raise TestError("잘못된 MCU 응답: " + line) from exc
    if stored_length != len(payload) or stored_crc != crc:
        raise TestError("MCU 저장 길이 또는 CRC가 원본과 다릅니다.")
    returned = port.read_exact(stored_length)
    if returned != payload or (zlib.crc32(returned) & 0xFFFFFFFF) != crc:
        raise TestError("Flash에서 돌아온 데이터가 PC 원본과 다릅니다.")
    return storage_ms, time.monotonic() - start


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
        if os.name != "posix":
            raise TestError("이 프로그램은 macOS/Linux용입니다.")
        path = args.port or auto_port()
        print("연결:", path, "(115200 baud)", flush=True)
        port = SerialPort(path)
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

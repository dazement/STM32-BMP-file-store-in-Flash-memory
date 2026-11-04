"""Serial link to the board and the PC side of every UART command.

macOS/Linux need only the standard library (termios). Windows needs pyserial;
when pyserial is installed it is used on every OS.
"""
from collections import deque
import glob
import os
import select
import struct
import sys
import time
import zlib


class TestError(Exception):
    pass


class LineReader:
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


class PosixSerialPort(LineReader):
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


class PySerialPort(LineReader):
    POLL_SECONDS = 0.05

    def __init__(self, path, serial):
        self.timeout_error = serial.SerialTimeoutException
        # Fixed timeouts: pyserial reconfigures the port whenever they change.
        self.serial = serial.Serial(path, 115200, timeout=self.POLL_SECONDS, write_timeout=3.0)
        self.serial.reset_input_buffer()

    def close(self):
        if self.serial is not None:
            self.serial.close()
            self.serial = None

    def set_baud(self, baud):
        self.serial.baudrate = baud

    def write_all(self, data, timeout=3.0):
        if self.serial.write_timeout != timeout:
            self.serial.write_timeout = timeout
        try:
            count = self.serial.write(data)
        except self.timeout_error as exc:
            raise TestError("UART 전송 시간 초과") from exc
        if count != len(data):
            raise TestError("UART 전송 시간 초과")

    def read_exact(self, size, timeout=5.0):
        deadline = time.monotonic() + timeout
        result = bytearray()
        while len(result) < size:
            if time.monotonic() >= deadline:
                raise TestError("보드 응답 시간 초과: ▶ Continue와 시리얼 모니터 종료를 확인하세요.")
            result.extend(self.serial.read(size - len(result)))
        return bytes(result)


def open_port(path):
    try:
        import serial
    except ImportError:
        if os.name != "posix":
            raise TestError("Windows에서는 pyserial이 필요합니다: py -m pip install pyserial") from None
        return PosixSerialPort(path)
    return PySerialPort(path, serial)


ST_LINK_VID = 0x0483


def auto_port():
    ports = []
    try:
        from serial.tools import list_ports
        ports = sorted(p.device for p in list_ports.comports() if p.vid == ST_LINK_VID)
    except ImportError:
        pass
    if not ports:
        ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))
    if len(ports) != 1:
        raise TestError("보드 포트를 하나로 선택할 수 없습니다. "
                        "--port로 지정하세요 (macOS /dev/cu.usbmodem…, Windows COM5 등).")
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


CHUNK_BYTES = 4096


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


def read_gray(port, source, destination, reference):
    source_crc = zlib.crc32(source) & 0xFFFFFFFF
    # The final byte is the output depth; the firmware accepts only 8.
    port.write_all(b"UFG1" + struct.pack("<IIB", len(source), source_crc, 8))
    line = port.read_line(timeout=30)
    if line != f"GRAY DATA {len(reference)} 8":
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

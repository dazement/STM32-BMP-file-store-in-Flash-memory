#!/usr/bin/env python3
"""PC <-> STM32 BMP 전송 도구 (프로토콜: firmware/Core/Inc/bmp_link.h)

    python bmp_transfer.py --port COM5 all color.bmp gray.bmp   # 업로드 -> 변환/다운로드 -> 검증
    python bmp_transfer.py --port COM5 ping
    python bmp_transfer.py --port COM5 upload color.bmp
    python bmp_transfer.py --port COM5 download gray.bmp [--ref color.bmp]

필요 패키지: pyserial  (pip install pyserial)   / 검증 교차확인용(선택): pillow
"""
import argparse
import binascii
import struct
import sys
import time

import serial

import verify_gray_bmp

SOF_TX, SOF_RX = 0xA5, 0x5A
MAX_PAYLOAD = 4096
T_PING, T_START, T_DATA, T_END, T_CONVERT = 0x01, 0x02, 0x03, 0x04, 0x05
T_ACK, T_NAK, T_GRAY_DATA, T_GRAY_END = 0x80, 0x81, 0x84, 0x85

STATUS = {0: 'OK', 1: 'BAD_CRC', 2: 'BAD_SEQ', 3: 'TOO_BIG', 4: 'FLASH_ERR', 5: 'VERIFY_ERR',
          6: 'NOT_BMP', 7: 'UNSUPPORTED', 8: 'BAD_STATE', 9: 'BAD_LEN', 10: 'UNKNOWN_CMD'}


def crc16(b):
    return binascii.crc_hqx(b, 0xFFFF)          # CRC-16/CCITT-FALSE


class LinkError(Exception):
    pass


class Link:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.ser.reset_input_buffer()
        self.retries = 0

    def close(self):
        self.ser.close()

    # ---------------------------------------------------------------- frames
    def send(self, ftype, seq, payload=b''):
        body = struct.pack('<BHH', ftype, seq, len(payload)) + payload
        self.ser.write(bytes([SOF_TX]) + body + struct.pack('<H', crc16(body)))

    def _read_exact(self, n, deadline):
        buf = bytearray()
        while len(buf) < n:
            if time.monotonic() > deadline:
                return None
            buf += self.ser.read(n - len(buf))
        return bytes(buf)

    def recv(self, timeout):
        """-> (type, seq, payload) | 'crc' (손상) | None (타임아웃)"""
        deadline = time.monotonic() + timeout
        while True:
            b = self._read_exact(1, deadline)
            if b is None:
                return None
            if b[0] == SOF_RX:
                break
        h = self._read_exact(5, time.monotonic() + 0.5)
        if h is None:
            return 'crc'
        ftype, seq, ln = struct.unpack('<BHH', h)
        if ln > MAX_PAYLOAD:
            return 'crc'
        rest = self._read_exact(ln + 2, time.monotonic() + 0.5 + ln / 5000)
        if rest is None:
            return 'crc'
        payload, (crc,) = rest[:ln], struct.unpack('<H', rest[ln:])
        if crc16(h + payload) != crc:
            return 'crc'
        return ftype, seq, payload

    def flush(self):
        time.sleep(0.05)
        self.ser.reset_input_buffer()

    def request(self, ftype, seq, payload=b'', timeout=2.0, tries=10):
        """명령 전송 후 같은 seq 의 ACK 대기. 실패/손상 시 재전송. -> (status, extra)"""
        for _ in range(tries):
            self.send(ftype, seq, payload)
            while True:
                r = self.recv(timeout)
                if r is None or r == 'crc' or r[0] == T_NAK:
                    break                                  # 재전송
                if r[0] == T_ACK and r[1] == seq:
                    return r[2][0], r[2][1:]
                # 다른 seq 의 늦은 ACK 등은 무시하고 계속 대기
            self.retries += 1
            self.flush()
        raise LinkError(f'응답 없음 (type=0x{ftype:02X}, seq={seq})')

    # -------------------------------------------------------------- commands
    def ping(self):
        st, ex = self.request(T_PING, 0, timeout=1.0, tries=3)
        jedec, fsize, maxp, ver = struct.unpack('<IIHB', ex[:11])
        return jedec, fsize, maxp, ver

    def upload(self, data):
        t0 = time.monotonic()
        print(f'[1/3] Flash erase 중... ({len(data):,} bytes, 수 초 소요)')
        st, _ = self.request(T_START, 0, struct.pack('<I', len(data)), timeout=200.0, tries=2)
        if st:
            raise LinkError(f'START 실패: {STATUS.get(st, st)}')

        nframes = (len(data) + MAX_PAYLOAD - 1) // MAX_PAYLOAD
        t1 = time.monotonic()
        for seq in range(nframes):
            chunk = data[seq * MAX_PAYLOAD:(seq + 1) * MAX_PAYLOAD]
            st, ex = self.request(T_DATA, seq, chunk)
            if st:
                raise LinkError(f'DATA seq={seq} 실패: {STATUS.get(st, st)} {ex.hex()}')
            if seq % 16 == 0 or seq == nframes - 1:
                done = min((seq + 1) * MAX_PAYLOAD, len(data))
                rate = done / max(time.monotonic() - t1, 1e-6) / 1024
                print(f'\r      업로드 {done * 100 // len(data):3d}%  {done:,}/{len(data):,}  {rate:6.1f} KB/s',
                      end='', flush=True)
        print()

        crc = binascii.crc32(data) & 0xFFFFFFFF
        st, ex = self.request(T_END, nframes, struct.pack('<I', crc), timeout=30.0)
        fcrc = struct.unpack('<I', ex[:4])[0] if len(ex) >= 4 else None
        if st:
            raise LinkError(f'Flash 검증 실패: {STATUS.get(st, st)} (PC crc={crc:08X}, flash crc={fcrc})')
        print(f'      Flash 저장 + read-back CRC32 일치 ({crc:08X}), {time.monotonic() - t0:.1f}s')

    def download(self):
        t0 = time.monotonic()
        print('[2/3] Flash -> Gray 변환 + 다운로드...')
        st, ex = self.request(T_CONVERT, 0, timeout=5.0, tries=3)
        if st:
            raise LinkError(f'CONVERT 실패: {STATUS.get(st, st)}')
        size, w, h, bpp = struct.unpack('<IiiH', ex[:14])
        print(f'      원본 {w}x{h} {bpp}bpp -> Gray 8bpp {size:,} bytes')

        out = bytearray()
        expect = 0
        while True:
            r = self.recv(1.5)
            if r is None or r == 'crc':
                self.retries += 1
                if r is None and self.retries > 200:
                    raise LinkError('다운로드 타임아웃')
                self.flush()
                self.send(T_NAK, expect, b'\x01')
                continue
            ftype, seq, payload = r
            if seq == (expect - 1) & 0xFFFF and expect > 0:      # 중복 (ACK 유실) -> 재-ACK
                self.send(T_ACK, seq, b'\x00')
                continue
            if seq != expect:
                self.send(T_NAK, expect, b'\x02')
                continue
            self.send(T_ACK, seq, b'\x00')
            expect += 1
            if ftype == T_GRAY_DATA:
                out += payload
                if seq % 16 == 0 or len(out) >= size:
                    rate = len(out) / max(time.monotonic() - t0, 1e-6) / 1024
                    print(f'\r      다운로드 {len(out) * 100 // size:3d}%  {len(out):,}/{size:,}  {rate:6.1f} KB/s',
                          end='', flush=True)
            elif ftype == T_GRAY_END:
                print()
                crc = struct.unpack('<I', payload)[0]
                mine = binascii.crc32(out) & 0xFFFFFFFF
                if len(out) != size or crc != mine:
                    raise LinkError(f'다운로드 손상: size {len(out)}/{size}, crc {mine:08X}/{crc:08X}')
                # MCU 가 마지막 ACK 를 못 받았을 경우를 대비해 잠시 재-ACK 처리
                end = time.monotonic() + 0.3
                while time.monotonic() < end:
                    r = self.recv(0.1)
                    if r and r != 'crc':
                        self.send(T_ACK, r[1], b'\x00')
                print(f'      CRC32 일치 ({crc:08X}), {time.monotonic() - t0:.1f}s')
                return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True, help='COM5, /dev/ttyACM0, /dev/tty.usbmodemXXXX ...')
    ap.add_argument('--baud', type=int, default=921600)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('ping')
    p = sub.add_parser('upload'); p.add_argument('color')
    p = sub.add_parser('download'); p.add_argument('gray'); p.add_argument('--ref')
    p = sub.add_parser('all'); p.add_argument('color'); p.add_argument('gray')
    p.add_argument('--png', help='Gray 미리보기 PNG 저장 (Pillow 필요)')
    a = ap.parse_args()

    link = Link(a.port, a.baud)
    try:
        jedec, fsize, maxp, ver = link.ping()
        print(f'MCU 연결됨: JEDEC ID {jedec:06X}'
              f"{' (W25Q32)' if jedec in (0xEF4016, 0xEF7016) else ' (!! W25Q32 아님 - 배선 확인)'}, "
              f'저장 가능 {fsize:,} bytes, proto v{ver}')
        if a.cmd == 'ping':
            return 0

        if a.cmd in ('upload', 'all'):
            data = open(a.color, 'rb').read()
            info = verify_gray_bmp.parse_bmp(data)
            if info['bpp'] not in (24, 32) or info['comp'] != 0:
                raise LinkError(f"True Color BMP 가 아님 (bpp={info['bpp']}, compression={info['comp']})")
            if len(data) > fsize:
                raise LinkError(f'파일이 Flash 용량보다 큼: {len(data):,} > {fsize:,}')
            print(f"원본: {a.color} {info['w']}x{info['h']} {info['bpp']}bpp, {len(data):,} bytes")
            link.upload(data)

        if a.cmd in ('download', 'all'):
            gray = link.download()
            with open(a.gray, 'wb') as f:
                f.write(gray)
            print(f'      저장: {a.gray}')
            print('[3/3] Gray BMP 검증')
            ref = a.color if a.cmd == 'all' else a.ref
            ok = verify_gray_bmp.verify(a.gray, ref, getattr(a, 'png', None))
            if link.retries:
                print(f'(재전송 {link.retries}회 발생 - 통신 오류가 있었지만 복구됨)')
            return 0 if ok else 1
        return 0
    except (LinkError, verify_gray_bmp.BmpError) as e:
        print(f'\n오류: {e}', file=sys.stderr)
        return 1
    finally:
        link.close()


if __name__ == '__main__':
    sys.exit(main())

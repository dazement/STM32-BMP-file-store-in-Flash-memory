#!/usr/bin/env python3
"""테스트용 24-bit True Color BMP 생성 (기본 1024x1024 = 3,145,782 bytes ≈ 3 MB).

    python make_test_bmp.py color.bmp [--width 1024 --height 1024]
"""
import argparse
import math
import struct


def make_bmp(width, height):
    stride = (width * 3 + 3) & ~3
    img_size = stride * height
    off = 54
    hdr = struct.pack('<2sIHHI', b'BM', off + img_size, 0, 0, off)
    hdr += struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, img_size, 2835, 2835, 0, 0)

    data = bytearray(img_size)
    cx, cy, rad = width / 2, height / 2, min(width, height) * 0.35
    for y in range(height):                       # BMP 는 bottom-up: 첫 행이 맨 아래
        row = (height - 1 - y)
        base = y * stride
        for x in range(width):
            if row < height // 6:                 # 상단: 8색 컬러바
                bar = x * 8 // width
                r, g, b = (255 * ((bar >> 2) & 1), 255 * ((bar >> 1) & 1), 255 * (bar & 1))
            elif row > height * 5 // 6:           # 하단: 회색 램프
                r = g = b = x * 255 // (width - 1)
            else:                                 # 가운데: RGB 그라디언트 + 원
                r = x * 255 // (width - 1)
                g = row * 255 // (height - 1)
                b = 255 - r
                d = math.hypot(x - cx, row - cy)
                if abs(d - rad) < 6:
                    r, g, b = 255, 255, 255
            i = base + x * 3
            data[i] = b; data[i + 1] = g; data[i + 2] = r
    return hdr + bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--width', type=int, default=1024)
    ap.add_argument('--height', type=int, default=1024)
    a = ap.parse_args()
    bmp = make_bmp(a.width, a.height)
    with open(a.out, 'wb') as f:
        f.write(bmp)
    print(f'{a.out}: {a.width}x{a.height} 24bpp, {len(bmp):,} bytes ({len(bmp) / 2**20:.2f} MiB)')


if __name__ == '__main__':
    main()

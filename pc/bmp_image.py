"""BMP files on the PC: the test image, input checks and the reference Gray conversion."""
from pathlib import Path
import struct

from board import TestError


ROOT = Path(__file__).resolve().parent


IMAGE_LIMIT = 0x3F0000


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


def gray_reference(data):
    """Independent PC reference; handles stored row order and row padding."""
    if len(data) < 54 or data[:2] != b"BM":
        raise TestError("지원되지 않는 BMP")
    size, = struct.unpack_from("<I", data, 2)
    offset, = struct.unpack_from("<I", data, 10)
    dib, width, height, planes, source_bpp, compression = struct.unpack_from("<IiiHHI", data, 14)
    if (size != len(data) or dib < 40 or dib > len(data) - 14 or
            not 14 + dib <= offset <= len(data) or width <= 0 or
            height in (0, -2147483648) or planes != 1 or source_bpp != 24 or compression != 0):
        raise TestError("잘못된 True Color BMP 헤더")
    rows = abs(height)
    source_stride = (width * 3 + 3) & ~3
    target_stride = (width + 3) & ~3
    if offset + source_stride * rows > len(data):
        raise TestError("BMP 픽셀 데이터 부족")
    header_size = 1078
    image_size = target_stride * rows
    x_ppm, y_ppm = struct.unpack_from("<ii", data, 38)
    header = struct.pack("<2sIHHI", b"BM", header_size + image_size, 0, 0, header_size)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0,
                          image_size, x_ppm, y_ppm, 256, 0)
    header += bytes(component for level in range(256) for component in (level, level, level, 0))
    result = bytearray(header + bytes(image_size))
    for row in range(rows):
        source_row = offset + row * source_stride
        target_row = header_size + row * target_stride
        for x in range(width):
            pixel = source_row + x * 3
            blue, green, red = data[pixel:pixel + 3]
            result[target_row + x] = (29 * blue + 150 * green + 77 * red + 128) // 256
    return bytes(result)

from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from bmp_gray import TestError, gray_reference, read_gray
from test_bmp_store import StreamPort


def tiny_bmp(width=3, height=2, gap=0):
    stride = (width * 3 + 3) & ~3
    rows = abs(height)
    offset = 54 + gap
    header = struct.pack("<2sIHHI", b"BM", offset + stride * rows, 0, 0, offset)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, stride * rows, 2835, 2835, 0, 0)
    data = bytearray(header + bytes(gap) + b"\xEE" * (stride * rows))
    for row in range(rows):
        for x in range(width):
            at = offset + row * stride + x * 3
            data[at:at + 3] = bytes(((row * 55 + x * 19) & 255, 150, 77))
    return bytes(data)


class GrayTests(unittest.TestCase):
    def test_padding_and_top_down(self):
        for width in (1, 2, 3, 4, 5, 1025):
            for height in (2, -2):
                source = tiny_bmp(width, height, gap=12)
                for bpp in (8, 24):
                    result = gray_reference(source, bpp)
                    offset = struct.unpack_from("<I", result, 10)[0]
                    stride = (width * (bpp // 8) + 3) & ~3
                    self.assertEqual(struct.unpack_from("<i", result, 22)[0], height)
                    self.assertEqual(len(result), offset + stride * 2)
                    for row in range(2):
                        pad = result[offset + row * stride + width * (bpp // 8):offset + (row + 1) * stride]
                        self.assertEqual(pad, bytes(len(pad)))
                    if bpp == 8:
                        self.assertEqual(result[54:58], b"\0\0\0\0")
                        self.assertEqual(result[1074:1078], b"\xff\xff\xff\0")

    def test_output_and_flash_crc_checks(self):
        source = tiny_bmp()
        result = gray_reference(source, 8)
        source_crc = zlib.crc32(source)
        crc = zlib.crc32(result)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "gray.bmp"
            port = StreamPort([f"GRAY DATA {len(result)} 8", f"GRAY DONE {crc:08X} {source_crc:08X} 42"], result)
            ms, _, returned_crc = read_gray(port, source, 8, destination, result)
            self.assertEqual((ms, returned_crc), (42, crc))
            self.assertEqual(destination.read_bytes(), result)
            destination.unlink()
            port = StreamPort([f"GRAY DATA {len(result)} 8", f"GRAY DONE {crc:08X} 00000000 42"], result)
            with self.assertRaises(TestError): read_gray(port, source, 8, destination, result)
            self.assertFalse(destination.exists())

    def test_wrong_pixels_even_with_valid_crc_rejected(self):
        source = tiny_bmp()
        result = gray_reference(source, 8)
        wrong = bytearray(result); wrong[-4] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "gray.bmp"
            port = StreamPort([f"GRAY DATA {len(result)} 8",
                               f"GRAY DONE {zlib.crc32(wrong):08X} {zlib.crc32(source):08X} 1"], bytes(wrong))
            with self.assertRaises(TestError): read_gray(port, source, 8, destination, result)
            self.assertFalse(destination.exists())

    def test_malformed_input(self):
        source = tiny_bmp()
        for offset, value in ((10, 0xFFFFFFFF), (14, 0xFFFFFFFF), (18, 0x7FFFFFFF), (22, 0x80000000), (30, 1)):
            broken = bytearray(source); struct.pack_into("<I", broken, offset, value)
            with self.assertRaises(TestError): gray_reference(broken)


if __name__ == "__main__": unittest.main()

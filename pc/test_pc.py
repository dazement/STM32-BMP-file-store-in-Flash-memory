"""PC-side checks without a board: serial protocol, BMP store and readback, Gray reference."""
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from bmp_image import gray_reference, make_test_bmp, validate_bmp
from board import (CHUNK_BYTES, TestError, read_flash_file, read_gray, send_file, synchronize,
                   transfer)


class FakePort:
    def __init__(self, lines, returned=b""):
        self.lines = iter(lines)
        self.returned = returned
        self.writes = []

    def write_all(self, data):
        self.writes.append(data)

    def read_line(self, timeout=5):
        return next(self.lines)

    def read_exact(self, count):
        if len(self.returned) != count:
            raise TestError("truncated readback")
        return self.returned


class ProtocolTests(unittest.TestCase):
    def test_all_binary_values_round_trip(self):
        payload = bytes(range(256)) * 4
        crc = zlib.crc32(payload)
        port = FakePort(["READY", f"STORED 1024 {crc:08X} 48"], payload)
        storage_ms, _ = transfer(port, payload)
        self.assertEqual(storage_ms, 48)
        self.assertEqual(port.writes, [b"UFT1" + struct.pack("<II", 1024, crc), payload])

    def test_corrupted_readback_rejected(self):
        payload = b"abcd"
        port = FakePort(["READY", f"STORED 4 {zlib.crc32(payload):08X} 1"], b"abce")
        with self.assertRaises(TestError):
            transfer(port, payload)

    def test_foreign_flash_contents_error_propagated(self):
        port = FakePort(["READY", "ERROR existing sector data preserved"])
        with self.assertRaisesRegex(TestError, "preserved"):
            transfer(port, b"test")

    def test_wrong_stored_length_rejected(self):
        port = FakePort(["READY", "STORED 1025 00000000 1"])
        with self.assertRaises(TestError):
            transfer(port, b"test")

    def test_invalid_sizes_send_nothing(self):
        for payload in (b"", bytes(1025)):
            port = FakePort([])
            with self.assertRaises(TestError):
                transfer(port, payload)
            self.assertEqual(port.writes, [])

    def test_sync_skips_boot_message(self):
        port = FakePort(["UART Flash test ready.", "UFT READY EF4018"])
        self.assertEqual(synchronize(port), "EF4018")
        self.assertEqual(port.writes, [b"?"])

    def test_unknown_chip_blocks_transfer(self):
        with self.assertRaises(TestError):
            synchronize(FakePort(["UFT READY 20BA18"]))

    def test_dma_firmware_identified(self):
        port = FakePort(["UFT READY EF4018 DMA"])
        self.assertEqual(synchronize(port), "EF4018")
        self.assertEqual(port.rx_mode, "DMA")

    def test_old_firmware_identified(self):
        port = FakePort(["UFT READY EF4018"])
        synchronize(port)
        self.assertEqual(port.rx_mode, "POLLING")

    def test_double_buffer_firmware_identified(self):
        port = FakePort(["UFT READY EF4018 DBM"])
        self.assertEqual(synchronize(port), "EF4018")
        self.assertEqual(port.rx_mode, "DBM")


class StreamPort(FakePort):
    def read_exact(self, count):
        result = self.returned[:count]
        self.returned = self.returned[count:]
        if len(result) != count:
            raise TestError("short read")
        return result


class StoreTests(unittest.TestCase):
    def test_real_three_mib_true_color_bmp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.bmp"
            make_test_bmp(path)
            data = path.read_bytes()
            validate_bmp(data)
            self.assertEqual(len(data), 3145782)
            self.assertEqual(struct.unpack_from("<H", data, 28)[0], 24)

    def test_page_and_chunk_boundaries(self):
        for size in (1, 255, 256, 257, 4095, 4096, 4097, 8192, 8193):
            data = bytes((i * 73) & 255 for i in range(size))
            crc = zlib.crc32(data)
            ends = list(range(CHUNK_BYTES, size, CHUNK_BYTES)) + [size]
            port = FakePort(["FILE READY"] + [f"ACK {end}" for end in ends] +
                            [f"FILE STORED {size} {crc:08X} 123"])
            self.assertEqual(send_file(port, data), 123)
            self.assertEqual(port.writes[0], b"UFL1" + struct.pack("<II", size, crc))
            recovered = bytearray()
            for frame in port.writes[1:]:
                chunk = frame[4:]
                self.assertEqual(struct.unpack("<I", frame[:4])[0], zlib.crc32(chunk))
                recovered.extend(chunk)
            self.assertEqual(bytes(recovered), data)

    def test_wrong_ack_fails(self):
        with self.assertRaises(TestError):
            send_file(FakePort(["FILE READY", "ACK 999"]), b"a")

    def test_readback_crc_and_atomic_file(self):
        data = bytes(range(256)) * 32
        crc = zlib.crc32(data)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "readback.bmp"
            port = StreamPort([f"FILE DATA {len(data)}", f"FILE DATA DONE {crc:08X}"], data)
            self.assertEqual(read_flash_file(port, len(data), destination), crc)
            self.assertEqual(port.writes[0], b"UFR1" + struct.pack("<I", len(data)))
            self.assertEqual(destination.read_bytes(), data)
            destination.unlink()
            port = StreamPort([f"FILE DATA {len(data)}", "FILE DATA DONE 00000000"], data)
            with self.assertRaises(TestError):
                read_flash_file(port, len(data), destination)
            self.assertFalse(destination.exists())


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
                result = gray_reference(source)
                offset = struct.unpack_from("<I", result, 10)[0]
                stride = (width + 3) & ~3
                self.assertEqual(struct.unpack_from("<i", result, 22)[0], height)
                self.assertEqual(len(result), offset + stride * 2)
                for row in range(2):
                    pad = result[offset + row * stride + width:offset + (row + 1) * stride]
                    self.assertEqual(pad, bytes(len(pad)))
                self.assertEqual(result[54:58], b"\0\0\0\0")
                self.assertEqual(result[1074:1078], b"\xff\xff\xff\0")

    def test_output_and_flash_crc_checks(self):
        source = tiny_bmp()
        result = gray_reference(source)
        source_crc = zlib.crc32(source)
        crc = zlib.crc32(result)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "gray.bmp"
            port = StreamPort([f"GRAY DATA {len(result)} 8", f"GRAY DONE {crc:08X} {source_crc:08X} 42"], result)
            ms, _, returned_crc = read_gray(port, source, destination, result)
            self.assertEqual((ms, returned_crc), (42, crc))
            self.assertEqual(destination.read_bytes(), result)
            destination.unlink()
            port = StreamPort([f"GRAY DATA {len(result)} 8", f"GRAY DONE {crc:08X} 00000000 42"], result)
            with self.assertRaises(TestError): read_gray(port, source, destination, result)
            self.assertFalse(destination.exists())

    def test_wrong_pixels_even_with_valid_crc_rejected(self):
        source = tiny_bmp()
        result = gray_reference(source)
        wrong = bytearray(result); wrong[-4] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "gray.bmp"
            port = StreamPort([f"GRAY DATA {len(result)} 8",
                               f"GRAY DONE {zlib.crc32(wrong):08X} {zlib.crc32(source):08X} 1"], bytes(wrong))
            with self.assertRaises(TestError): read_gray(port, source, destination, result)
            self.assertFalse(destination.exists())

    def test_malformed_input(self):
        source = tiny_bmp()
        for offset, value in ((10, 0xFFFFFFFF), (14, 0xFFFFFFFF), (18, 0x7FFFFFFF), (22, 0x80000000), (30, 1)):
            broken = bytearray(source); struct.pack_into("<I", broken, offset, value)
            with self.assertRaises(TestError): gray_reference(broken)


if __name__ == "__main__":
    unittest.main()

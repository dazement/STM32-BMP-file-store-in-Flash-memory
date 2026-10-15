from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from bmp_store import CHUNK_BYTES, TestError, make_test_bmp, read_flash_file, send_file, validate_bmp
from test_uart_flash_test import FakePort


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


if __name__ == "__main__":
    unittest.main()

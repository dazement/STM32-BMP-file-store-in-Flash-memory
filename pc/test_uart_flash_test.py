import struct
import unittest
import zlib

from uart_flash_test import TestError, synchronize, transfer


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


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""October 20 demo: restore the complete original BMP from SPI Flash."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import sys
import time
import zlib

from bmp_store import ROOT, BLOCK_BYTES, change_baud, dma_stats, make_test_bmp, read_flash_file, send_file, validate_bmp
from uart_flash_test import SerialPort, TestError, auto_port, synchronize


def main(argv=None):
    parser = argparse.ArgumentParser(description="2차 발표: 3 MiB BMP 원본 저장·복원 Demo")
    parser.add_argument("--port")
    parser.add_argument("--in", dest="input_path", type=Path)
    parser.add_argument("--baud", type=int, choices=(115200, 921600, 2250000), default=921600)
    parser.add_argument("--read-only", action="store_true", help="리셋 후 Flash에서 복원만 수행")
    args = parser.parse_args(argv)
    port = None
    high_baud = False
    try:
        source_path = args.input_path or ROOT / "images" / "test_1024x1024.bmp"
        if args.input_path is None and not source_path.exists(): make_test_bmp(source_path)
        source = source_path.read_bytes()
        validate_bmp(source)
        expected_crc = zlib.crc32(source) & 0xFFFFFFFF
        expected_sha = hashlib.sha256(source).hexdigest()
        print("원본:", source_path, flush=True)
        print(f"크기: {len(source):,}바이트 / CRC32: {expected_crc:08X}", flush=True)
        port = SerialPort(args.port or auto_port())
        chip = synchronize(port)
        if port.rx_mode != "DBM": raise TestError("새 펌웨어를 다운로드하고 Continue를 누르세요.")
        flash_model = "W25Q128" if chip == "EF4018" else "W25Q32"
        print(f"실제 Flash: {flash_model} / JEDEC: {chip} / UART: 8N1", flush=True)
        if args.baud != 115200:
            high_baud = True
            change_baud(port, args.baud)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        backup_path = None
        store_ms = 0
        if not args.read_only:
            backup_path = ROOT / "backups" / f"flash_before_demo_{stamp}.bin"
            length = (len(source) + BLOCK_BYTES - 1) & ~(BLOCK_BYTES - 1)
            print("기존 Flash 영역 백업 중...", flush=True)
            read_flash_file(port, length, backup_path, backup=True)
        start = time.monotonic()
        if not args.read_only: store_ms = send_file(port, source)
        destination = ROOT / "results" / f"demo_restored_{stamp}.bmp"
        actual_crc = read_flash_file(port, len(source), destination)
        restored = destination.read_bytes()
        actual_sha = hashlib.sha256(restored).hexdigest()
        equal = source == restored
        if not equal or actual_crc != expected_crc or actual_sha != expected_sha:
            raise TestError("원본과 복원 파일의 내용이 다릅니다.")
        elapsed = time.monotonic() - start
        a, b, overruns = dma_stats(port)
        if overruns: raise TestError("DMA 버퍼 초과가 발생했습니다.")
        report = dict(timestamp=datetime.now().isoformat(timespec="seconds"),
                      mode="flash_read_only" if args.read_only else "live_roundtrip",
                      input=str(source_path), restored=str(destination), bytes=len(source),
                      input_crc32=f"{expected_crc:08X}", restored_crc32=f"{actual_crc:08X}",
                      input_sha256=expected_sha, restored_sha256=actual_sha,
                      byte_equal=equal, flash_jedec=chip, flash_model=flash_model,
                      required_flash_model="W25Q32", exact_required_chip=chip == "EF4016",
                      uart_format="8N1", baud=args.baud, spi_mode="1-1-1", spi_clock_hz=27000000,
                      dma_buffer_bytes=4096, dma_buffers=2, dma_a=a, dma_b=b, dma_overruns=overruns,
                      store_mcu_ms=store_ms, roundtrip_and_verification_seconds=elapsed,
                      backup=str(backup_path) if backup_path else None)
        report_path = ROOT / "results" / f"demo_report_{stamp}.json"
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"PASS: 원본과 복원 BMP {len(source):,}바이트 모두 일치")
        print(f"CRC32: {expected_crc:08X} = {actual_crc:08X}")
        print("SHA-256:", expected_sha)
        print(f"Double Buffer: A {a}회 / B {b}회 / 버퍼 초과 {overruns}회")
        print(f"복원·검증까지: {elapsed:.2f} s (백업 준비 별도)")
        print("복원 BMP:", destination)
        print("Demo 기록:", report_path)
        if chip != "EF4016": print("과제의 W25Q32와 실제 칩이 다릅니다. 발표에서는 W25Q128 사용을 명시하세요.")
        return 0
    except (OSError, TestError) as exc:
        print("실패:", exc, file=sys.stderr)
        return 1
    finally:
        if port is not None:
            if high_baud:
                try: change_baud(port, 115200)
                except (OSError, TestError): print("다음 실행 전에 보드 RESET을 누르세요.", file=sys.stderr)
            port.close()


if __name__ == "__main__": sys.exit(main())

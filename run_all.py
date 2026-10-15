#!/usr/bin/env python3
"""테스트 → 빌드 → 다운로드 → 보드 검증 → Flash 저장 증명을 한 번에 실행합니다.

macOS: python3 run_all.py (또는 ./run_all.sh)   Windows: py run_all.py
VS Code Serial Monitor와 디버그 세션을 닫고 실행합니다.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "pc"))
from uart_flash_test import TestError, auto_port, open_port  # noqa: E402

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
INPUT = ROOT / "pc" / "images" / "test_1024x1024.bmp"
BUILD = ROOT / "build" / "cli"          # VS Code가 쓰는 build/Debug와 분리
ELF = BUILD / "bmp_gray_practice.elf"
LOADER = "W25Q128JVEIQ_STM32F746G-DISCO.stldr"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class StepFailed(Exception):
    pass


def step(number, title):
    print(f"\n\033[1;36m[{number}/9] {title}\033[0m", flush=True)


def ok(text):
    print(f"\033[1;32mPASS\033[0m {text}", flush=True)


def run(args, **kwargs):
    if subprocess.run([str(a) for a in args], cwd=ROOT, **kwargs).returncode:
        raise StepFailed(" ".join(str(a) for a in args))


def capture(args, **kwargs):
    """출력이 긴 도구는 모아 두었다가 실패할 때만 끝부분을 보여 줍니다."""
    result = subprocess.run([str(a) for a in args], cwd=ROOT, capture_output=True,
                            text=True, errors="replace", **kwargs)
    output = ANSI.sub("", result.stdout + result.stderr)
    if result.returncode:
        print("\n".join(output.splitlines()[-25:]))
        raise StepFailed(Path(str(args[0])).name + " " + " ".join(str(a) for a in args[1:]))
    return output


def version_key(path):
    return tuple(int(n) for n in re.findall(r"\d+", path.name))


def bundles_root():
    """STM32Cube for VS Code 확장이 도구를 설치하는 폴더."""
    candidates = [os.environ.get("CUBE_BUNDLE_PATH")]
    if WINDOWS:
        candidates.append(os.path.join(os.environ.get("LOCALAPPDATA", ""), "stm32cube", "bundles"))
    elif sys.platform == "darwin":
        candidates.append(str(Path.home() / "Library" / "Application Support" / "stm32cube" / "bundles"))
    for candidate in candidates:
        if candidate and Path(candidate).is_dir():
            return Path(candidate)
    return None


def bundle_tool(root, bundle, *relatives):
    """가장 최신 버전 번들에서 도구를 찾습니다. relatives는 OS별 후보 경로입니다."""
    if root is None:
        return None
    versions = sorted((p for p in (root / bundle).glob("*") if p.is_dir()), key=version_key)
    for version in reversed(versions):
        for relative in relatives:
            path = version / relative
            if path.exists():
                return path
    return None


def find_tools():
    root = bundles_root()
    gcc = bundle_tool(root, "gnu-tools-for-stm32", f"bin/arm-none-eabi-gcc{EXE}") or \
        shutil.which("arm-none-eabi-gcc")
    standalone = [Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin")
                  / "STM32_Programmer_CLI.exe",
                  Path("/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/"
                       "STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI")]
    programmer = bundle_tool(root, "programmer", f"bin/STM32_Programmer_CLI{EXE}") or \
        shutil.which("STM32_Programmer_CLI") or next((p for p in standalone if p.exists()), None)
    tools = {
        "cmake": bundle_tool(root, "cmake", "CMake.app/Contents/bin/cmake", f"bin/cmake{EXE}") or
        shutil.which("cmake"),
        "ninja": bundle_tool(root, "ninja", f"bin/ninja{EXE}") or shutil.which("ninja"),
        "arm-none-eabi-gcc": gcc,
        "STM32_Programmer_CLI": programmer,
        "ExternalLoader": Path(programmer).parent / "ExternalLoader" / LOADER if programmer else None,
    }
    missing = [name for name, path in tools.items() if not path or not Path(path).exists()]
    if missing:
        raise StepFailed("도구를 찾을 수 없습니다: " + ", ".join(missing) +
                         "\n  VS Code에 STM32Cube 확장을 설치하고 프로젝트를 한 번 빌드하면 설치됩니다.")
    tools = {name: Path(path) for name, path in tools.items()}
    tools["cc"] = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    return tools


def stlink_users():
    names = ("ST-LINK_gdbserver", "STM32CubeProgrammer", "openocd")
    command = ["tasklist", "/FO", "CSV", "/NH"] if WINDOWS else ["ps", "ax", "-o", "command"]
    listing = subprocess.run(command, capture_output=True, text=True, errors="replace").stdout.lower()
    return [name for name in names if name.lower() in listing]


def check_board():
    if stlink_users():
        raise StepFailed("ST-LINK를 다른 프로그램이 사용 중입니다: " + ", ".join(stlink_users()) +
                         "\n  VS Code 디버그 세션(Shift+F5)이나 CubeProgrammer를 종료하세요.")
    try:
        path = auto_port()
        # macOS는 같은 포트를 여러 프로그램이 열 수 있어서 lsof로 따로 확인합니다.
        if shutil.which("lsof") and subprocess.run(["lsof", path, path.replace("/cu.", "/tty.")],
                                                   capture_output=True, text=True).stdout.strip():
            raise TestError(path + "를 다른 프로그램이 사용 중입니다.")
        open_port(path).close()
    except (OSError, TestError) as exc:
        raise StepFailed(f"시리얼 포트: {exc}\n  보드 연결을 확인하고 VS Code Serial Monitor를 닫으세요.")
    return path


def main():
    if WINDOWS:
        os.system("")                    # Windows 콘솔에서 ANSI 색상 사용
    start = time.monotonic()
    tools = find_tools()
    port = check_board()
    programmer = tools["STM32_Programmer_CLI"]
    print(f"보드: {port}\nProgrammer: {programmer}")
    env = dict(os.environ, PATH=str(tools["arm-none-eabi-gcc"].parent) + os.pathsep + os.environ["PATH"])
    skipped = []

    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)

        step(1, "PC 단위 테스트")
        run([sys.executable, "-m", "unittest", "discover", "-s", "pc", "-p", "test_*.py"])
        ok("Python 단위 테스트")

        step(2, "C 모의 테스트")
        if tools["cc"] is None:
            print("SKIP: C 컴파일러(cc, gcc, clang)가 없습니다.")
            skipped.append("C 모의 테스트")
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror"]
            for name, sources, include in (("t_dma", ["tests/test_uart_dma.c"], "tests/dma_stub"),
                                           ("t_tx", ["tests/test_uart_tx_dma.c"], "tests/dma_stub"),
                                           ("t_gray", ["tests/test_bmp_gray.c", "Core/Src/bmp_gray.c"],
                                            "Core/Inc")):
                binary = temp / (name + EXE)
                run([tools["cc"], *flags, "-I", include, *sources, "-o", binary])
                run([binary])
            ok("C 모의 테스트 3개")

        step(3, "펌웨어 빌드")
        capture([tools["cmake"], "-S", ".", "-B", BUILD, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
                 "-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake", "-DCMAKE_BUILD_TYPE=Debug"], env=env)
        for line in capture([tools["cmake"], "--build", BUILD], env=env).splitlines():
            if "warning" in line:
                print(line)
        print(capture([tools["arm-none-eabi-gcc"].parent / f"arm-none-eabi-size{EXE}", ELF]).rstrip())
        ok(str(ELF))

        step(4, "보드에 다운로드")
        output = capture([programmer, "-c", "port=SWD", "mode=UR", "reset=HWrst", "-d", ELF, "-v", "-rst"])
        if "Download verified successfully" not in output:
            raise StepFailed("다운로드 검증")
        time.sleep(1)
        ok("다운로드 후 읽어서 대조 일치")

        step(5, "원본 BMP 저장 → 복원 (CRC32·SHA-256)")
        run([sys.executable, "pc/demo_roundtrip.py"])

        step(6, "Gray 전체 과정")
        run([sys.executable, "pc/bmp_gray.py"])

        step(7, "1 KiB 반복 저장·재읽기 12회")
        run([sys.executable, "pc/uart_flash_test.py", "--repeat", "12"])

        step(8, "증명 1: 펌웨어 없이 ST 외부 로더로 QSPI Flash 칩 직접 덤프")
        size = INPUT.stat().st_size
        dump = temp / "qspi_dump.bin"
        capture([programmer, "-c", "port=SWD", "mode=UR", "-el", tools["ExternalLoader"],
                 "-u", "0x90000000", size, dump])
        original = INPUT.read_bytes()
        dumped = dump.read_bytes()
        print("SHA-256 덤프:", hashlib.sha256(dumped).hexdigest())
        print("SHA-256 원본:", hashlib.sha256(original).hexdigest())
        if dumped != original:
            raise StepFailed("Flash 칩 내용이 원본과 다릅니다")
        ok(f"Flash 칩 0x90000000부터 {size:,}바이트 = 원본 BMP")

        step(9, "증명 2: RAM 320 KB 전체를 0xA5로 덮어쓴 뒤 Flash에서만 복원·변환")
        output = capture([programmer, "-c", "port=SWD", "mode=UR", "-fillmemory", "0x20000000", "size=0x50000",
                          "pattern=0xA5A5A5A5", "dataWidth=32", "-r32", "0x20000000", "0x10",
                          "-r32", "0x20027FF0", "0x10", "-r32", "0x2004FFF0", "0x10"])
        if output.count("A5A5A5A5 A5A5A5A5 A5A5A5A5 A5A5A5A5") != 3:
            raise StepFailed("RAM 덮어쓰기 확인")
        ok("RAM 처음·중간·끝 모두 0xA5")
        capture([programmer, "-c", "port=SWD", "mode=HOTPLUG", "-rst"])
        time.sleep(1)
        run([sys.executable, "pc/demo_roundtrip.py", "--read-only"])
        run([sys.executable, "pc/bmp_gray.py", "--convert-only"])

    note = f" / 건너뜀: {', '.join(skipped)}" if skipped else ""
    print(f"\n\033[1;32m모든 단계 PASS ({time.monotonic() - start:.0f}초{note})\033[0m")


if __name__ == "__main__":
    try:
        main()
    except StepFailed as exc:
        print(f"\033[1;31mFAIL\033[0m {exc}")
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)

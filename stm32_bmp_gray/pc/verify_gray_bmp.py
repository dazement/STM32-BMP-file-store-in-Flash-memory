#!/usr/bin/env python3
"""수신한 파일이 정상적인 Gray BMP 인지 검사하고, 원본(컬러)과 픽셀 단위로 비교.

    python verify_gray_bmp.py gray.bmp [--ref color.bmp] [--png preview.png]

검사 항목
  1. BMP 구조: 'BM', 파일 크기 == bfSize, BITMAPINFOHEADER, 8bpp, BI_RGB, 데이터 범위
  2. 팔레트: 256개 엔트리 모두 R == G == B (그레이), i 번째 = (i,i,i)
  3. --ref 지정 시: 원본에서 동일 공식으로 계산한 Gray 값과 100% 일치하는지
     (+ Pillow 가 있으면 Pillow 의 convert('L') 결과와 오차 ±1 이내인지)
종료 코드: 0 = 정상, 1 = 실패
"""
import argparse
import struct
import sys


class BmpError(Exception):
    pass


def parse_bmp(data):
    if len(data) < 54 or data[:2] != b'BM':
        raise BmpError("'BM' 시그니처 없음")
    bf_size, _, _, off = struct.unpack_from('<IHHI', data, 2)
    (isz, w, h, planes, bpp, comp, img_size, xppm, yppm, clr_used, _) = struct.unpack_from('<IiiHHIIiiII', data, 14)
    if isz < 40 or planes != 1:
        raise BmpError(f'지원하지 않는 헤더 (biSize={isz}, planes={planes})')
    if w <= 0 or h == 0:
        raise BmpError(f'잘못된 크기 {w}x{h}')
    rows = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    if off + stride * rows > len(data):
        raise BmpError(f'픽셀 데이터 부족: 필요 {off + stride * rows}, 파일 {len(data)}')
    return dict(bf_size=bf_size, off=off, isz=isz, w=w, h=h, rows=rows, bpp=bpp, comp=comp,
                img_size=img_size, clr_used=clr_used, stride=stride)


def gray_ref(r, g, b):
    """펌웨어와 동일한 BT.601 정수 근사."""
    return (77 * r + 150 * g + 29 * b + 128) >> 8


def color_to_gray_rows(data, info):
    """컬러 BMP -> 행(파일 순서)별 기대 Gray 값 bytes 리스트."""
    if info['bpp'] not in (24, 32) or info['comp'] != 0:
        raise BmpError(f"원본이 True Color(24/32bpp BI_RGB)가 아님: bpp={info['bpp']}, comp={info['comp']}")
    lut_r = [77 * v for v in range(256)]
    lut_g = [150 * v for v in range(256)]
    lut_b = [29 * v + 128 for v in range(256)]
    n = info['bpp'] // 8
    w = info['w']
    out = []
    for y in range(info['rows']):
        s = info['off'] + y * info['stride']
        row = data[s:s + w * n]
        bs, gs, rs = row[0::n], row[1::n], row[2::n]
        out.append(bytes((lut_r[r] + lut_g[g] + lut_b[b]) >> 8 for r, g, b in zip(rs, gs, bs)))
    return out


def verify(gray_path, ref_path=None, png_path=None, quiet=False):
    def log(msg):
        if not quiet:
            print(msg)

    ok = True

    def check(cond, msg):
        nonlocal ok
        log(f"  [{'OK' if cond else 'FAIL'}] {msg}")
        ok &= bool(cond)

    data = open(gray_path, 'rb').read()
    log(f'== {gray_path} ({len(data):,} bytes)')
    try:
        g = parse_bmp(data)
    except BmpError as e:
        log(f'  [FAIL] {e}')
        return False

    check(g['bf_size'] == len(data), f"bfSize({g['bf_size']:,}) == 파일 크기")
    check(g['bpp'] == 8, f"8 bpp (실제 {g['bpp']})")
    check(g['comp'] == 0, 'BI_RGB (무압축)')
    log(f"  크기 {g['w']} x {g['h']} ({'bottom-up' if g['h'] > 0 else 'top-down'}), stride {g['stride']}")
    if g['bpp'] != 8:
        return False

    ncol = g['clr_used'] or 256
    pal_off = 14 + g['isz']
    check(pal_off + ncol * 4 <= g['off'], f'팔레트 {ncol} 엔트리가 헤더와 픽셀 사이에 존재')
    pal = data[pal_off:pal_off + ncol * 4]
    is_gray = all(pal[i * 4] == pal[i * 4 + 1] == pal[i * 4 + 2] for i in range(ncol))
    is_ident = ncol == 256 and all(pal[i * 4] == i for i in range(256))
    check(is_gray, '팔레트 모든 엔트리가 R == G == B (Gray)')
    check(is_ident, '팔레트가 0..255 선형 그레이 램프')

    if ref_path:
        cdata = open(ref_path, 'rb').read()
        c = parse_bmp(cdata)
        log(f"== 원본 {ref_path}: {c['w']} x {c['h']}, {c['bpp']} bpp")
        check((c['w'], c['h']) == (g['w'], g['h']), '원본과 크기/행 방향 동일')
        if (c['w'], c['h']) == (g['w'], g['h']):
            exp = color_to_gray_rows(cdata, c)
            bad = 0
            first = None
            pad_bad = 0
            for y in range(g['rows']):
                s = g['off'] + y * g['stride']
                got = data[s:s + g['w']]
                if got != exp[y]:
                    for x, (a, b) in enumerate(zip(got, exp[y])):
                        if a != b:
                            bad += 1
                            if first is None:
                                first = (x, y, a, b)
                if any(data[s + g['w']:s + g['stride']]):
                    pad_bad += 1
            total = g['w'] * g['rows']
            check(bad == 0, f'픽셀 일치 {total - bad:,}/{total:,}'
                  + (f' (첫 불일치 x={first[0]} row={first[1]} got={first[2]} exp={first[3]})' if first else ''))
            check(pad_bad == 0, '행 패딩 바이트 = 0')

            try:
                from PIL import Image
                pil = Image.open(ref_path).convert('L')
                if g['h'] > 0:          # PIL 은 top-down, BMP bottom-up
                    pil = pil.transpose(Image.FLIP_TOP_BOTTOM)
                pb = pil.tobytes()
                mx = 0
                for y in range(g['rows']):
                    s = g['off'] + y * g['stride']
                    got = data[s:s + g['w']]
                    ref = pb[y * g['w']:(y + 1) * g['w']]
                    mx = max(mx, max(abs(a - b) for a, b in zip(got, ref)))
                check(mx <= 1, f"Pillow convert('L') 대비 최대 오차 {mx} (<= 1)")
            except ImportError:
                log('  (Pillow 미설치: Pillow 교차검증 생략)')

    try:
        from PIL import Image
        im = Image.open(gray_path)
        im.load()
        check(im.mode in ('L', 'P') and im.size == (g['w'], g['rows']), f'Pillow 로 열림: mode={im.mode}, size={im.size}')
        if png_path:
            im.convert('L').save(png_path)
            log(f'  미리보기 저장: {png_path}')
    except ImportError:
        pass
    except Exception as e:  # noqa: BLE001
        check(False, f'Pillow 로 열기 실패: {e}')

    log('== 결과: ' + ('정상적인 Gray BMP 입니다.' if ok else '검증 실패'))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('gray')
    ap.add_argument('--ref', help='원본 컬러 BMP (픽셀 비교)')
    ap.add_argument('--png', help='미리보기 PNG 저장 (Pillow 필요)')
    a = ap.parse_args()
    sys.exit(0 if verify(a.gray, a.ref, a.png) else 1)


if __name__ == '__main__':
    main()

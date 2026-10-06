#!/usr/bin/env python3
"""Checks the firmware's C decoder against the reference encoder in scripts/bap.py.

Both ends are exercised on synthetic frames, so the test needs no copyrighted
material and can run in CI. Every codec and the XOR variant are covered, at a
geometry whose frame is larger than the heatshrink window (so the LZ stage has
to match across frame boundaries) and at one that is smaller.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import bap  # noqa: E402

SRC = ROOT / 'firmware/libraries/BadApple/src'
CASES = [
    (372, 280, 3, bap.CODEC_RLE_HS, False, 14, 4), # runs exceed 15-bit RLE limit
    (372, 280, 3, bap.CODEC_RLE, True, 14, 8),
    # (width, height, frames, codec, xor, heatshrink window, bpp)
    (184, 138, 24, bap.CODEC_RAW, False, 11, 1),
    (184, 138, 24, bap.CODEC_RLE, False, 11, 1),
    (184, 138, 24, bap.CODEC_RLE, True, 11, 1),
    (184, 138, 24, bap.CODEC_RLE_HS, False, 11, 1),  # frame 3174 B > window 2 KB
    (184, 138, 24, bap.CODEC_RLE_HS, True, 11, 1),
    (184, 138, 24, bap.CODEC_RLE_HS, False, 14, 1),  # frame fits in a 16 KB window
    (128, 64, 24, bap.CODEC_RLE_HS, False, 11, 1),   # the hackffm/ESP32_BadApple geometry
    (7, 5, 3, bap.CODEC_RLE_HS, False, 11, 1),       # odd stride, tiny stream
    (186, 140, 12, bap.CODEC_RLE_HS, False, 11, 2),  # grey ramps below
    (186, 140, 12, bap.CODEC_RLE_HS, False, 11, 4),
    (186, 140, 12, bap.CODEC_RLE_HS, False, 11, 8),
    (186, 140, 12, bap.CODEC_RLE, True, 11, 4),
    (7, 5, 3, bap.CODEC_RAW, False, 11, 2),
    (7, 5, 3, bap.CODEC_RAW, False, 11, 8),
    (7, 5, 3, bap.CODEC_RAW, False, 11, 4),          # odd width, half-used byte
]


def expected_render(frame, width, height, bpp, scale):
    """What the panel should receive: big-endian RGB565, every pixel magnified."""
    lut = [bap.gray_rgb565(v, bpp).to_bytes(2, 'big') for v in range(1 << bpp)]
    stride = bap.stride_of(width, bpp)
    mask = (1 << bpp) - 1
    out = bytearray()
    for y in range(height):
        row = bytearray()
        for x in range(width):
            bit = x * bpp
            value = (frame[y * stride + (bit >> 3)] >> (8 - bpp - (bit & 7))) & mask
            row += lut[value] * scale
        out += bytes(row) * scale
    return bytes(out)


def build(workdir):
    binary = workdir / 'test_bap_decode'
    subprocess.run(
        ['cc', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{SRC}', '-o', str(binary),
         str(ROOT / 'tests/test_bap_decode.c'), str(SRC / 'bap_decode.c'),
         str(SRC / 'bap_render.c'), str(SRC / 'heatshrink_decoder.c')],
        check=True)
    return binary


def main():
    with tempfile.TemporaryDirectory() as tmp:
        workdir = pathlib.Path(tmp)
        binary = build(workdir)
        for width, height, count, codec, xor, window, bpp in CASES:
            frames = bap.synthetic_frames(width, height, count, bpp=bpp)
            if width == 372:
                n = bap.frame_bytes_of(width, height, bpp)
                frames = [bytes(n), bytes([255]) * n, (b'\x55\xaa' * ((n + 1) // 2))[:n]]
            blob = bap.encode(frames, width, height, codec=codec, xor=xor,
                              hs_window=window, hs_lookahead=8 if width == 372 else 4, bpp=bpp, scale=1 if width in (372, 7, 186) else 2)
            if bap.decode(blob)[1] != frames:
                sys.exit(f'FAIL python round-trip {width}x{height} bpp={bpp} codec={codec}')
            clip, ref = workdir / 'clip.bap', workdir / 'reference.raw'
            rendered = workdir / 'rendered.raw'
            clip.write_bytes(blob)
            ref.write_bytes(b''.join(frames))
            result = subprocess.run([str(binary), str(clip), str(ref), str(rendered)],
                                    capture_output=True, text=True)
            line = (result.stdout or result.stderr).strip()
            if result.returncode:
                sys.exit(f'FAIL C decoder {width}x{height} bpp={bpp} codec={codec} '
                         f'xor={xor} w{window}: {line}')
            scale = bap.parse_header(blob)['scale']
            if rendered.read_bytes() != expected_render(frames[0], width, height, bpp, scale):
                sys.exit(f'FAIL C renderer {width}x{height} bpp={bpp} scale={scale}')
            print(f'{width:>4}x{height:<4} {bpp}bpp codec={codec} xor={int(xor)} w{window:<3} '
                  f'{len(blob):>8,} B  {line}')
    print('bap decoder and renderer match the reference')


if __name__ == '__main__':
    main()

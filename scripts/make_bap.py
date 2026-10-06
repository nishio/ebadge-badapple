#!/usr/bin/env python3
"""Build a .bap clip for the badge from packed grayscale frames.

Two inputs. `--frames` takes packed 1/2/4/8bpp frames at any geometry, as written
by scripts/badapple_frames.py. `--hs` takes the 128x64 artifact published by
hackffm/ESP32_BadApple (data/video.hs) and rewraps it without changing a pixel.

    curl -L -o video.hs https://github.com/hackffm/ESP32_BadApple/raw/master/data/video.hs
    .venv/bin/python scripts/make_bap.py --hs video.hs --out build/badapple.bap

Frame data is someone else's video: the output is never committed.
"""
import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bap  # noqa: E402

PANEL = 466          # the panel is a circle of this diameter
FFAT_BYTES = 0x1BE0000  # the free data partition the clip is written to

CODECS = {'raw': bap.CODEC_RAW, 'rle': bap.CODEC_RLE, 'rle-hs': bap.CODEC_RLE_HS}


def largest_scale(width, height):
    """Biggest integer magnification whose corners stay inside the circle."""
    scale = 1
    while math.hypot(width * (scale + 1), height * (scale + 1)) <= PANEL:
        scale += 1
    return scale


def frames_from_hs(path):
    import heatshrink2
    blob = pathlib.Path(path).read_bytes()
    flat = bap.rle_decode(heatshrink2.decompress(blob, window_sz2=11, lookahead_sz2=4))
    frame_bytes = bap.frame_bytes_of(128, 64)
    if len(flat) % frame_bytes:
        sys.exit(f'{len(flat)} bytes is not a whole number of 128x64 frames')
    return 128, 64, [flat[i:i + frame_bytes] for i in range(0, len(flat), frame_bytes)]


def frames_from_raw(path, size, bpp):
    width, height = (int(v) for v in size.lower().split('x'))
    flat = pathlib.Path(path).read_bytes()
    frame_bytes = bap.frame_bytes_of(width, height, bpp)
    if len(flat) % frame_bytes:
        sys.exit(f'{len(flat)} bytes is not a whole number of '
                 f'{width}x{height} {bpp}bpp frames')
    return width, height, [flat[i:i + frame_bytes] for i in range(0, len(flat), frame_bytes)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hs', help='published hackffm/ESP32_BadApple artifact, implies 128x64')
    ap.add_argument('--frames', help='packed 1/2/4/8bpp frames')
    ap.add_argument('--size', help='WIDTHxHEIGHT, required with --frames')
    ap.add_argument('--out', default='build/badapple.bap')
    ap.add_argument('--codec', default='rle-hs', choices=sorted(CODECS))
    ap.add_argument('--bpp', type=int, default=1, choices=bap.BIT_DEPTHS,
                    help='bits per pixel in the input frames')
    ap.add_argument('--window', type=int, default=11)
    ap.add_argument('--lookahead', type=int, default=4)
    ap.add_argument('--xor', action='store_true')
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--scale', type=int, default=0, help='0 picks the largest that fits')
    ap.add_argument('--capacity', type=lambda s: int(s, 0), default=FFAT_BYTES,
                    help='target data partition bytes; default is 32 MB layout ffat')
    ap.add_argument('--limit', type=int, default=0, help='keep only the first N frames')
    args = ap.parse_args()

    if args.hs:
        if args.bpp != 1:
            ap.error('--hs is 1bpp material')
        width, height, frames = frames_from_hs(args.hs)
    elif args.frames and args.size:
        width, height, frames = frames_from_raw(args.frames, args.size, args.bpp)
    else:
        ap.error('pass --hs, or --frames with --size')
    if args.limit:
        frames = frames[:args.limit]

    scale = args.scale or largest_scale(width, height)
    blob = bap.encode(frames, width, height, scale=scale, codec=CODECS[args.codec],
                      xor=args.xor, hs_window=args.window, hs_lookahead=args.lookahead,
                      fps_num=args.fps, bpp=args.bpp)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)

    raw = len(frames) * bap.frame_bytes_of(width, height, args.bpp)
    shown = f'{width * scale}x{height * scale}'
    corner = math.hypot(width * scale, height * scale)
    print(f'{out}: {len(frames)} frames {width}x{height} {args.bpp}bpp x{scale} -> {shown} '
          f'(corners at {corner:.0f} of {PANEL} px)')
    print(f'  {len(blob):,} B  raw {raw:,} B  ratio {raw / len(blob):.2f}x  '
          f'{len(blob) / len(frames):.1f} B/frame  '
          f'{len(frames) / args.fps:.1f} s at {args.fps} fps')
    print(f'  ffat {args.capacity:,} B: ' +
          ('fits' if len(blob) <= args.capacity else 'DOES NOT FIT'))
    if len(blob) > args.capacity:
        sys.exit(1)


if __name__ == '__main__':
    main()

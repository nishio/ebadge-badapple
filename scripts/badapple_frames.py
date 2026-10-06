#!/usr/bin/env python3
"""Turn a Bad Apple video into packed frames at one target geometry and bit depth.

The point is to go from the source straight to the stored resolution in a single
downscale; never via a smaller proxy that is enlarged again. Output is raw,
`ceil(width * bits / 8) * height` bytes per frame, MSB first, matching the layout
that scripts/make_bap.py and the hackffm/ESP32_BadApple artifact use.

    .venv/bin/python scripts/badapple_frames.py badapple.mp4 out.bin --size 186x140
    .venv/bin/python scripts/badapple_frames.py badapple.mp4 out.bin --size 186x140 --dither fs

Dithering trades compressibility for apparent grey. Ordered dithering is periodic,
so it survives run-length encoding far better than Floyd-Steinberg noise does.

Frame data is derived from someone else's video; keep it out of the repository.
"""
import argparse, subprocess, sys
import numpy as np
from PIL import Image

# 8x8 Bayer matrix, scaled to (0,1); periodic, so run-length encoding survives it.
BAYER = (np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                   [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                   [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                   [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]])
         + 0.5) / 64


def floyd_steinberg(frame, levels):
    """Serial error diffusion; the classic dither, and the worst case for RLE."""
    if levels == 2:      # Pillow ships this in C; the loop below is far too slow
        return np.asarray(Image.fromarray(frame).convert('1', dither=Image.Dither.FLOYDSTEINBERG),
                          dtype=np.uint8)
    work = frame.astype(np.float32)
    height, width = work.shape
    step = 255.0 / (levels - 1)
    for y in range(height):
        row, below = work[y], work[y + 1] if y + 1 < height else None
        for x in range(width):
            old = row[x]
            new = min(max(round(old / step), 0), levels - 1)
            error = old - new * step
            row[x] = new
            if x + 1 < width:
                row[x + 1] += error * 7 / 16
            if below is not None:
                if x:
                    below[x - 1] += error * 3 / 16
                below[x] += error * 5 / 16
                if x + 1 < width:
                    below[x + 1] += error * 1 / 16
    return work.astype(np.uint8)


def pack(values, bits):
    """Pack `bits`-wide values, MSB first, into bytes."""
    per_byte = 8 // bits
    out = np.zeros((values.shape[0], values.shape[1] // per_byte), dtype=np.uint8)
    for i in range(per_byte):
        out |= values[:, i::per_byte].astype(np.uint8) << (8 - bits * (i + 1))
    return out.tobytes()


def probe(path):
    out = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
         'stream=width,height,r_frame_rate,nb_frames', '-of', 'default=noprint_wrappers=1:nokey=1',
         path], capture_output=True, text=True, check=True).stdout.split()
    num, den = out[2].split('/')
    return int(out[0]), int(out[1]), int(num) / int(den), int(out[3])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('output')
    ap.add_argument('--size', required=True, help='WIDTHxHEIGHT of the stored frame')
    ap.add_argument('--fps', type=float, help='resample rate; default is the source rate')
    ap.add_argument('--threshold', type=int, default=127, help='>threshold is lit (hackffm/ESP32_BadApple uses 127)')
    ap.add_argument('--levels', type=int, default=2, choices=(2, 4, 16, 256),
                    help='grey levels: 2 is 1bpp, 4 is 2bpp, 16 is 4bpp, 256 is 8bpp')
    ap.add_argument('--dither', default='none', choices=('none', 'ordered', 'fs'),
                    help='ordered is an 8x8 Bayer matrix; fs is Floyd-Steinberg')
    ap.add_argument('--endpoint-snap', type=int, default=0, choices=range(0, 9),
                    help='snap source grey <=N to black and >=255-N to white before quantization')
    ap.add_argument('--white-point', type=int, default=255, choices=range(1, 256),
                    help='after area resize, map gray v to min(round(v*255/white-point),255); black stays 0')
    ap.add_argument('--filter', default='area', help='ffmpeg scaler: area, lanczos, bicubic')
    ap.add_argument('--crop-square', action='store_true',
                    help='centre-crop the source to a square before scaling (option C)')
    args = ap.parse_args()

    width, height = (int(v) for v in args.size.lower().split('x'))
    src_w, src_h, src_fps, src_frames = probe(args.video)
    print(f'source {src_w}x{src_h} {src_fps:g} fps {src_frames} frames -> {width}x{height}'
          f'{" (square crop)" if args.crop_square else ""} @ {args.fps or src_fps:g} fps'
          f' via {args.filter}, {args.levels} levels, dither={args.dither}, endpoint_snap={args.endpoint_snap}, white_point={args.white_point}')

    chain = []
    if args.crop_square:
        chain.append(f'crop={min(src_w, src_h)}:{min(src_w, src_h)}')
    if args.fps:
        chain.append(f'fps={args.fps}')
    chain += [f'scale={width}:{height}:flags={args.filter}', 'format=gray']

    bits = {2: 1, 4: 2, 16: 4, 256: 8}[args.levels]
    stride = (width * bits + 7) // 8
    pixels = width * height
    proc = subprocess.Popen(
        ['ffmpeg', '-v', 'error', '-i', args.video, '-vf', ','.join(chain),
         '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
        stdout=subprocess.PIPE)

    # Pad each row to a byte boundary, then pack `bits` per pixel, MSB first.
    padded = np.zeros((height, stride * 8 // bits), dtype=np.uint8)
    threshold = np.tile(BAYER, (height // 8 + 1, width // 8 + 1))[:height, :width]
    count = 0
    white_lut = np.clip(np.rint(np.arange(256) * 255.0 / args.white_point), 0, 255).astype(np.uint8)
    with open(args.output, 'wb') as out:
        while True:
            buf = proc.stdout.read(pixels)
            if len(buf) < pixels:
                break
            frame = np.frombuffer(buf, dtype=np.uint8).reshape(height, width)
            if args.white_point != 255:
                frame = white_lut[frame]
            if args.endpoint_snap:
                frame = frame.copy()
                frame[frame <= args.endpoint_snap] = 0
                frame[frame >= 255 - args.endpoint_snap] = 255
            if args.levels == 2 and args.dither == 'none':
                padded[:, :width] = frame > args.threshold
            elif args.dither == 'fs':
                padded[:, :width] = floyd_steinberg(frame, args.levels)
            elif args.dither == 'ordered':
                scaled = frame.astype(np.float32) / 255 * (args.levels - 1)
                floor = np.floor(scaled)
                padded[:, :width] = np.clip(floor + (scaled - floor > threshold),
                                            0, args.levels - 1)
            else:
                padded[:, :width] = np.round(frame.astype(np.float32) / 255
                                             * (args.levels - 1))
            out.write(np.packbits(padded, axis=1, bitorder='big').tobytes()
                      if bits == 1 else pack(padded, bits))
            count += 1
    if proc.wait() != 0:
        sys.exit('ffmpeg failed')
    print(f'{count} frames x {stride * height} B = {count * stride * height:,} B -> {args.output}')

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Build a synthetic clip and soundtrack, to try the player without the source video.

The picture is a grey ramp, a moving disc and a frame counter bar; once a second
the whole frame flashes white while the soundtrack beeps, so A/V sync can be
judged by eye and ear. Nothing here is derived from anyone else's work.

    .venv/bin/python scripts/make_demo_clip.py --out-dir build/demo
    scripts/upload_video.sh PORT build/demo/demo.bap
    .venv/bin/python scripts/upload_av_audio.py PORT build/demo/demo.bap build/demo/audio/full.aap \
        --backup build/demo/tail-before.bin --metadata build/demo/upload.json
"""
import argparse
import math
import pathlib
import subprocess
import sys
import wave

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
import bap  # noqa: E402
from badapple_frames import pack  # noqa: E402

WIDTH, HEIGHT, BPP, FPS, RATE = 372, 280, 4, 30, 22050


def frame(index):
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    if index % FPS < 3:                       # one-second marker, first 0.1 s
        return np.full((HEIGHT, WIDTH), 15, dtype=np.uint8)
    img = (x * 16 // WIDTH).astype(np.uint8) // 2   # dim ramp, 0..7
    t = index / FPS
    cx = WIDTH / 2 + WIDTH * 0.33 * math.cos(t * 1.3)
    cy = HEIGHT / 2 + HEIGHT * 0.30 * math.sin(t * 2.1)
    img[(x - cx) ** 2 + (y - cy) ** 2 < 40 ** 2] = 15
    img[HEIGHT - 12:, :int(WIDTH * (index % FPS + 1) / FPS)] = 12   # sub-second progress
    return img


def write_tone(path, frames):
    samples = frames * RATE // FPS
    n = np.arange(samples)
    beep = (n % RATE) < RATE // 10                       # 0.1 s beep each second
    pcm = (0.4 * 32767 * np.sin(2 * np.pi * 880 * n / RATE) * beep).astype('<i2')
    with wave.open(str(path), 'wb') as w:
        w.setparams((1, 2, RATE, samples, 'NONE', ''))
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('build/demo'))
    ap.add_argument('--seconds', type=int, default=20)
    args = ap.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    count = args.seconds * FPS
    frames = [pack(frame(i), BPP) for i in range(count)]
    blob = bap.encode(frames, WIDTH, HEIGHT, scale=1, codec=bap.CODEC_RLE_HS,
                      hs_window=14, hs_lookahead=8, fps_num=FPS, bpp=BPP)
    clip = args.out_dir / 'demo.bap'
    clip.write_bytes(blob)
    print(f'{clip}: {count} frames {WIDTH}x{HEIGHT} {BPP}bpp, {len(blob):,} B')
    tone = args.out_dir / 'tone.wav'
    write_tone(tone, count)
    subprocess.run([sys.executable, str(ROOT / 'prepare_av_audio.py'), str(tone), str(clip),
                    '--output-dir', str(args.out_dir / 'audio')], check=True,
                   stdout=subprocess.DEVNULL)
    print(f'{args.out_dir / "audio/full.aap"}: {(args.out_dir / "audio/full.aap").stat().st_size:,} B')


if __name__ == '__main__':
    main()

# Bad Apple!! on a round AMOLED badge

A full-length *Bad Apple!!* player for the
[Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)
(466×466 round AMOLED, ESP32-S3, 32 MB flash, 8 MB PSRAM): all 6573 frames at
372×280 with 16 grey levels, in sync with the panel's tearing signal, and
optionally with the soundtrack through the on-board ES8311 codec and speaker.

This repository contains **code only**. It does not contain the video, the music,
or anything converted from them. See [Preparing the data](#preparing-the-data).

## How it works

- **Container (`.bap`)**: 4-bit packed frames → run-length coding → heatshrink
  (LZSS, 16 KB window) as one continuous stream, so the LZ stage matches against
  the previous frame. The full clip is 25.8 MB, which fits the 27.9 MB data partition.
  Encoder and reference decoder: [`scripts/bap.py`](scripts/bap.py). Device
  decoder: [`firmware/libraries/BadApple`](firmware/libraries/BadApple).
- **Two cores**: core 0 decodes up to 8 frames ahead into PSRAM; core 1 expands
  the next one into a full RGB565 frame in internal DMA RAM and sends it in one burst.
- **Tear-free**: the CO5300's TE output (GPIO13) is enabled and measured at boot.
  Each transfer starts within 500 µs after a TE edge, so 30 fps material is shown
  on every second panel refresh. The transfer rectangle uses even start and even
  size, which the CO5300 requires.
- **Audio** (`-DVIDEO_AUDIO`): the soundtrack is mono IMA ADPCM at 22.05 kHz
  (`.aap`, 2.4 MB). It is stored right after the clip and fed to I2S, and the
  audio clock paces the video.

On the device this played all 6573 of 6573 frames without a prefetch underrun,
with and without audio.

## Requirements

- The board above. The partition table assumes **32 MB flash**.
- [arduino-cli](https://arduino.github.io/arduino-cli/) with the ESP32 core 3.3.11
- Python 3, FFmpeg (8.0.1 to reproduce the reference files bit for bit), a C compiler

```sh
python3 -m venv .venv
.venv/bin/pip install -r scripts/requirements.txt
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
scripts/test.sh                                   # host tests, no media needed
```

`scripts/build.sh` fetches the Waveshare board repository at a pinned commit
(for `Arduino_GFX`, `SensorLib` and `pin_config.h`) into `.cache/`.

## Preparing the data

*Bad Apple!!* is someone else's work. The shadow-art PV is by あにら (Anira),
[nicovideo sm8628149](https://www.nicovideo.jp/watch/sm8628149). The music is
"Bad Apple!! feat. nomico" by Alstroemeria Records, an arrangement of a Touhou
Project theme by ZUN. No explicit permission to redistribute either has been
found. So this project ships no media and no script that downloads any.
**Bring your own copy, and keep what you convert local.** The output directories
are in `.gitignore`.

### 1. Get the source

Use a copy of the PV that you are allowed to use. The reference files were made
from the Internet Archive's preservation copy
([nicovideo-sm8628149](https://archive.org/details/nicovideo-sm8628149)):

| | |
|---|---|
| container | MP4, H.264 + AAC |
| video | 480×360, exactly 30 fps, 6573 frames, 219.10 s |
| SHA-256 | `faf50034a3f0a9bf5f4180e720ecf6aea6b21f0b64972b0104898662dec7442d` |

Other copies of the PV will work if they have the same frame count at 30 fps.
They will not be byte-identical to the reference. The soundtrack is taken from
the same file. If your copy has no audio, build the video-only firmware.

### 2. Convert

```sh
scripts/prepare_data.sh /path/to/badapple.mp4          # writes build/data/
```

The script runs three steps and compares every result with the files that were
tested on the badge:

1. `badapple_frames.py`: scale to 372×280 (area filter), 16 grey levels, 8×8
   ordered dither
2. `make_bap.py`: encode to `badapple.bap` (heatshrink window 14, lookahead 8)
3. `prepare_av_audio.py --preset bass-headroom`: downmix to mono, apply −6 dB,
   add a +3 dB low shelf at 350 Hz for the small speaker, trim to the clip's
   exact length, and encode IMA ADPCM into `audio/full.aap`

| output | SHA-256 with the source above and FFmpeg 8.0.1 |
|---|---|
| `badapple.bap` (25,774,637 B) | `bf26b8204721462876667a9b21dda2f98c56a6ab92684dccb603abf3f0ffcfb9` |
| `audio/full.aap` (2,424,896 B) | `30c5ee90fd925ad4f5af42f4a51a33c202151e3fe077d506a0edd4ab81e57a5b` |

A mismatch is not fatal. It usually means a different source copy or FFmpeg
version, and the clip will still play. The individual tools take more options
(other sizes, bit depths, dithers, `--preset original`); see `--help`.

### No source? Try the demo clip

```sh
.venv/bin/python scripts/make_demo_clip.py      # build/demo/demo.bap + audio/full.aap
```

The demo is 20 s of synthetic test pattern. Once a second the screen flashes
white and the speaker beeps, so you can check the whole chain (flash layout,
decoder, TE sync, audio sync) without any third-party material.

## Flashing

Ports change between connections, so check yours each time (`/dev/cu.usbmodem*`
on macOS). **Back up the factory flash first.** `upload.sh` refuses to run
without a complete 32 MB backup:

```sh
.venv/bin/python -m esptool --port PORT --baud 921600 read-flash 0 0x2000000 /absolute/path/factory-32mb.bin
```

Firmware:

```sh
BUILD_EXTRA_FLAGS='-DVIDEO_AUDIO' scripts/build.sh     # or plain scripts/build.sh for video only
scripts/upload.sh PORT /absolute/path/factory-32mb.bin
```

Data goes to the `ffat` partition. It survives later firmware uploads, so you
only write it once:

```sh
scripts/upload_video.sh PORT build/data/badapple.bap
.venv/bin/python scripts/upload_av_audio.py PORT build/data/badapple.bap build/data/audio/full.aap \
    --backup build/data/tail-before.bin --metadata build/data/upload.json
```

Both scripts read the partition table back from the device, so a write can never
land in an app slot. Both also verify every written byte by reading it back.
`upload_av_audio.py` saves whatever it overwrites to `--backup` first and refuses
to reuse an existing backup path.

## Using it

- On boot it verifies the clip's CRC (a few seconds) and starts playing.
  Build with `-DNO_AUTOSTART` to wait on the idle screen instead.
- Swipe to stop. Tap the idle screen to play again.
- With audio, playback **starts muted**. Tap **UNMUTE** at the top. The bottom
  row is **− / STOP / +** for volume (starts at 50/100). Above 75 the codec adds
  gain and loud passages may clip.
- Over serial (115200): `p` play, `s` stop. With audio: `+`, `-`, `m` (mute),
  and `?` (status).
- `-DVIDEO_PROFILE` adds timing statistics: frames shown, decode, transfer, TE
  phase and missed edges. They are reported after each loop and on any serial
  input. `scripts/capture_serial.py` records them without resetting the board.
- `-DVIDEO_AUDIO_SILENT` runs the whole audio path with the codec muted and the
  amplifier off, for measurements in quiet places.

## Layout

```
firmware/badapple_player/     the sketch: TE sync, prefetch, audio, profile
firmware/libraries/BadApple   .bap decoder and RGB565 row expander (+ heatshrink decoder)
firmware/libraries/BadgeAudio IMA ADPCM block decoder (+ ES8311 driver)
firmware/partitions/          32 MB layout: 2 × 2 MiB app, 27.875 MiB data
scripts/                      conversion, build, upload, capture
tests/                        host tests on synthetic data
```

## License

MIT, see [LICENSE](LICENSE). Bundled third-party code keeps its own license:

- heatshrink decoder: ISC, Scott Vokes
  ([`heatshrink_LICENSE`](firmware/libraries/BadApple/src/heatshrink_LICENSE))
- ES8311 driver: Apache-2.0, Espressif Systems, as shipped in Waveshare's board
  repository at commit `6d19f7e`
  ([`vendor/LICENSE`](firmware/libraries/BadgeAudio/src/vendor/LICENSE))

The license covers the code in this repository only. It grants nothing for
*Bad Apple!!* itself.

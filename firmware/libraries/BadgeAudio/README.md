# BadgeAudio

`ima_mono_decode` decodes one mono, 4-bit IMA WAV block into bounded caller-owned PCM storage. It validates the block header and output size before writing. Each block starts from its own predictor and index. Nibbles are low-first and integer rounding multiplies before truncation, checked against the installed FFmpeg 8.0.1 IMA WAV decoder. FFmpeg master now uses a different rounding path; validation targets the encoder/decoder used for these local assets.

`src/vendor/es8311.*` and `es8311_reg.h` are unchanged copies from Waveshare C-board commit `6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c`, `examples/arduino/examples/07_ES8311/`. They retain Espressif's Apache-2.0 notices; the upstream license is in `src/vendor/LICENSE`. Source: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/tree/6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c/examples/arduino/examples/07_ES8311

The decoder is independent of I2S and heap allocation. The player uses it from `firmware/badapple_player/video_audio.h`.

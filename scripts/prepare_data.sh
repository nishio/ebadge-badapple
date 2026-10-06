#!/usr/bin/env bash
# Turn your own copy of the Bad Apple!! PV into the clip and soundtrack the player
# reads, and compare them with the files that were tested on the badge.
#
#   scripts/prepare_data.sh /path/to/badapple.mp4 [OUT_DIR]
#
# Nothing is downloaded. The outputs are derived from someone else's work: keep
# them local, do not commit or redistribute them.
set -euo pipefail
cd "$(dirname "$0")/.."
source=${1:?Usage: scripts/prepare_data.sh SOURCE_VIDEO [OUT_DIR]}
out=${2:-build/data}

# The source and outputs verified on hardware (FFmpeg 8.0.1). Another copy of
# the PV or another FFmpeg can still play fine; it just will not be bit-identical.
SOURCE_SHA256=faf50034a3f0a9bf5f4180e720ecf6aea6b21f0b64972b0104898662dec7442d
VIDEO_SHA256=bf26b8204721462876667a9b21dda2f98c56a6ab92684dccb603abf3f0ffcfb9
AUDIO_SHA256=30c5ee90fd925ad4f5af42f4a51a33c202151e3fe077d506a0edd4ab81e57a5b

sha() { LC_ALL=C shasum -a 256 "$1" | cut -d' ' -f1; }
check() { # label actual expected
  if [ "$2" = "$3" ]; then echo "  $1: matches the tested file"
  else echo "  $1: differs from the tested file ($2)"; fi
}

[ ! -e "$out/badapple.bap" ] && [ ! -e "$out/audio" ] || {
  echo "$out already holds outputs; pass a fresh OUT_DIR" >&2; exit 1; }
mkdir -p "$out"
echo "ffmpeg: $(ffmpeg -version | head -1)"
ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate,nb_frames \
  -of default=noprint_wrappers=1 "$source"
check source "$(sha "$source")" "$SOURCE_SHA256"

.venv/bin/python scripts/badapple_frames.py "$source" "$out/frames.bin" \
  --size 372x280 --levels 16 --dither ordered
.venv/bin/python scripts/make_bap.py --frames "$out/frames.bin" --size 372x280 --bpp 4 \
  --window 14 --lookahead 8 --out "$out/badapple.bap"
rm "$out/frames.bin"   # 342 MB of raw frames, not needed after encoding
.venv/bin/python scripts/prepare_av_audio.py "$source" "$out/badapple.bap" \
  --preset bass-headroom --output-dir "$out/audio" > /dev/null

echo "outputs:"
check "video $out/badapple.bap" "$(sha "$out/badapple.bap")" "$VIDEO_SHA256"
check "audio $out/audio/full.aap" "$(sha "$out/audio/full.aap")" "$AUDIO_SHA256"
cat <<EOF
next:
  scripts/upload_video.sh PORT $out/badapple.bap
  .venv/bin/python scripts/upload_av_audio.py PORT $out/badapple.bap $out/audio/full.aap \\
      --backup $out/tail-before.bin --metadata $out/upload.json
EOF

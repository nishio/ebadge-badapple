#!/usr/bin/env bash
# Resolve ffat from the device, so changing the firmware layout cannot cause
# a clip upload to overwrite an app slot. No FAT filesystem is used.
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?Usage: scripts/upload_video.sh PORT CLIP.bap}
clip=${2:?Pass the .bap clip to write}
size=$(wc -c < "$clip" | tr -d ' ')
[ "$(head -c 4 "$clip")" = 'BAP1' ] || { echo 'Not a .bap clip' >&2; exit 1; }
mkdir -p build
scratch=$(mktemp -d build/upload-video.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
.venv/bin/python -m esptool --port "$port" --baud 921600 read-flash 0x8000 0x1000 "$scratch/partitions.bin"
layout=$(.venv/bin/python scripts/flash_partition.py "$scratch/partitions.bin")
read -r offset limit <<< "$layout"
[ "$size" -le "$limit" ] || { echo "Clip is $size bytes, ffat holds $limit" >&2; exit 1; }
echo "device ffat: offset=$offset bytes=$limit; clip=$clip bytes=$size"
.venv/bin/python -m esptool --port "$port" --baud 921600 write-flash "$offset" "$clip"
# The flasher already verifies a digest. Independent byte-for-byte readback also
# proves the chosen address and host artifact, and is kept in the experiment log.
# Keep one stub connection and read 512 KiB at a time. Retry only a failed read,
# never the flash write; every byte must still match the host artifact.
.venv/bin/python scripts/verify_video_readback.py "$port" "$offset" "$clip"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
upstream=.cache/waveshare-1.75c
revision=6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c
if [ ! -d "$upstream/.git" ]; then
  mkdir -p .cache
  git clone --filter=blob:none --no-checkout https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C.git "$upstream"
  git -C "$upstream" sparse-checkout set examples/arduino
  git -C "$upstream" checkout --detach "$revision"
fi
[ "$(git -C "$upstream" rev-parse HEAD)" = "$revision" ] || { echo 'Unexpected upstream revision' >&2; exit 1; }
sketch=${1:-badapple_player}
source scripts/board_config.sh
partition_flags=()
export ARDUINO_BUILD_CACHE_PATH=${ARDUINO_BUILD_CACHE_PATH:-"$PWD/.cache/arduino-build-cache"}
if [ "$partition_scheme" = custom ]; then
  partition_flags=(--build-property "recipe.hooks.prebuild.3.pattern=/bin/cp \"$PWD/$partition_csv\" \"{build.path}/partitions.csv\""
                   --build-property upload.maximum_size=2097152)
fi
arduino-cli compile --fqbn "$fqbn" "${partition_flags[@]}" ${BUILD_EXTRA_FLAGS:+--build-property compiler.cpp.extra_flags="$BUILD_EXTRA_FLAGS"} \
  --build-path "$PWD/build/arduino/$sketch" \
  --libraries "$upstream/examples/arduino/libraries" --libraries firmware/libraries \
  --output-dir "build/$sketch" "firmware/$sketch"

# Record what this build actually is. build/ is .gitignored, so without a stamp
# nobody can tell from the outside whether build/<sketch> holds a plain build or
# one carrying experiment flags -- and scripts/upload.sh flashes whatever is
# there.
{
  echo "sketch=$sketch"
  echo "fqbn=$fqbn"
  if [ "$partition_scheme" = custom ]; then
    echo "partition_csv=$partition_csv"
    echo "partition_source_sha256=$(LC_ALL=C shasum -a 256 "$partition_csv" | cut -d' ' -f1)"
  fi
  echo "partition_sha256=$(LC_ALL=C shasum -a 256 "build/$sketch/$sketch.ino.partitions.bin" | cut -d' ' -f1)"
  echo "built=$(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "flags=${BUILD_EXTRA_FLAGS:-}"
  echo "head=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  # Untracked sources count too: a new sketch is not "clean" until committed.
  echo "tree=$([ -z "$(git status --porcelain -- firmware "$0" 2>/dev/null)" ] && echo clean || echo dirty)"
} > "build/$sketch/BUILD_STAMP"

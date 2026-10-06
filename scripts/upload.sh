#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?Usage: scripts/upload.sh PORT VERIFIED_BACKUP [SKETCH]}
backup=${2:?Pass the verified 32 MB factory backup path}
sketch=${3:-badapple_player}
[ "$(wc -c < "$backup" | tr -d ' ')" = 33554432 ] || { echo 'Expected complete 32 MB backup' >&2; exit 1; }

# Say what is about to be flashed. The badge is shared between sessions and
# build/ is .gitignored, so the artifact here may be someone else's experiment.
image="build/$sketch/$sketch.ino.bin"
[ -f "$image" ] || { echo "No build for $sketch; run scripts/build.sh $sketch" >&2; exit 1; }
echo "--- flashing $sketch ---"
if [ -f "build/$sketch/BUILD_STAMP" ]; then
  sed 's/^/    /' "build/$sketch/BUILD_STAMP"
  flags=$(sed -n 's/^flags=//p' "build/$sketch/BUILD_STAMP")
  if [ -n "$flags" ]; then
    echo "    NOTE: this build carries extra flags, it is not a plain build"
  fi
else
  echo "    (no BUILD_STAMP: built before stamping, or by hand -- contents unknown)"
fi

# A binary older than the sources is the classic "edited, then flashed the old
# build" mistake. Refuse rather than flash something nobody asked for.
# Look at sources only: arduino-cli leaves its own intermediates under
# firmware/<sketch>/build, and those are always newer than the output.
stale=$(find -L "firmware/$sketch" firmware/libraries -name build -prune -o \
  -type f \( -name '*.ino' -o -name '*.h' -o -name '*.c' -o -name '*.cpp' \) \
  -newer "$image" -print 2>/dev/null | head -1)
if [ -n "$stale" ]; then
  echo "    stale: $stale is newer than the build" >&2
  echo "Sources are newer than $image; run scripts/build.sh $sketch first" >&2
  exit 1
fi

source scripts/board_config.sh
# Upload with the configuration that produced these artifacts.
stamped_fqbn=$(sed -n 's/^fqbn=//p' "build/$sketch/BUILD_STAMP" 2>/dev/null || true)
[ -n "$stamped_fqbn" ] || { echo 'Rebuild to record the partition configuration' >&2; exit 1; }
fqbn=$stamped_fqbn
csv=$(sed -n 's/^partition_csv=//p' "build/$sketch/BUILD_STAMP")
if [ -n "$csv" ]; then
  expected_csv=$(sed -n 's/^partition_source_sha256=//p' "build/$sketch/BUILD_STAMP")
  [ "$(LC_ALL=C shasum -a 256 "$csv" | cut -d' ' -f1)" = "$expected_csv" ] || {
    echo 'Partition CSV changed; rebuild before uploading' >&2; exit 1;
  }
fi
expected=$(sed -n 's/^partition_sha256=//p' "build/$sketch/BUILD_STAMP")
actual=$(LC_ALL=C shasum -a 256 "build/$sketch/$sketch.ino.partitions.bin" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || { echo 'Partition artifact differs from BUILD_STAMP' >&2; exit 1; }
arduino-cli upload --fqbn "$fqbn" --port "$port" --input-dir "build/$sketch" "firmware/$sketch"

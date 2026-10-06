#!/usr/bin/env bash
# Host tests. None of them needs the source video: clips and audio are synthetic.
set -euo pipefail
cd "$(dirname "$0")/.."
.venv/bin/python tests/verify_bap.py
.venv/bin/python tests/verify_ima_audio.py
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror tests/verify_audio_output.cpp -o "$scratch/verify_audio_output"
"$scratch/verify_audio_output"

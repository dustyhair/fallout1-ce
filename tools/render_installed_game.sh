#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
game_root=${1:-/home/jwagner/Games/Fallout-CE-TTS}
extracted_root="$game_root/.kokoro/master-extracted"
build_root="$game_root/.tts-build"
cast_jobs=${FALLOUT_CAST_JOBS:-4}
render_threads=${FALLOUT_RENDER_THREADS:-8}
shard_count=${FALLOUT_SHARD_COUNT:-1}
shard_index=${FALLOUT_SHARD_INDEX:-0}

export PYTHONPATH="$source_root/tools"

"$game_root/.kokoro/venv/bin/python" "$source_root/tools/prepare_voice_cast.py" \
  --source "$extracted_root/TEXT/ENGLISH/DIALOG" \
  --patch-source "$game_root/DATA/TEXT/ENGLISH/DIALOG" \
  --scripts-list "$extracted_root/SCRIPTS/SCRIPTS.LST" \
  --ssl-dir "$build_root/ssl" \
  --output "$build_root/cast" \
  --kokoro-model "$game_root/.kokoro/models/kokoro-v1.0.int8.onnx" \
  --kokoro-voices "$game_root/.kokoro/models/voices-v1.0.bin" \
  --jobs "$cast_jobs"

"$game_root/.chatterbox/venv/bin/python" -u "$source_root/tools/precompute_tts.py" \
  --source "$extracted_root/TEXT/ENGLISH/DIALOG" \
  --patch-source "$game_root/DATA/TEXT/ENGLISH/DIALOG" \
  --scripts-list "$extracted_root/SCRIPTS/SCRIPTS.LST" \
  --ssl-dir "$build_root/ssl" \
  --cache "$game_root/TTS_CACHE" \
  --cast "$build_root/cast/cast.json" \
  --model-dir "$game_root/.chatterbox/turbo-onnx" \
  --threads "$render_threads" \
  --shard-count "$shard_count" \
  --shard-index "$shard_index"

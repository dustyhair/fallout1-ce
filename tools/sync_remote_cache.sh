#!/usr/bin/env bash
set -euo pipefail

remote=${FALLOUT_TTS_REMOTE:-root@codex-testbox}
remote_cache=${FALLOUT_TTS_REMOTE_CACHE:-/root/fallout-tts/game/TTS_CACHE}
local_cache=${FALLOUT_TTS_LOCAL_CACHE:-/home/jwagner/Games/Fallout-CE-TTS/TTS_CACHE}

mkdir -p "$local_cache/dialog"
rsync -a --ignore-existing --exclude='*.part' "$remote:$remote_cache/dialog/" "$local_cache/dialog/"
rsync -a "$remote:$remote_cache/manifest.jsonl" "$local_cache/manifest.jsonl"

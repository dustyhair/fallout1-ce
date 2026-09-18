#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
exec "$source_root/tools/generate_tts_cache.sh" "$@"

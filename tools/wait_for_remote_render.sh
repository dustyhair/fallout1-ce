#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
remote=${FALLOUT_TTS_REMOTE:-root@codex-testbox}
poll_seconds=${FALLOUT_TTS_POLL_SECONDS:-60}
services=(fallout-tts-render fallout-tts-render-b)

while true; do
  states=$(ssh "$remote" systemctl show --property=ActiveState --value "${services[@]}")
  if ! grep -Eq '^(active|activating|reloading)$' <<<"$states"; then
    break
  fi
  sleep "$poll_seconds"
done

results=$(ssh "$remote" systemctl show --property=Result --value "${services[@]}")
if grep -Evq '^(success)?$' <<<"$results"; then
  printf 'Remote render stopped without success:\n%s\n' "$results" >&2
  exit 1
fi

exec "$source_root/tools/sync_remote_cache.sh"

#!/usr/bin/env bash
set -euo pipefail

systemctl --user stop fallout-tts-render-high.service 2>/dev/null || true
systemctl --user start fallout-tts-render.service

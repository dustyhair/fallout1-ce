---
name: fallout-tts
description: Set up, generate, audit, or troubleshoot the local Fallout CE dialogue voice cache. Use for this repository's TTS models, game-data extraction, voice cast, rendering backends, cache layout, or missing dialogue.
---

# Fallout dialogue TTS

Read `TTS.md` before changing or running the pipeline. Treat it as the human-facing source of truth.

Use `tools/generate_tts_cache.sh` for ordinary work. It owns setup, legal game-data extraction, script decompilation, voice-cast generation, and resumable rendering. Use `--backend auto` unless the user requests CPU or CUDA explicitly. Run it with `--dry-run` when only an inventory or parser audit is needed.

Keep these boundaries intact:

- Never add Fallout game data, generated model weights, reference prompts, or `TTS_CACHE` audio to Git.
- Do not delete existing clips when repairing a cache. The renderer skips completed files and safely retries missing or failed files.
- CPU means the Q4 ONNX Chatterbox Turbo export. CUDA means the official PyTorch Chatterbox Turbo implementation.
- Voice prompts come from Kokoro and are synthetic. Do not substitute recorded actor dialogue as cloning input.
- Dialogue discovery must follow `SCRIPTS.LST`; similarly named extracted files can be unused DOS-name alternatives.

For parser changes, run `PYTHONPATH=tools python3 -m unittest discover -s tools/tests -v`. For shell changes, also run `bash -n` on both setup and generation scripts and exercise each script's `--help` path.

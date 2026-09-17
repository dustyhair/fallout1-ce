# Native Linux dialogue voices

This unofficial Fallout Community Edition modification plays pre-rendered dialogue from an Opus cache. It keeps Fallout's original recorded lines and uses Speech Dispatcher when a generated clip is missing. Conversation replies, player choices, and floating dialogue above characters are supported.

The cache identifies each clip by speaker script, final text, role, and gender. NPC replies and player choices therefore do not share a voice by accident. Text-based keys also cover script-built replies when their final wording is known. At runtime the engine reads an NPC's gender from its critter data and the player's gender from the current character. Dialogue that inserts a custom player name is spoken with the stable name "Vault Dweller" while the chosen name remains visible on screen.

## Controls

- Move the pointer onto a dialogue option to read it aloud.
- `Ctrl+R`: repeat the last spoken line.
- `F9`: stop speech.
- `Ctrl+F8`: toggle text-to-speech. The setting is saved in `fallout.cfg` when the game exits normally.

## Configuration

The defaults are added automatically. They can be overridden in `fallout.cfg`:

```ini
[tts]
enabled=1
speak_options=1
rate=0
pitch=0
volume=0
language=en
voice=
output_module=
encoding=WINDOWS-1252
cache_path=TTS_CACHE
```

`cache_path` can be absolute or relative to the directory from which the game starts. Cached files use mono Opus at 40 kbps. This is roughly one tenth the size of uncompressed 24 kHz PCM while remaining transparent enough for dialogue.

`rate`, `pitch`, and `volume` apply to Speech Dispatcher and accept values from `-100` to `100`. An empty `voice` or `output_module` uses its default. The encoding setting converts the original game text to UTF-8.

You can inspect available voices with `spd-say -L` and test the system outside the game with:

```console
spd-say "Fallout speech test"
```

## Building on Debian or Ubuntu

Install a C++ toolchain, CMake, Ninja, SDL2 development files, FFmpeg development files, and Speech Dispatcher development files:

```console
sudo apt install build-essential cmake ninja-build libsdl2-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libspeechd-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The resulting executable is `build/fallout-ce`. Run it from a directory containing legally obtained Fallout 1 assets. No game data is included in this repository.

## Pre-rendering the cache

The tooling uses the official Chatterbox Turbo ONNX model with its Q4 CPU graphs. It decompiles the installed game scripts to separate NPC replies, player choices, and floating dialogue. Original voiced conversation lines are skipped. Floating lines are rendered because the game does not play their recorded-audio metadata. Large roles receive distinct synthetic voice prompts, while incidental roles share a voice suited to their script description. Ambiguous NPC scripts get male and female clips; the engine selects the matching one.

The process has three resumable stages:

1. Extract `MASTER.DAT` and decompile the files in `SCRIPTS` with `int2ssl -1`.
2. Run `tools/prepare_voice_cast.py` in a Python environment containing `kokoro-onnx`. This creates synthetic reference voices and `cast.json`.
3. Run `tools/precompute_tts.py` in a Python environment containing ONNX Runtime, Transformers, librosa, and soundfile. Existing `.opus` files are skipped, so restarting the command resumes the render.

Run either Python tool with `--help` for the required paths. The renderer's `--dry-run` option audits the number of referenced replies, choices, and pending gender variants without loading the model. Its JSON Lines manifest records the source script, role, gender, text, duration, and destination for every completed clip.

For the installation used during development, `tools/render_installed_game.sh` runs the cast and render stages together. It is safe to rerun. When launched as the `fallout-tts-render.service` user unit, inspect it with `systemctl --user status fallout-tts-render` and follow progress in `.tts-build/full-render.log`. Stop it with `systemctl --user stop fallout-tts-render`.

## Scope

The modification currently speaks conversation replies, choices, and floating dialogue. It does not yet read inventory descriptions, Pip-Boy pages, or the main interface.

# Native Linux dialogue voices

This unofficial Fallout Community Edition modification plays pre-rendered dialogue from an Opus cache. It keeps Fallout's original recorded lines and uses Speech Dispatcher when a generated clip is missing. Conversation replies, player choices, and floating dialogue above characters are supported.

The cache identifies each clip by speaker script, final text, role, and gender. NPC replies and player choices therefore do not share a voice by accident. Text-based keys also cover script-built replies when their final wording is known. At runtime the engine reads an NPC's gender from its critter data and the player's gender from the current character. Dialogue that inserts a custom player name is spoken with the stable name "Vault Dweller" while the chosen name remains visible on screen.

## Controls

- Select a dialogue option to speak it. The NPC waits for the selected line to finish, while the next options remain available on screen. Selecting another option stops the current speech and replaces the pending reply, so skipping ahead does not build a backlog.
- `Ctrl+R`: repeat the last spoken line.
- `F9`: stop the current speech and clear queued dialogue.
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

## Generating the dialogue cache

The repository does not distribute Fallout data, generated dialogue, or model weights. The generator works from your own Fallout 1 installation and writes the finished cache beside it. The installation directory must contain `MASTER.DAT`; files under `DATA` are treated as patches when present.

On Debian or Ubuntu, install the small set of system prerequisites:

```console
sudo apt install build-essential cmake git ffmpeg python3 python3-venv libboost-program-options-dev zlib1g-dev
```

Then run one command from this repository:

```console
./tools/generate_tts_cache.sh --backend auto "/path/to/Fallout"
```

`auto` selects CUDA when a working NVIDIA driver is visible and otherwise selects CPU. The first run creates isolated voice-cast and rendering environments under `Fallout/.fallout-ce-tts`, builds the two small data tools, downloads the required models, extracts `MASTER.DAT`, decompiles the scripts, creates a synthetic voice cast, and starts rendering. The separate environments avoid the incompatible NumPy requirements of Kokoro and Chatterbox. The final files go to `Fallout/TTS_CACHE`.

Every stage is resumable. Run the same command again after an interruption or a failed clip. Existing model files, extracted data, voice prompts, and completed Opus files are reused.

### CPU without CUDA

Force the portable CPU path with:

```console
./tools/generate_tts_cache.sh --backend cpu "/path/to/Fallout"
```

This uses the official [Chatterbox Turbo ONNX export](https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX) with only its Q4 graphs. It does not install PyTorch or require a GPU. Rendering the complete game is compute-intensive and can take a long time, but it can be stopped and resumed safely.

### NVIDIA CUDA

Use the faster CUDA path with:

```console
./tools/generate_tts_cache.sh --backend cuda "/path/to/Fallout"
```

This installs the official [Chatterbox Python package](https://github.com/resemble-ai/chatterbox) and a CUDA-enabled PyTorch 2.6 wheel in its own virtual environment. A system CUDA toolkit is not required, but the NVIDIA driver must support the bundled CUDA runtime. The default wheel uses CUDA 12.4. To select another official PyTorch wheel index, set it before the first setup:

```console
FALLOUT_TTS_TORCH_INDEX_URL=https://download.pytorch.org/whl/cu126 \
  ./tools/generate_tts_cache.sh --backend cuda "/path/to/Fallout"
```

If the CUDA check fails, the script stops with a direct error. Re-run with `--backend cpu` to use the GPU-independent path.

### Storage and customization

The CPU setup downloads about 850 MB of model files. CUDA uses several gigabytes for PyTorch and Chatterbox weights. Generated audio currently needs about 550 MB, while work files and Python environments remain under `.fallout-ce-tts`. Nothing in that directory needs to be committed or copied into this repository.

Useful options are available through `--help`:

```console
./tools/generate_tts_cache.sh --help
./tools/setup_tts_generator.sh --help
```

Use `--dry-run` to extract and index dialogue, prepare the cast, and report the number of clips without loading Chatterbox. `--threads` controls CPU inference threads. `--cast-jobs` controls parallel Kokoro prompt generation. `--shard-count` and `--shard-index` let several machines render deterministic portions of the same cache.

Set `FALLOUT_TTS_WORK_DIR` to keep models and temporary files outside the game directory. Set `FALLOUT_TTS_CACHE_DIR` to change the generated cache destination. If the latter is not `TTS_CACHE` under the game directory, set the matching `cache_path` in `fallout.cfg`.

The generator uses [Kokoro ONNX](https://github.com/thewh1teagle/kokoro-onnx) to create synthetic reference prompts. It never uses the original actors' recordings as cloning input. Original voiced conversation lines remain untouched and take precedence in the game.

For lower-level work, `tools/prepare_voice_cast.py` and `tools/precompute_tts.py` expose each stage directly. The renderer's JSON Lines manifest records the source script, role, gender, text, duration, and destination for each newly completed clip.

The repo-local agent workflow is in `.agents/skills/fallout-tts/SKILL.md`. It points automated contributors back to this document and preserves the legal-data, cache-safety, backend, and parser constraints without duplicating the setup procedure.

## Scope

The modification currently speaks conversation replies, choices, and floating dialogue. It does not yet read inventory descriptions, Pip-Boy pages, or the main interface.

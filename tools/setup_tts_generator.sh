#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
backend=auto
game_root=

usage() {
  cat <<'EOF'
Usage: tools/setup_tts_generator.sh [--backend auto|cpu|cuda] GAME_DIRECTORY

Create an isolated Python environment, build the Fallout data tools, and
download the voice models needed by tools/generate_tts_cache.sh.

Options:
  --backend MODE  auto (default), cpu, or cuda
  -h, --help      show this help

Environment:
  FALLOUT_TTS_WORK_DIR         override GAME_DIRECTORY/.fallout-ce-tts
  FALLOUT_TTS_PYTHON           Python 3.10 through 3.13 (default: python3)
  FALLOUT_TTS_TORCH_INDEX_URL  PyTorch wheel index for CUDA
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

while (($#)); do
  case "$1" in
    --backend)
      (($# >= 2)) || die "--backend requires a value"
      backend=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      (($# == 1)) || die "expected one game directory"
      game_root=$1
      shift
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      [[ -z $game_root ]] || die "expected one game directory"
      game_root=$1
      shift
      ;;
  esac
done

[[ -n $game_root ]] || { usage >&2; exit 2; }
case "$backend" in
  auto|cpu|cuda) ;;
  *) die "backend must be auto, cpu, or cuda" ;;
esac

game_root=$(realpath -m "$game_root")
[[ -d $game_root ]] || die "game directory does not exist: $game_root"

if [[ $backend == auto ]]; then
  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
    backend=cuda
  else
    backend=cpu
  fi
fi

for command_name in cmake c++ ffmpeg git make; do
  command -v "$command_name" >/dev/null 2>&1 || die "missing required command: $command_name"
done

python_command=${FALLOUT_TTS_PYTHON:-python3}
command -v "$python_command" >/dev/null 2>&1 || die "missing Python command: $python_command"
"$python_command" - <<'PY' || die "Python 3.10 through 3.13 is required"
import sys
raise SystemExit(0 if (3, 10) <= sys.version_info < (3, 14) else 1)
PY

work_root=${FALLOUT_TTS_WORK_DIR:-"$game_root/.fallout-ce-tts"}
work_root=$(realpath -m "$work_root")
cast_venv="$work_root/venv-cast"
render_venv="$work_root/venv-$backend"
bin_dir="$work_root/bin"
source_dir="$work_root/sources"
build_dir="$work_root/build"
models_dir="$work_root/models"
mkdir -p "$bin_dir" "$source_dir" "$build_dir" "$models_dir"

if [[ ! -x $cast_venv/bin/python ]]; then
  printf 'Creating voice-cast environment in %s\n' "$cast_venv"
  "$python_command" -m venv "$cast_venv" || die "could not create a virtual environment; install the Python venv package"
fi
cast_fingerprint=$(sha256sum "$source_root/tools/requirements-tts-cast.txt" | cut -d' ' -f1)
cast_marker="$cast_venv/.fallout-ce-tts-cast-$cast_fingerprint"
if [[ ! -f $cast_marker ]]; then
  "$cast_venv/bin/python" -m pip install --upgrade pip
  "$cast_venv/bin/python" -m pip install -r "$source_root/tools/requirements-tts-cast.txt"
  touch "$cast_marker"
fi

if [[ ! -x $render_venv/bin/python ]]; then
  printf 'Creating %s renderer environment in %s\n' "$backend" "$render_venv"
  "$python_command" -m venv "$render_venv" || die "could not create a virtual environment; install the Python venv package"
fi
if [[ $backend == cuda ]]; then
  render_requirements="$source_root/tools/requirements-tts-cuda.txt"
  torch_index=${FALLOUT_TTS_TORCH_INDEX_URL:-https://download.pytorch.org/whl/cu124}
else
  render_requirements="$source_root/tools/requirements-tts-cpu.txt"
  torch_index=
fi
requirements_digest=$(sha256sum "$render_requirements" | cut -d' ' -f1)
render_fingerprint=$(printf '%s\n%s\n' "$requirements_digest" "$torch_index" | sha256sum | cut -d' ' -f1)
render_marker="$render_venv/.fallout-ce-tts-$backend-$render_fingerprint"
if [[ ! -f $render_marker ]]; then
  "$render_venv/bin/python" -m pip install --upgrade pip
  if [[ $backend == cuda ]]; then
    "$render_venv/bin/python" -m pip install \
      --index-url "$torch_index" torch==2.6.0 torchaudio==2.6.0
  fi
  "$render_venv/bin/python" -m pip install -r "$render_requirements"
fi

if [[ $backend == cuda ]]; then
  if ! "$render_venv/bin/python" - <<'PY'
import torch
raise SystemExit(0 if torch.cuda.is_available() else 1)
PY
  then
    die "CUDA was selected, but PyTorch cannot use the GPU. Retry with --backend cpu or set FALLOUT_TTS_TORCH_INDEX_URL for a compatible PyTorch wheel."
  fi
fi
touch "$render_marker"

clone_at_revision() {
  local repository=$1
  local revision=$2
  local destination=$3
  if [[ ! -d $destination/.git ]]; then
    git clone --filter=blob:none --no-checkout "$repository" "$destination"
  fi
  if [[ ! -f $destination/CMakeLists.txt || $(git -C "$destination" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
    git -C "$destination" fetch --depth 1 origin "$revision"
    git -C "$destination" checkout --detach "$revision"
  fi
}

if [[ ! -x $bin_dir/dat-unpacker ]]; then
  clone_at_revision \
    https://github.com/falltergeist/dat-unpacker.git \
    d536ef630d9a47e866cddd7e826480c3ac34bb3d \
    "$source_dir/dat-unpacker"
  if ! cmake -S "$source_dir/dat-unpacker" -B "$build_dir/dat-unpacker" -DCMAKE_BUILD_TYPE=Release; then
    die "dat-unpacker needs the Boost program_options and zlib development packages; see TTS.md"
  fi
  cmake --build "$build_dir/dat-unpacker" --parallel
  cp "$build_dir/dat-unpacker/dat-unpacker" "$bin_dir/dat-unpacker"
fi

if [[ ! -x $bin_dir/int2ssl ]]; then
  clone_at_revision \
    https://github.com/falltergeist/int2ssl.git \
    438fef9bfcaff0e0b9e59d7ac7fbf21603dc75c6 \
    "$source_dir/int2ssl"
  cmake -S "$source_dir/int2ssl" -B "$build_dir/int2ssl" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir/int2ssl" --parallel
  cp "$build_dir/int2ssl/int2ssl" "$bin_dir/int2ssl"
fi

export HF_HOME="$work_root/huggingface"
"$render_venv/bin/python" "$source_root/tools/download_tts_models.py" \
  --models-dir "$models_dir" \
  --backend "$backend"

printf '\nTTS generator is ready.\n'
printf 'Backend: %s\n' "$backend"
printf 'Work directory: %s\n' "$work_root"

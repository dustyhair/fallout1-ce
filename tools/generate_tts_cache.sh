#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
backend=auto
cast_jobs=${FALLOUT_CAST_JOBS:-4}
render_threads=${FALLOUT_RENDER_THREADS:-8}
shard_count=${FALLOUT_SHARD_COUNT:-1}
shard_index=${FALLOUT_SHARD_INDEX:-0}
dry_run=0
game_root=

usage() {
  cat <<'EOF'
Usage: tools/generate_tts_cache.sh [OPTIONS] GAME_DIRECTORY

Generate a resumable Fallout 1 dialogue cache from legally owned game data.

Options:
  --backend MODE    auto (default), cpu, or cuda
  --cast-jobs N     parallel Kokoro prompt jobs (default: 4)
  --threads N       inference threads used by each renderer (default: 8)
  --shard-count N   split the cache into N deterministic shards (default: 1)
  --shard-index N   zero-based shard rendered by this process (default: 0)
  --dry-run         index dialogue and report work without rendering clips
  -h, --help        show this help

The first run creates GAME_DIRECTORY/.fallout-ce-tts, downloads the selected
models, extracts MASTER.DAT, and decompiles its scripts. Existing voice prompts
and Opus clips are retained, so an interrupted run resumes where it stopped.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

positive_integer() {
  [[ $1 =~ ^[0-9]+$ ]] && ((10#$1 > 0))
}

nonnegative_integer() {
  [[ $1 =~ ^[0-9]+$ ]]
}

while (($#)); do
  case "$1" in
    --backend)
      (($# >= 2)) || die "--backend requires a value"
      backend=$2
      shift 2
      ;;
    --cast-jobs)
      (($# >= 2)) || die "--cast-jobs requires a value"
      cast_jobs=$2
      shift 2
      ;;
    --threads)
      (($# >= 2)) || die "--threads requires a value"
      render_threads=$2
      shift 2
      ;;
    --shard-count)
      (($# >= 2)) || die "--shard-count requires a value"
      shard_count=$2
      shift 2
      ;;
    --shard-index)
      (($# >= 2)) || die "--shard-index requires a value"
      shard_index=$2
      shift 2
      ;;
    --dry-run)
      dry_run=1
      shift
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
positive_integer "$cast_jobs" || die "--cast-jobs must be a positive integer"
positive_integer "$render_threads" || die "--threads must be a positive integer"
positive_integer "$shard_count" || die "--shard-count must be a positive integer"
nonnegative_integer "$shard_index" || die "--shard-index must be a nonnegative integer"
((10#$shard_index < 10#$shard_count)) || die "--shard-index must be smaller than --shard-count"

game_root=$(realpath -m "$game_root")
[[ -d $game_root ]] || die "game directory does not exist: $game_root"

if [[ $backend == auto ]]; then
  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
    backend=cuda
  else
    backend=cpu
  fi
fi

"$source_root/tools/setup_tts_generator.sh" --backend "$backend" "$game_root"

work_root=${FALLOUT_TTS_WORK_DIR:-"$game_root/.fallout-ce-tts"}
work_root=$(realpath -m "$work_root")
cast_venv="$work_root/venv-cast"
render_venv="$work_root/venv-$backend"
extracted_root="$work_root/master-extracted"
ssl_dir="$work_root/ssl"
cast_dir="$work_root/cast"
models_dir="$work_root/models"
cache_dir=${FALLOUT_TTS_CACHE_DIR:-"$game_root/TTS_CACHE"}

master_dat=$(find -L "$game_root" -maxdepth 2 -type f -iname 'master.dat' -print -quit)
[[ -n $master_dat ]] || die "could not find MASTER.DAT below $game_root"

scripts_list="$extracted_root/SCRIPTS/SCRIPTS.LST"
if [[ ! -f $scripts_list ]]; then
  printf '\nExtracting %s...\n' "$master_dat"
  mkdir -p "$extracted_root"
  "$work_root/bin/dat-unpacker" \
    --format dat1 \
    --source "$master_dat" \
    --destination "$extracted_root"
fi
[[ -f $scripts_list ]] || die "MASTER.DAT did not contain SCRIPTS/SCRIPTS.LST"

mkdir -p "$ssl_dir"
decompiled=0
failed=0
while IFS= read -r -d '' input; do
  name=$(basename "$input")
  output="$ssl_dir/${name%.*}.ssl"
  if [[ -f $output && $output -nt $input ]]; then
    continue
  fi
  if "$work_root/bin/int2ssl" -1 "$input" "$output" >/dev/null 2>&1; then
    ((decompiled += 1))
  else
    : > "$output"
    ((failed += 1))
  fi
done < <(find "$extracted_root/SCRIPTS" -maxdepth 1 -type f -iname '*.INT' -print0 | sort -z)
printf 'Decompiled %d changed scripts; %d unsupported scripts were indexed conservatively.\n' "$decompiled" "$failed"

dialog_source="$extracted_root/TEXT/ENGLISH/DIALOG"
[[ -d $dialog_source ]] || die "MASTER.DAT did not contain English dialogue messages"
patch_source=
for candidate in \
  "$game_root/DATA/TEXT/ENGLISH/DIALOG" \
  "$game_root/data/text/english/dialog"; do
  if [[ -d $candidate ]]; then
    patch_source=$candidate
    break
  fi
done
common_arguments=(
  --source "$dialog_source"
  --scripts-list "$scripts_list"
  --ssl-dir "$ssl_dir"
)
if [[ -n $patch_source ]]; then
  common_arguments+=(--patch-source "$patch_source")
fi

export PYTHONPATH="$source_root/tools"
export HF_HOME="$work_root/huggingface"
printf '\nPreparing the reusable voice cast...\n'
"$cast_venv/bin/python" "$source_root/tools/prepare_voice_cast.py" \
  "${common_arguments[@]}" \
  --output "$cast_dir" \
  --kokoro-model "$models_dir/kokoro/kokoro-v1.0.int8.onnx" \
  --kokoro-voices "$models_dir/kokoro/voices-v1.0.bin" \
  --jobs "$cast_jobs"

render_arguments=(
  "${common_arguments[@]}"
  --cache "$cache_dir"
  --cast "$cast_dir/cast.json"
  --model-dir "$models_dir/chatterbox-turbo-onnx"
  --threads "$render_threads"
  --shard-count "$shard_count"
  --shard-index "$shard_index"
)
if [[ $backend == cuda ]]; then
  render_arguments+=(--backend torch --device cuda)
else
  render_arguments+=(--backend onnx --device cpu --quantization q4)
fi
if ((dry_run)); then
  render_arguments+=(--dry-run)
fi

if ((dry_run)); then
  printf '\nIndexing dialogue with the %s backend selected...\n' "$backend"
else
  printf '\nIndexing dialogue and generating missing clips with the %s backend...\n' "$backend"
fi
"$render_venv/bin/python" -u "$source_root/tools/precompute_tts.py" "${render_arguments[@]}"
if ((dry_run)); then
  printf '\nDry run complete. Generated clips would be written to %s\n' "$cache_dir"
else
  printf '\nCache ready at %s\n' "$cache_dir"
fi

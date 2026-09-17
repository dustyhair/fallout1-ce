#!/usr/bin/env python3
"""Render Fallout dialogue to a resumable, gender-aware Opus cache."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import wave

from dialog_data import DialogRecord, load_dialogue


def cache_key(text: str) -> str:
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def encode_opus(samples: bytes, sample_rate: int, destination: Path, bitrate: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".opus.part")
    subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "f32le",
            "-ar", str(sample_rate), "-ac", "1", "-i", "pipe:0", "-c:a",
            "libopus", "-b:a", bitrate, "-application", "voip", "-vbr", "on",
            "-compression_level", "10", "-f", "opus", "-y", str(temporary),
        ],
        input=samples,
        check=True,
    )
    os.replace(temporary, destination)


def torch_prompt(source: Path, cast_root: Path) -> Path:
    """Pad short PCM references to Chatterbox's five-second minimum."""
    with wave.open(str(source), "rb") as stream:
        parameters = stream.getparams()
        frames = stream.readframes(parameters.nframes)
    minimum_frames = int(parameters.framerate * 5.25)
    if parameters.nframes >= minimum_frames:
        return source

    relative = source.relative_to(cast_root)
    destination = cast_root / ".torch-prompts" / relative
    if destination.exists() and destination.stat().st_mtime_ns >= source.stat().st_mtime_ns:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".wav.part")
    silence_frames = minimum_frames - parameters.nframes
    with wave.open(str(temporary), "wb") as stream:
        stream.setparams(parameters)
        stream.writeframes(frames)
        stream.writeframes(b"\0" * silence_frames * parameters.nchannels * parameters.sampwidth)
    os.replace(temporary, destination)
    return destination


def jobs_for(record: DialogRecord, cache: Path, cast: dict, cast_root: Path):
    if "npc" in record.roles and (not record.audio or "floating" in record.roles):
        profile = cast["scripts"].get(str(record.list_id))
        if profile:
            genders = ("neutral",) if profile["archetype"] == "machine" else ("male", "female")
            for gender in genders:
                code = {"male": "m", "female": "f", "neutral": "n"}[gender]
                yield {
                    "role": "npc",
                    "gender": gender,
                    "prompt": cast_root / profile[gender],
                    "path": cache / "dialog" / "npc" / str(record.list_id) / f"{cache_key(record.text)}-{code}.opus",
                }
    if "player" in record.roles:
        for gender in ("male", "female"):
            profile = cast["player"][gender]
            code = "m" if gender == "male" else "f"
            yield {
                "role": "player",
                "gender": gender,
                "prompt": cast_root / profile,
                "path": cache / "dialog" / "player" / f"{cache_key(record.text)}-{code}.opus",
            }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--patch-source", type=Path)
    parser.add_argument("--scripts-list", type=Path, required=True)
    parser.add_argument("--ssl-dir", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--cast", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--backend", choices=("onnx", "torch"), default="onnx")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--quantization", choices=("q4", "q4f16", "q8"), default="q4")
    parser.add_argument("--bitrate", default="40k")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--shard-count", type=int, default=1)
    parser.add_argument(
        "--shard-index",
        type=int,
        action="append",
        dest="shard_indices",
        help="Zero-based shard to render; repeat to assign several shards to one process",
    )
    parser.add_argument("--script", action="append", help="Only render these script names")
    parser.add_argument("--player-name-lines-only", action="store_true")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    shard_indices = set(args.shard_indices or [0])
    if args.shard_count < 1 or any(not 0 <= index < args.shard_count for index in shard_indices):
        parser.error("each --shard-index must be between zero and --shard-count minus one")

    cast = json.loads(args.cast.read_text())
    records = load_dialogue(args.source, args.patch_source, args.scripts_list, args.ssl_dir)
    if args.script:
        selected = {name.upper() for name in args.script}
        records = [record for record in records if record.script in selected]
    if args.player_name_lines_only:
        records = [record for record in records if "player_name" in record.roles]
    jobs_by_path = {}
    for record in records:
        for job in jobs_for(record, args.cache, cast, args.cast.parent):
            jobs_by_path[job["path"]] = (record, job)
    sharded = {
        path: item
        for path, item in jobs_by_path.items()
        if int(cache_key(str(path.relative_to(args.cache))), 16) % args.shard_count in shard_indices
    }
    pending = [item for path, item in sharded.items() if not path.exists()]
    if args.backend == "torch":
        pending.sort(key=lambda item: (str(item[1]["prompt"]), str(item[1]["path"])))
    if args.limit is not None:
        pending = pending[: args.limit]

    npc = sum(
        "npc" in record.roles and (not record.audio or "floating" in record.roles)
        for record in records
    )
    player = sum("player" in record.roles for record in records)
    floating = sum("floating" in record.roles for record in records)
    print(
        f"Indexed {npc} NPC lines, {player} player lines, and {floating} floating lines; "
        f"shards {','.join(str(index + 1) for index in sorted(shard_indices))}/{args.shard_count} "
        f"own {len(sharded)} clips and "
        f"{len(pending)} need generation.",
        flush=True,
    )
    if args.dry_run or not pending:
        return 0

    if args.backend == "onnx":
        from chatterbox_onnx import ChatterboxOnnx

        model = ChatterboxOnnx(args.model_dir, args.quantization, args.threads)
        sample_rate = model.sample_rate
    else:
        import torch
        from chatterbox.tts_turbo import ChatterboxTurboTTS

        torch.set_num_threads(args.threads)
        model = ChatterboxTurboTTS.from_pretrained(device=args.device, nano=False)
        sample_rate = model.sr
    voices = {}
    current_torch_prompt = None
    manifest = args.cache / "manifest.jsonl"
    args.cache.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    generated_audio = 0.0
    failed = 0

    for index, (record, job) in enumerate(pending, 1):
        try:
            prompt = job["prompt"]
            speech_text = record.text.replace("_", "...")
            if args.backend == "onnx":
                if prompt not in voices:
                    voices[prompt] = model.prepare_voice(prompt)
                samples = model.generate(speech_text, voices[prompt])
            else:
                if prompt != current_torch_prompt:
                    prepared_prompt = torch_prompt(prompt, args.cast.parent)
                    model.prepare_conditionals(str(prepared_prompt), exaggeration=0.0)
                    current_torch_prompt = prompt
                samples = model.generate(speech_text).squeeze().detach().cpu().numpy().astype("float32", copy=False)
            encode_opus(samples.tobytes(), sample_rate, job["path"], args.bitrate)
        except Exception as error:
            failed += 1
            print(
                f"[{index}/{len(pending)}] FAILED {record.script} "
                f"{job['role']}/{job['gender']}: {error}",
                flush=True,
            )
            continue
        duration = len(samples) / sample_rate
        generated_audio += duration
        with manifest.open("a", encoding="utf-8") as stream:
            stream.write(
                json.dumps(
                    {
                        "list": record.list_id,
                        "message_list": record.message_list_id,
                        "message": record.message_id,
                        "role": job["role"],
                        "gender": job["gender"],
                        "script": record.script,
                        "seconds": duration,
                        "path": str(job["path"].relative_to(args.cache)),
                        "text": record.text,
                    }
                )
                + "\n"
            )
        elapsed = time.monotonic() - started
        print(
            f"[{index}/{len(pending)}] {duration:.1f}s, {generated_audio / elapsed:.2f}x realtime, "
            f"{record.script} {job['role']}/{job['gender']}: {record.text[:60]}",
            flush=True,
        )
    if failed:
        print(f"Completed with {failed} failed clips; rerunning will retry them.", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

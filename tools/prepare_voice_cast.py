#!/usr/bin/env python3
"""Create synthetic, gendered voice prompts and a Fallout dialogue cast file."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import multiprocessing
import os
from pathlib import Path

import numpy as np
import soundfile as sf

from dialog_data import load_dialogue


MALE_VOICES = [
    "am_adam", "am_echo", "am_eric", "am_fenrir", "am_liam", "am_michael",
    "am_onyx", "am_puck", "bm_daniel", "bm_fable", "bm_george", "bm_lewis",
]
FEMALE_VOICES = [
    "af_alloy", "af_aoede", "af_bella", "af_heart", "af_jessica", "af_kore",
    "af_nicole", "af_nova", "af_river", "af_sarah", "af_sky", "bf_alice",
    "bf_emma", "bf_isabella", "bf_lily",
]

ARCHETYPES = {
    "machine": ("computer", "terminal", "robot", "machine", "holodisk"),
    "mutant": ("mutant", "super mutant", "master", "lieutenant"),
    "ghoul": ("ghoul", "necropol", "glowing one"),
    "measured": ("elder", "scribe", "doctor", "scientist", "overseer"),
    "guarded": ("guard", "sentry", "police", "law enforcement"),
    "hostile": ("raider", "ganger", "merc", "assassin", "thug", "killer"),
    "fanatic": ("priest", "cathedral", "children", "cult", "morpheus"),
    "young": (" child", "kid", "tandi"),
}

ARCHETYPE_OVERRIDES = {
    "CALDER": "fanatic",
    "CHDSCOUT": "fanatic",
    "DANE": "fanatic",
    "DOCWU": "measured",
    "GABRIEL": "guarded",
    "GIDEON": "weary",
    "HEATHER": "fanatic",
    "JEREM": "weary",
    "JUSTIN": "guarded",
    "KANE": "hostile",
    "LASHER": "fanatic",
    "OFFICER": "guarded",
    "RAZOR": "hostile",
    "SLUMMER": "fanatic",
    "THORNDYK": "measured",
    "TYCHO": "guarded",
    "VIOLA": "fanatic",
}

PROMPT_TEXT = {
    "weary": "Another hard day in the dust. Save your breath, keep your eyes open, and keep moving.",
    "mutant": "The wastes made us strong. Speak plainly, little human, before my patience is gone.",
    "ghoul": "I've watched the dust settle for a long time. Sit down, if you can stand the smell.",
    "hostile": "You picked a bad road and a worse time. Hand it over, or this gets ugly.",
    "guarded": "That's close enough. State your business, keep your hands where I can see them.",
    "young": "Everybody says the old world is gone. I still want to know what was out there.",
    "measured": "We have survived by thinking before we act. There is no room left for careless choices.",
    "fanatic": "The old world burned for its sins. We alone understand what must rise from the ashes.",
    "machine": "System status degraded. State your request. Authorization remains required.",
}


def stable_number(value: str) -> int:
    result = 14695981039346656037
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result


def archetype(name: str, comment: str) -> str:
    if name in ARCHETYPE_OVERRIDES:
        return ARCHETYPE_OVERRIDES[name]
    description = f"{name} {comment}".lower()
    for kind, words in ARCHETYPES.items():
        if any(word in description for word in words):
            return kind
    return "weary"


def blended_style(kokoro, voices: list[str], seed: int) -> np.ndarray:
    first = voices[seed % len(voices)]
    second = voices[(seed // len(voices) + 3) % len(voices)]
    weight = 0.25 + ((seed >> 8) % 51) / 100
    return kokoro.get_voice_style(first) * weight + kokoro.get_voice_style(second) * (1 - weight)


_kokoro = None


def init_worker(model: str, voices: str) -> None:
    global _kokoro
    from kokoro_onnx import Kokoro

    _kokoro = Kokoro(model, voices)


def render_prompt(job: tuple[str, str, str, float]) -> str:
    destination_text, identity, gender, speed = job
    destination = Path(destination_text)
    if destination.exists():
        return destination.name
    pool = MALE_VOICES if gender in {"male", "neutral"} else FEMALE_VOICES
    kind = identity.split("|", 1)[0]
    seed_name = identity.split("|", 1)[1]
    style = blended_style(_kokoro, pool, stable_number(seed_name))
    audio, rate = _kokoro.create(PROMPT_TEXT[kind], style, speed=speed)
    temporary = destination.with_suffix(".wav.part")
    sf.write(temporary, audio, rate, format="WAV")
    os.replace(temporary, destination)
    return destination.name


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--patch-source", type=Path)
    parser.add_argument("--scripts-list", type=Path, required=True)
    parser.add_argument("--ssl-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kokoro-model", type=Path, required=True)
    parser.add_argument("--kokoro-voices", type=Path, required=True)
    parser.add_argument("--unique-threshold", type=int, default=25)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()

    records = load_dialogue(args.source, args.patch_source, args.scripts_list, args.ssl_dir)
    counts = Counter(
        record.list_id
        for record in records
        if "npc" in record.roles and (not record.audio or "floating" in record.roles)
    )
    metadata = {}
    for record in records:
        metadata.setdefault(record.list_id, (record.script, record.comment))

    args.output.mkdir(parents=True, exist_ok=True)
    prompt_dir = args.output / "prompts"
    prompt_dir.mkdir(exist_ok=True)
    cast = {"version": 1, "unique_threshold": args.unique_threshold, "player": {}, "scripts": {}}
    prompt_jobs = []

    for gender, pool in (("male", MALE_VOICES), ("female", FEMALE_VOICES)):
        identity = f"player-{gender}"
        destination = prompt_dir / f"{identity}.wav"
        prompt_jobs.append((str(destination), f"weary|{identity}", gender, 0.96))
        cast["player"][gender] = str(destination.relative_to(args.output))

    for list_id, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        name, comment = metadata[list_id]
        kind = archetype(name, comment)
        entry = {"name": name, "comment": comment, "archetype": kind, "npc_lines": count}
        genders = ("neutral",) if kind == "machine" else ("male", "female")
        for gender in genders:
            voice_gender = "male" if gender == "neutral" else gender
            pool = MALE_VOICES if voice_gender == "male" else FEMALE_VOICES
            identity = f"{list_id}-{kind}-{gender}" if count >= args.unique_threshold else f"generic-{kind}-{gender}"
            destination = prompt_dir / f"{identity}.wav"
            speed = 0.91 if kind in {"weary", "ghoul", "mutant"} else 1.0
            prompt_jobs.append((str(destination), f"{kind}|{identity}", gender, speed))
            entry[gender] = str(destination.relative_to(args.output))
        cast["scripts"][str(list_id)] = entry

    (args.output / "cast.json").write_text(json.dumps(cast, indent=2) + "\n")
    pending = {job[0]: job for job in prompt_jobs if not Path(job[0]).exists()}
    if pending:
        with multiprocessing.Pool(
            processes=args.jobs,
            initializer=init_worker,
            initargs=(str(args.kokoro_model), str(args.kokoro_voices)),
        ) as workers:
            for index, name in enumerate(workers.imap_unordered(render_prompt, pending.values()), 1):
                print(f"[{index}/{len(pending)}] Voice prompt: {name}", flush=True)
    unique = sum(1 for entry in cast["scripts"].values() if entry["npc_lines"] >= args.unique_threshold)
    print(f"Created cast for {len(cast['scripts'])} speaking scripts; {unique} receive unique voices.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

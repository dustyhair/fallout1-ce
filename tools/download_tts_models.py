#!/usr/bin/env python3
"""Download the model files used by the Fallout dialogue renderer."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import urllib.request


KOKORO_FILES = {
    "kokoro-v1.0.int8.onnx": (
        "https://github.com/thewh1teagle/kokoro-onnx/releases/download/"
        "model-files-v1.1/kokoro-v1.0.int8.onnx",
        "ae315a79b623f244700e4afb9246c46a26066782e049ba174bf3ba433970ee9c",
    ),
    "voices-v1.0.bin": (
        "https://github.com/thewh1teagle/kokoro-onnx/releases/download/"
        "model-files-v1.1/voices-v1.0.bin",
        "bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d",
    ),
}

CHATTERBOX_REPOSITORY = "ResembleAI/chatterbox-turbo-ONNX"
CHATTERBOX_REVISION = "d21799bd0354adb85e348b8a0442a8405110a2cf"
CHATTERBOX_FILES = [
    "config.json",
    "generation_config.json",
    "preprocessor_config.json",
    "tokenizer.json",
    "tokenizer_config.json",
    "onnx/conditional_decoder_q4.onnx",
    "onnx/conditional_decoder_q4.onnx_data",
    "onnx/embed_tokens_q4.onnx",
    "onnx/embed_tokens_q4.onnx_data",
    "onnx/language_model_q4.onnx",
    "onnx/language_model_q4.onnx_data",
    "onnx/speech_encoder_q4.onnx",
    "onnx/speech_encoder_q4.onnx_data",
]


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def download_file(url: str, destination: Path, expected_digest: str) -> None:
    if destination.is_file() and digest(destination) == expected_digest:
        print(f"Already downloaded: {destination.name}", flush=True)
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    temporary.unlink(missing_ok=True)
    print(f"Downloading {destination.name}...", flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "fallout1-ce-tts"})
    with urllib.request.urlopen(request) as response, temporary.open("wb") as output:
        while block := response.read(1024 * 1024):
            output.write(block)
    actual_digest = digest(temporary)
    if actual_digest != expected_digest:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(
            f"checksum mismatch for {destination.name}: expected {expected_digest}, "
            f"received {actual_digest}"
        )
    os.replace(temporary, destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--models-dir", type=Path, required=True)
    parser.add_argument("--backend", choices=("cpu", "cuda"), required=True)
    args = parser.parse_args()

    kokoro_dir = args.models_dir / "kokoro"
    for name, (url, expected_digest) in KOKORO_FILES.items():
        download_file(url, kokoro_dir / name, expected_digest)

    if args.backend == "cpu":
        from huggingface_hub import snapshot_download

        chatterbox_dir = args.models_dir / "chatterbox-turbo-onnx"
        missing = [name for name in CHATTERBOX_FILES if not (chatterbox_dir / name).is_file()]
        if missing:
            print("Downloading the Chatterbox Turbo Q4 ONNX model...", flush=True)
            snapshot_download(
                repo_id=CHATTERBOX_REPOSITORY,
                revision=CHATTERBOX_REVISION,
                local_dir=chatterbox_dir,
                allow_patterns=CHATTERBOX_FILES,
            )
        missing = [name for name in CHATTERBOX_FILES if not (chatterbox_dir / name).is_file()]
        if missing:
            raise RuntimeError("model download is incomplete: " + ", ".join(missing))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

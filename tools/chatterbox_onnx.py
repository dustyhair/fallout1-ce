#!/usr/bin/env python3
"""Reusable CPU inference wrapper for the official Chatterbox Turbo ONNX model."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import librosa
import numpy as np
import onnxruntime as ort
from transformers import AutoTokenizer


SAMPLE_RATE = 24_000
START_SPEECH_TOKEN = 6561
STOP_SPEECH_TOKEN = 6562
SILENCE_TOKEN = 4299
NUM_KV_HEADS = 16
HEAD_DIM = 64


@dataclass
class VoiceConditioning:
    cond_embedding: np.ndarray
    prompt_token: np.ndarray
    speaker_embeddings: np.ndarray
    speaker_features: np.ndarray


class ChatterboxOnnx:
    def __init__(self, model_dir: Path, quantization: str = "q4", threads: int = 8):
        suffix = "_quantized" if quantization == "q8" else f"_{quantization}"
        options = ort.SessionOptions()
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL

        def session(name: str) -> ort.InferenceSession:
            return ort.InferenceSession(
                str(model_dir / "onnx" / f"{name}{suffix}.onnx"),
                sess_options=options,
                providers=["CPUExecutionProvider"],
            )

        self.tokenizer = AutoTokenizer.from_pretrained(model_dir, local_files_only=True)
        self.speech_encoder = session("speech_encoder")
        self.embed_tokens = session("embed_tokens")
        self.language_model = session("language_model")
        self.decoder = session("conditional_decoder")
        self.sample_rate = SAMPLE_RATE

    def prepare_voice(self, audio_path: Path) -> VoiceConditioning:
        audio, _ = librosa.load(audio_path, sr=SAMPLE_RATE, mono=True)
        values = audio[np.newaxis, :].astype(np.float32)
        cond, prompt, embeddings, features = self.speech_encoder.run(
            None, {"audio_values": values}
        )
        return VoiceConditioning(cond, prompt, embeddings, features)

    @staticmethod
    def _apply_repetition_penalty(
        generated: np.ndarray, logits: np.ndarray, penalty: float
    ) -> np.ndarray:
        selected = np.take_along_axis(logits, generated, axis=1)
        selected = np.where(selected < 0, selected * penalty, selected / penalty)
        result = logits.copy()
        np.put_along_axis(result, generated, selected, axis=1)
        return result

    def generate(
        self,
        text: str,
        voice: VoiceConditioning,
        max_new_tokens: int = 1024,
        repetition_penalty: float = 1.2,
    ) -> np.ndarray:
        input_ids = self.tokenizer(text, return_tensors="np")["input_ids"].astype(np.int64)
        generated = np.array([[START_SPEECH_TOKEN]], dtype=np.int64)
        past = None
        stopped = False

        for step in range(max_new_tokens):
            embeddings = self.embed_tokens.run(None, {"input_ids": input_ids})[0]
            if step == 0:
                embeddings = np.concatenate((voice.cond_embedding, embeddings), axis=1)
                batch_size, sequence_length, _ = embeddings.shape
                past = {
                    item.name: np.zeros(
                        [batch_size, NUM_KV_HEADS, 0, HEAD_DIM],
                        dtype=np.float16 if item.type == "tensor(float16)" else np.float32,
                    )
                    for item in self.language_model.get_inputs()
                    if "past_key_values" in item.name
                }
                attention_mask = np.ones((batch_size, sequence_length), dtype=np.int64)
                position_ids = np.arange(sequence_length, dtype=np.int64)[np.newaxis, :]

            outputs = self.language_model.run(
                None,
                {
                    "inputs_embeds": embeddings,
                    "attention_mask": attention_mask,
                    "position_ids": position_ids,
                    **past,
                },
            )
            logits = self._apply_repetition_penalty(
                generated, outputs[0][:, -1, :], repetition_penalty
            )
            input_ids = np.argmax(logits, axis=-1, keepdims=True).astype(np.int64)
            generated = np.concatenate((generated, input_ids), axis=1)

            if (input_ids == STOP_SPEECH_TOKEN).all():
                stopped = True
                break

            attention_mask = np.concatenate(
                (attention_mask, np.ones((batch_size, 1), dtype=np.int64)), axis=1
            )
            position_ids = position_ids[:, -1:] + 1
            for index, key in enumerate(past):
                past[key] = outputs[index + 1]

        end = -1 if stopped else None
        speech_tokens = generated[:, 1:end]
        silence = np.full((speech_tokens.shape[0], 3), SILENCE_TOKEN, dtype=np.int64)
        speech_tokens = np.concatenate((voice.prompt_token, speech_tokens, silence), axis=1)
        audio = self.decoder.run(
            None,
            {
                "speech_tokens": speech_tokens,
                "speaker_embeddings": voice.speaker_embeddings,
                "speaker_features": voice.speaker_features,
            },
        )[0]
        return audio.squeeze().astype(np.float32, copy=False)

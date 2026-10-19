# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Local vision-language descriptions for ``describe_worst_frames`` (ADR-1886).

The model runs through ONNX Runtime GenAI (``onnxruntime-genai``, the ``vlm``
extra) from a model directory on this machine that ``VMAF_MCP_VLM_MODEL``
names, for example the CPU build of Phi-3.5-vision-instruct-onnx. Nothing is
downloaded and no model code runs: the directory holds ONNX graphs, a
``genai_config.json`` and a tokenizer. Without the extra or the directory the
tool returns frame metadata only, with a note that says what is missing.

torch and transformers used to provide this (SmolVLM, then Moondream2, fetched
from a model hub at first use with ``trust_remote_code``); torch now lives only
in the training environments (``ai/``, ``tools/ensemble-training-kit/``).
"""

from __future__ import annotations

import json
import os
from collections.abc import Callable
from pathlib import Path
from typing import Any

#: Environment variable that names the local model directory.
MODEL_ENV = "VMAF_MCP_VLM_MODEL"

#: Token budget of one description (prompt plus answer).
MAX_LENGTH = 4096

PROMPT = (
    "Describe what visible compression / encoding artefacts you see in this video "
    "frame in 1-2 sentences. Focus on blocking, ringing, banding, blur, or chroma "
    "distortion if present. Skip aesthetic commentary."
)

#: The image placeholder each model family's processor expects in front of
#: the prompt, per ``model.type`` (onnxruntime-genai's examples/python/common.py).
#: Families not listed take the structured content list.
IMAGE_TAGS = {
    "phi3v": "<|image_1|>\n",
    "phi4mm": "<|image_1|>\n",
    "qwen2_5_vl": "<|vision_start|><|image_pad|><|vision_end|>",
    "qwen3_vl": "<|vision_start|><|image_pad|><|vision_end|>",
    "fara": "<|vision_start|><|image_pad|><|vision_end|>",
    "lfm2_vl": "<image>",
    "mistral3": "[IMG]",
}

Describer = Callable[[str], str]


def unavailable_note(reason: str) -> str:
    """The description a frame gets when no model runs."""
    return (
        f"(VLM unavailable: {reason}. Install `vmaf-mcp[vlm]` and set "
        f"{MODEL_ENV} to a local ONNX Runtime GenAI vision model directory; "
        "see docs/mcp/tools.md#describe_worst_frames)"
    )


def user_content(model_type: str, prompt: str) -> Any:
    """The user turn for one image and @p prompt in the family's format."""
    tag = IMAGE_TAGS.get(model_type)
    if tag is not None:
        return tag + prompt
    return [{"type": "image"}, {"type": "text", "text": prompt}]


def chat_prompt(tokenizer: Any, model_dir: Path, model_type: str, prompt: str) -> str:
    """Render the chat template of the model directory around one image turn."""
    template = model_dir / "chat_template.jinja"
    template_str = template.read_text(encoding="utf-8") if template.is_file() else ""
    messages = json.dumps([{"role": "user", "content": user_content(model_type, prompt)}])
    return str(
        tokenizer.apply_chat_template(
            messages=messages, add_generation_prompt=True, template_str=template_str
        )
    )


def generate(og: Any, model: Any, inputs: Any, stream: Any) -> str:
    """Greedy decoding of one answer; bounded by MAX_LENGTH tokens."""
    params = og.GeneratorParams(model)
    params.set_search_options(max_length=MAX_LENGTH, do_sample=False)
    generator = og.Generator(model, params)
    generator.set_inputs(inputs)
    pieces: list[str] = []
    for _ in range(MAX_LENGTH):
        if generator.is_done():
            break
        generator.generate_next_token()
        pieces.append(stream.decode(generator.get_next_tokens()[0]))
    return "".join(pieces).strip()


def load(model_dir: Path, og: Any) -> Describer:
    """Load the model in @p model_dir; returns a function image path -> text."""
    model = og.Model(str(model_dir))
    tokenizer = og.Tokenizer(model)
    processor = model.create_multimodal_processor()
    prompt = chat_prompt(tokenizer, model_dir, str(model.type), PROMPT)

    def describe(image_path: str) -> str:
        images = og.Images.open(image_path)
        inputs = processor(prompt, images=images)
        return generate(og, model, inputs, tokenizer.create_stream())

    return describe


def configured_model_dir() -> Path | None:
    """The directory ``VMAF_MCP_VLM_MODEL`` names, or None when unset."""
    value = os.environ.get(MODEL_ENV, "").strip()
    return Path(value) if value else None


def load_configured() -> tuple[Describer, str] | str:
    """The configured model as ``(describe, model id)``, or why there is none."""
    model_dir = configured_model_dir()
    if model_dir is None:
        return f"{MODEL_ENV} is not set"
    if not (model_dir / "genai_config.json").is_file():
        return f"{model_dir} has no genai_config.json"
    try:
        import onnxruntime_genai as og  # optional `vlm` extra
    except ImportError:
        return "onnxruntime-genai is not installed"
    return load(model_dir, og), model_dir.name

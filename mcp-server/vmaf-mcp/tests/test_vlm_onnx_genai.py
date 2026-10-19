# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""``describe_worst_frames`` descriptions through ONNX Runtime GenAI (ADR-1886).

A stand-in for ``onnxruntime_genai`` records how the module drives the runtime:
model directory in, chat template around one image turn per model family,
greedy decoding bounded by ``MAX_LENGTH``, and the note when nothing runs.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from vmaf_mcp import vlm


class FakeGenerator:
    def __init__(self, tokens: list[int], log: dict[str, Any]) -> None:
        self.tokens = list(tokens)
        self.log = log
        self.last = -1

    def set_inputs(self, inputs: Any) -> None:
        self.log["inputs"] = inputs

    def is_done(self) -> bool:
        return not self.tokens

    def generate_next_token(self) -> None:
        self.last = self.tokens.pop(0)

    def get_next_tokens(self) -> list[int]:
        return [self.last]


def fake_runtime(model_type: str, tokens: list[int], log: dict[str, Any]) -> Any:
    """A module object with the onnxruntime_genai names vlm uses."""

    class Model:
        def __init__(self, path: str) -> None:
            log["model_dir"] = path
            self.type = model_type

        def create_multimodal_processor(self) -> Any:
            return lambda prompt, images: {"prompt": prompt, "images": images}

    class Tokenizer:
        def __init__(self, model: Any) -> None:
            pass

        def apply_chat_template(
            self, *, messages: str, add_generation_prompt: bool, template_str: str
        ) -> str:
            log["messages"] = json.loads(messages)
            log["template_str"] = template_str
            return f"<chat>{messages}</chat>"

        def create_stream(self) -> Any:
            return SimpleNamespace(decode=lambda token: f"w{token} ")

    class GeneratorParams:
        def __init__(self, model: Any) -> None:
            pass

        def set_search_options(self, **options: Any) -> None:
            log["search"] = options

    return SimpleNamespace(
        Model=Model,
        Tokenizer=Tokenizer,
        GeneratorParams=GeneratorParams,
        Generator=lambda model, params: FakeGenerator(tokens, log),
        Images=SimpleNamespace(open=lambda path: f"image:{path}"),
    )


def test_phi_family_gets_its_image_tag_and_greedy_bounded_decoding(tmp_path: Path) -> None:
    log: dict[str, Any] = {}
    describe = vlm.load(tmp_path, fake_runtime("phi3v", [1, 2, 3], log))
    text = describe("/frames/f1.png")
    assert text == "w1 w2 w3"
    assert log["model_dir"] == str(tmp_path)
    assert log["messages"] == [{"role": "user", "content": "<|image_1|>\n" + vlm.PROMPT}]
    assert log["search"] == {"max_length": vlm.MAX_LENGTH, "do_sample": False}
    assert log["inputs"]["images"] == "image:/frames/f1.png"


def test_unlisted_family_gets_structured_content_and_the_directory_template(tmp_path: Path) -> None:
    (tmp_path / "chat_template.jinja").write_text("{{ messages }}", encoding="utf-8")
    log: dict[str, Any] = {}
    vlm.load(tmp_path, fake_runtime("gemma3", [], log))
    assert log["messages"][0]["content"] == [
        {"type": "image"},
        {"type": "text", "text": vlm.PROMPT},
    ]
    assert log["template_str"] == "{{ messages }}"


def test_decoding_stops_at_max_length(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(vlm, "MAX_LENGTH", 4)
    log: dict[str, Any] = {}
    describe = vlm.load(tmp_path, fake_runtime("phi3v", list(range(10)), log))
    assert describe("x.png") == "w0 w1 w2 w3"


def test_unset_variable_is_named(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv(vlm.MODEL_ENV, raising=False)
    assert vlm.load_configured() == f"{vlm.MODEL_ENV} is not set"


def test_directory_without_genai_config_is_named(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv(vlm.MODEL_ENV, str(tmp_path))
    assert vlm.load_configured() == f"{tmp_path} has no genai_config.json"


def test_configured_model_loads_through_the_runtime(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    (tmp_path / "genai_config.json").write_text("{}", encoding="utf-8")
    monkeypatch.setenv(vlm.MODEL_ENV, str(tmp_path))
    log: dict[str, Any] = {}
    monkeypatch.setitem(sys.modules, "onnxruntime_genai", fake_runtime("qwen2_5_vl", [7], log))
    loaded = vlm.load_configured()
    assert not isinstance(loaded, str)
    describe, model_id = loaded
    assert model_id == tmp_path.name
    assert describe("f.png") == "w7"
    assert log["messages"][0]["content"].startswith("<|vision_start|>")


def test_a_configured_model_that_fails_to_load_raises(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """No silent fallback: a configured model that cannot load is an error."""
    (tmp_path / "genai_config.json").write_text("{}", encoding="utf-8")
    monkeypatch.setenv(vlm.MODEL_ENV, str(tmp_path))

    def broken(path: str) -> Any:
        raise RuntimeError("bad model")

    monkeypatch.setitem(sys.modules, "onnxruntime_genai", SimpleNamespace(Model=broken))
    with pytest.raises(RuntimeError, match="bad model"):
        vlm.load_configured()


def test_note_points_at_the_extra_and_the_variable() -> None:
    note = vlm.unavailable_note("why")
    assert note.startswith("(VLM unavailable: why.")
    assert "vmaf-mcp[vlm]" in note
    assert vlm.MODEL_ENV in note

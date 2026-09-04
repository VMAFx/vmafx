# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the ADR-1117 optional scoring pass-through parameters.

Covers the tiny-AI/DNN, feature-selection, CTC-preset, and frame-range
parameters added to ``vmaf_score`` / ``vmaf_score_encoded``:

- the tool schemas advertise every new property (both score tools);
- ``ScoreExtras.to_argv`` maps each MCP param onto the right ``vmaf`` CLI flag;
- a default-constructed ``ScoreExtras`` emits no flags (backward-compat);
- ``_extras_from_args`` distinguishes an explicit 0 from an absent key;
- the NR-mode gate rejects ``no_reference`` without ``tiny_model``.

The schemas here MUST stay byte-identical to the Go server's
``scoringExtraProperties()`` (cmd/vmafx-mcp/tools.go) — see
``cmd/vmafx-mcp/AGENTS.md``.
"""

from __future__ import annotations

import anyio
import pytest

from vmaf_mcp import server as srv

_NEW_PROPS = (
    "feature",
    "aom_ctc",
    "nflx_ctc",
    "tiny_model",
    "tiny_device",
    "dnn_ep",
    "tiny_threads",
    "tiny_fp16",
    "tiny_model_verify",
    "tiny_codec",
    "tiny_preset",
    "tiny_crf",
    "tiny_resize",
    "no_reference",
    "threads",
    "frame_cnt",
    "frame_skip_ref",
    "frame_skip_dist",
    "no_prediction",
)


def _tool_props(tool_name: str) -> dict:
    tools = anyio.run(srv._list_tools)
    for tool in tools:
        if tool.name == tool_name:
            return tool.input_schema["properties"]
    raise AssertionError(f"tool {tool_name!r} not found")


@pytest.mark.parametrize("tool", ["vmaf_score", "vmaf_score_encoded"])
def test_new_properties_present(tool: str) -> None:
    props = _tool_props(tool)
    for key in _NEW_PROPS:
        assert key in props, f"tool {tool!r} missing ADR-1117 property {key!r}"
    # Backward-compat: pre-existing properties still advertised.
    for key in ("model", "backend", "precision"):
        assert key in props, f"tool {tool!r} lost pre-existing property {key!r}"


def test_enums_match_cli_parse() -> None:
    props = _tool_props("vmaf_score")
    assert props["aom_ctc"]["enum"] == ["v1.0", "v2.0", "v3.0", "v4.0", "v5.0", "v6.0", "v7.0"]
    assert props["nflx_ctc"]["enum"] == ["v1.0"]
    assert props["tiny_resize"]["enum"] == ["bilinear", "nearest", "bicubic", "disabled"]
    assert "auto" in props["tiny_device"]["enum"]
    assert "rocm" in props["tiny_device"]["enum"]
    assert props["dnn_ep"]["enum"] == props["tiny_device"]["enum"]


def test_to_argv_maps_every_flag() -> None:
    extras = srv.ScoreExtras(
        features=("psnr", "cambi=full_ref=true"),
        aom_ctc="v3.0",
        tiny_model="/m/nr.onnx",
        tiny_device="cuda",
        tiny_threads=4,
        tiny_fp16=True,
        tiny_model_verify=True,
        tiny_codec="libx264",
        tiny_preset="medium",
        tiny_crf=23,
        tiny_resize="bilinear",
        no_reference=True,
        threads=8,
        frame_cnt=100,
        frame_skip_ref=2,
        frame_skip_dist=0,  # explicit 0 must be emitted
        no_prediction=True,
    )
    argv = extras.to_argv()
    joined = " ".join(argv)
    for sub in (
        "--feature psnr",
        "--feature cambi=full_ref=true",
        "--aom_ctc v3.0",
        "--tiny-model /m/nr.onnx",
        "--tiny-device cuda",
        "--tiny-threads 4",
        "--tiny-fp16",
        "--tiny-model-verify",
        "--tiny-codec libx264",
        "--tiny-preset medium",
        "--tiny-crf 23",
        "--tiny-resize bilinear",
        "--no-reference",
        "--threads 8",
        "--frame_cnt 100",
        "--frame_skip_ref 2",
        "--frame_skip_dist 0",
        "--no_prediction",
    ):
        assert sub in joined, f"argv {joined!r} missing {sub!r}"


def test_empty_extras_emits_no_flags() -> None:
    extras = srv.ScoreExtras()
    assert extras.is_empty() is True
    assert extras.to_argv() == []


def test_extras_from_args_distinguishes_zero_from_unset() -> None:
    with_zero = srv._extras_from_args({"frame_skip_dist": 0})
    assert with_zero.frame_skip_dist == 0
    assert "--frame_skip_dist 0" in " ".join(with_zero.to_argv())

    without = srv._extras_from_args({})
    assert without.frame_skip_dist is None
    assert without.to_argv() == []


def test_extras_from_args_filters_non_string_features() -> None:
    extras = srv._extras_from_args({"feature": ["psnr", "", 123, "ssim"]})
    assert extras.features == ("psnr", "ssim")


def test_no_reference_requires_tiny_model() -> None:
    # The dispatch should reject --no-reference without a tiny model, mirroring
    # cli_parse.c:997, before ever spawning the subprocess.
    with pytest.raises(ValueError, match="no_reference requires tiny_model"):
        anyio.run(
            lambda: srv._call_tool_dispatch(
                "vmaf_score",
                {
                    "dis": "/nonexistent/b.yuv",
                    "width": 64,
                    "height": 64,
                    "pixfmt": "420",
                    "bitdepth": 8,
                    "no_reference": True,
                },
                None,
            )
        )


def test_dnn_ep_alias_supported() -> None:
    extras = srv._extras_from_args({"dnn_ep": "openvino-npu"})
    assert extras.tiny_device == "openvino-npu"
    argv = extras.to_argv()
    assert "--tiny-device openvino-npu" in " ".join(argv)


@pytest.mark.parametrize(
    ("args", "match"),
    [
        ({"tiny_device": "invalid_dev"}, "invalid tiny_device"),
        ({"dnn_ep": "unknown_ep"}, "invalid tiny_device"),
        ({"tiny_device": "cpu", "dnn_ep": "cuda"}, "conflicting tiny_device"),
        ({"tiny_resize": "cubic"}, "invalid tiny_resize"),
        ({"tiny_crf": -1}, "invalid tiny_crf"),
        ({"tiny_crf": 64}, "invalid tiny_crf"),
        ({"tiny_threads": -1}, "invalid tiny_threads"),
        ({"aom_ctc": "v8.0"}, "invalid aom_ctc"),
        ({"nflx_ctc": "v2.0"}, "invalid nflx_ctc"),
        ({"threads": 0}, "invalid threads"),
        ({"frame_cnt": 0}, "invalid frame_cnt"),
        ({"frame_skip_ref": -1}, "invalid frame_skip_ref"),
        ({"subsample": 0}, "invalid subsample"),
    ],
)
def test_extras_validation_rejects_bad_enums(args: dict, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        srv._extras_from_args(args)


@pytest.mark.parametrize(
    ("args", "match"),
    [
        (
            {
                "ref": "model/vmaf_v0.6.1.json",
                "dis": "model/vmaf_v0.6.1.json",
                "width": 64,
                "height": 64,
                "pixfmt": "422p",
                "bitdepth": 8,
            },
            "invalid pixfmt",
        ),
        (
            {
                "ref": "model/vmaf_v0.6.1.json",
                "dis": "model/vmaf_v0.6.1.json",
                "width": 64,
                "height": 64,
                "pixfmt": "420",
                "bitdepth": 14,
            },
            "invalid bitdepth",
        ),
        (
            {
                "ref": "model/vmaf_v0.6.1.json",
                "dis": "model/vmaf_v0.6.1.json",
                "width": 64,
                "height": 64,
                "pixfmt": "420",
                "bitdepth": 8,
                "backend": "vulkan",
            },
            "invalid backend",
        ),
    ],
)
def test_vmaf_score_rejects_invalid_core_params(args: dict, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        anyio.run(lambda: srv._call_tool_dispatch("vmaf_score", args, None))

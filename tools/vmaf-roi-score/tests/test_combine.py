# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for vmaf-roi-score Option C.

Mocks the ``vmaf`` subprocess so no binaries are required. Pins:

- the combine-math is a pure linear blend on Python ``float``;
- the JSON output schema (key order, schema_version);
- the synthetic-mask code path materialises a masked YUV and produces
  a score *between* the two underlying VMAF runs (the contract the
  user-facing docstring promises).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmafroiscore import ROI_RESULT_KEYS, SCHEMA_VERSION, blend_scores
from vmafroiscore.cli import _build_parser, main
from vmafroiscore.mask import (
    MaskRequest,
    apply_saliency_mask,
    synthesise_uniform_mask,
)
from vmafroiscore.score import (
    ScoreRequest,
    build_vmaf_command,
    parse_vmaf_json,
)

# ---------------------------------------------------------------------------
# combine math


def test_blend_endpoints_are_pure_passthrough():
    assert blend_scores(80.0, 60.0, 0.0) == 80.0
    assert blend_scores(80.0, 60.0, 1.0) == 60.0


def test_blend_midpoint_is_arithmetic_mean():
    assert blend_scores(80.0, 60.0, 0.5) == 70.0


def test_blend_is_between_inputs_for_any_weight():
    """Contract: roi-vmaf is always between the two underlying scores."""
    a, b = 92.5, 41.25
    lo, hi = min(a, b), max(a, b)
    for w in (0.0, 0.25, 0.5, 0.75, 1.0):
        out = blend_scores(a, b, w)
        assert lo <= out <= hi, f"w={w} produced {out} outside [{lo}, {hi}]"


def test_blend_rejects_out_of_range_weight():
    with pytest.raises(ValueError):
        blend_scores(80.0, 60.0, 1.5)
    with pytest.raises(ValueError):
        blend_scores(80.0, 60.0, -0.1)


def test_blend_rejects_non_finite_inputs():
    with pytest.raises(ValueError):
        blend_scores(float("nan"), 60.0, 0.5)
    with pytest.raises(ValueError):
        blend_scores(80.0, float("inf"), 0.5)


# ---------------------------------------------------------------------------
# vmaf-cli wiring (pure-function pieces)


def test_build_vmaf_command_shape():
    req = ScoreRequest(
        reference=Path("/tmp/ref.yuv"),
        distorted=Path("/tmp/dis.yuv"),
        width=576,
        height=324,
        pix_fmt="yuv420p",
    )
    cmd = build_vmaf_command(req, Path("/tmp/out.json"))
    assert cmd[0] == "vmaf"
    assert "--reference" in cmd
    assert "/tmp/ref.yuv" in cmd
    assert "--distorted" in cmd
    assert "/tmp/dis.yuv" in cmd
    assert "--pixel_format" in cmd
    pf_idx = cmd.index("--pixel_format")
    assert cmd[pf_idx + 1] == "420"


def test_parse_vmaf_json_modern_shape():
    payload = {"pooled_metrics": {"vmaf": {"mean": 87.5, "min": 80.0}}}
    assert parse_vmaf_json(payload) == 87.5


def test_parse_vmaf_json_legacy_shape():
    payload = {"VMAF score": 72.5}
    assert parse_vmaf_json(payload) == 72.5


def test_parse_vmaf_json_missing_raises():
    with pytest.raises(ValueError):
        parse_vmaf_json({})


# ---------------------------------------------------------------------------
# argparse


def test_parser_accepts_minimum_args(tmp_path: Path):
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x80" * 16)
    dis.write_bytes(b"\x80" * 16)
    parser = _build_parser()
    ns = parser.parse_args(
        [
            "--reference",
            str(ref),
            "--distorted",
            str(dis),
            "--width",
            "576",
            "--height",
            "324",
        ]
    )
    assert ns.saliency_model is None
    assert ns.synthetic_mask is None


# ---------------------------------------------------------------------------
# synthetic-mask helper


def test_synthesise_uniform_mask_shape_and_value():
    m = synthesise_uniform_mask(4, 3, fill=0.7)
    assert len(m) == 3
    assert len(m[0]) == 4
    assert all(v == 0.7 for row in m for v in row)


def test_synthesise_uniform_mask_rejects_bad_args():
    with pytest.raises(ValueError):
        synthesise_uniform_mask(4, 3, fill=1.5)
    with pytest.raises(ValueError):
        synthesise_uniform_mask(0, 3, fill=0.5)


def test_apply_saliency_mask_materialises_yuv420p(tmp_path: Path):
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    out = tmp_path / "masked.yuv"

    # 4x4 yuv420p: Y has 16 bytes, U/V have 4 bytes each.
    ref.write_bytes(bytes([10] * 16 + [20] * 4 + [30] * 4))
    dis.write_bytes(bytes([110] * 16 + [120] * 4 + [130] * 4))

    def _mask(_rgb: bytes, width: int, height: int):
        assert width == 4
        assert height == 4
        return [
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
        ]

    apply_saliency_mask(
        MaskRequest(
            reference=ref,
            distorted=dis,
            output=out,
            width=4,
            height=4,
            pix_fmt="yuv420p",
            saliency_model=tmp_path / "unused.onnx",
            threshold=0.5,
            fade=0.0,
        ),
        inference=_mask,
    )

    data = out.read_bytes()
    assert data[:16] == bytes([10, 10, 110, 110] * 4)
    assert data[16:20] == bytes([20, 120, 20, 120])
    assert data[20:24] == bytes([30, 130, 30, 130])


def test_apply_saliency_mask_materialises_yuv420p10le(tmp_path: Path):
    ref = tmp_path / "ref10.yuv"
    dis = tmp_path / "dis10.yuv"
    out = tmp_path / "masked10.yuv"

    def _le16(values: list[int]) -> bytes:
        return b"".join(v.to_bytes(2, "little") for v in values)

    # 4x4 yuv420p10le: Y has 16 samples, U/V have 4 samples each.
    ref.write_bytes(_le16([64] * 16 + [256] * 4 + [384] * 4))
    dis.write_bytes(_le16([940] * 16 + [768] * 4 + [896] * 4))

    def _mask(_rgb: bytes, width: int, height: int):
        assert width == 4
        assert height == 4
        return [
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
            [0.0, 0.0, 1.0, 1.0],
        ]

    apply_saliency_mask(
        MaskRequest(
            reference=ref,
            distorted=dis,
            output=out,
            width=4,
            height=4,
            pix_fmt="yuv420p10le",
            saliency_model=tmp_path / "unused.onnx",
            threshold=0.5,
            fade=0.0,
        ),
        inference=_mask,
    )

    samples = [
        int.from_bytes(out.read_bytes()[idx : idx + 2], "little")
        for idx in range(0, out.stat().st_size, 2)
    ]
    assert samples[:16] == [64, 64, 940, 940] * 4
    assert samples[16:20] == [256, 768, 256, 768]
    assert samples[20:24] == [384, 896, 384, 896]


def test_build_vmaf_command_preserves_yuv420p16le_bitdepth():
    req = ScoreRequest(
        reference=Path("/tmp/ref.yuv"),
        distorted=Path("/tmp/dis.yuv"),
        width=4,
        height=4,
        pix_fmt="yuv420p16le",
    )
    cmd = build_vmaf_command(req, Path("/tmp/out.json"))
    assert cmd[cmd.index("--pixel_format") + 1] == "420"
    assert cmd[cmd.index("--bitdepth") + 1] == "16"


# ---------------------------------------------------------------------------
# end-to-end smoke (synthetic-mask path) — mocks subprocess


class _FakeCompleted:
    """Stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def test_cli_synthetic_smoke(monkeypatch, tmp_path: Path):
    """Drive ``main`` end-to-end with a mocked vmaf binary.

    Verifies:
    - exit code is 0;
    - JSON output has the canonical key order;
    - the blended score equals the (identical) underlying scores;
    - the schema_version matches the package constant.
    """
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(bytes([10] * 16 + [20] * 4 + [30] * 4))
    dis.write_bytes(bytes([110] * 16 + [120] * 4 + [130] * 4))
    out = tmp_path / "result.json"
    distorted_paths: list[Path] = []
    masked_bytes: list[bytes] = []

    def _fake_run(cmd, capture_output=False, text=False, check=False):
        # Find the --output path argparse handed us and drop a JSON
        # there in the modern pooled_metrics shape.
        dis_idx = cmd.index("--distorted")
        distorted = Path(cmd[dis_idx + 1])
        distorted_paths.append(distorted)
        if distorted != dis:
            masked_bytes.append(distorted.read_bytes())
        out_idx = cmd.index("--output")
        score = 80.0 if distorted == dis else 90.0
        Path(cmd[out_idx + 1]).write_text(
            json.dumps({"pooled_metrics": {"vmaf": {"mean": score}}}),
            encoding="utf-8",
        )
        return _FakeCompleted(0, stderr="VMAF version: smoke-mock\n")

    monkeypatch.setattr("vmafroiscore.score.subprocess.run", _fake_run)

    rc = main(
        [
            "--reference",
            str(ref),
            "--distorted",
            str(dis),
            "--width",
            "4",
            "--height",
            "4",
            "--synthetic-mask",
            "0.5",
            "--threshold",
            "0",
            "--fade",
            "1",
            "--weight",
            "0.7",
            "--output",
            str(out),
        ]
    )
    assert rc == 0
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert tuple(payload.keys()) == ROI_RESULT_KEYS
    assert payload["schema_version"] == SCHEMA_VERSION
    assert distorted_paths[0] == dis
    assert distorted_paths[1] != dis
    assert masked_bytes == [bytes([60] * 16 + [70] * 4 + [80] * 4)]
    assert payload["vmaf_full"] == 80.0
    assert payload["vmaf_masked"] == 90.0
    assert payload["vmaf_roi"] == 87.0
    assert payload["weight"] == 0.7
    assert payload["saliency_model"] == "synthetic"


def test_cli_saliency_model_materialises_mask(monkeypatch, tmp_path: Path):
    """The --saliency-model path materialises a masked YUV and scores it."""
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    fake_model = tmp_path / "saliency.onnx"
    out = tmp_path / "result.json"
    ref.write_bytes(bytes([10] * 16 + [20] * 4 + [30] * 4))
    dis.write_bytes(bytes([110] * 16 + [120] * 4 + [130] * 4))
    fake_model.write_bytes(b"")  # presence is enough; cli only checks exists()

    seen_distorted: list[Path] = []

    def _fake_run(cmd, capture_output=False, text=False, check=False):
        distorted = Path(cmd[cmd.index("--distorted") + 1])
        seen_distorted.append(distorted)
        out_idx = cmd.index("--output")
        score = 90.0 if len(seen_distorted) == 1 else 95.0
        Path(cmd[out_idx + 1]).write_text(
            json.dumps({"pooled_metrics": {"vmaf": {"mean": score}}}),
            encoding="utf-8",
        )
        return _FakeCompleted(0, stderr="VMAF version: smoke-mock\n")

    def _fake_mask(req, *, inference=None):
        req.output.write_bytes(b"masked-yuv")
        return req.output

    monkeypatch.setattr("vmafroiscore.score.subprocess.run", _fake_run)
    monkeypatch.setattr("vmafroiscore.mask.apply_saliency_mask", _fake_mask)

    rc = main(
        [
            "--reference",
            str(ref),
            "--distorted",
            str(dis),
            "--width",
            "4",
            "--height",
            "4",
            "--saliency-model",
            str(fake_model),
            "--weight",
            "0.25",
            "--output",
            str(out),
        ]
    )
    assert rc == 0
    assert seen_distorted[0] == dis
    assert seen_distorted[1].name == "distorted.saliency-masked.yuv"
    payload = json.loads(out.read_text(encoding="utf-8"))
    assert payload["vmaf_full"] == 90.0
    assert payload["vmaf_masked"] == 95.0
    assert payload["vmaf_roi"] == 91.25
    assert payload["saliency_model"] == str(fake_model)

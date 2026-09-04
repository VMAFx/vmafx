# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Parity tests between Python fast path (vmaftune) and Go twin (pkg/fast).

Guards against drift between the Python production fast path and the
Go implementation in pkg/fast across:
1. Canonical-6 raw feature extraction and pooled key lookup order
2. StandardScaler feature normalisation from model sidecar stats
3. Codec block 14-D encoding and encoder vocabulary ordering
4. End-to-end sample extraction with container decode and cleanup
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install — mirrors test_fast.py.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli as cli_module
from vmaftune.proxy import (
    DEFAULT_PROXY_MODEL_ID,
    ENCODER_VOCAB_V2,
    encode_codec_block,
    load_proxy_sidecar,
    normalise_features,
)


def _resolve_vmaf_bin() -> str | None:
    # Check VMAF_BIN_FOR_TESTS, then in-tree core/build/tools/vmaf, then PATH
    import os

    if "VMAF_BIN_FOR_TESTS" in os.environ:
        p = Path(os.environ["VMAF_BIN_FOR_TESTS"])
        if p.exists() and os.access(p, os.X_OK):
            return str(p)
    repo_root = Path(__file__).resolve().parents[3]
    candidate = repo_root / "core" / "build" / "tools" / "vmaf"
    if candidate.exists() and os.access(candidate, os.X_OK):
        return str(candidate)
    return shutil.which("vmaf")


def test_encoder_vocab_matches_sidecar() -> None:
    """Python ENCODER_VOCAB_V2 matches fr_regressor_v2.json sidecar exactly."""
    sidecar = load_proxy_sidecar(DEFAULT_PROXY_MODEL_ID)
    sidecar_vocab = tuple(sidecar["encoder_vocab"])
    assert ENCODER_VOCAB_V2 == sidecar_vocab, (
        f"ENCODER_VOCAB_V2 drifted from {DEFAULT_PROXY_MODEL_ID}.json: "
        f"{ENCODER_VOCAB_V2} != {sidecar_vocab}"
    )
    # Ensure index 3 is libvvenc (not libaom-av1) and last slot is unknown
    assert ENCODER_VOCAB_V2[3] == "libvvenc"
    assert ENCODER_VOCAB_V2[-1] == "unknown"


def test_encode_codec_block_layout_and_unknown_fallback() -> None:
    """Codec block is 14-D and correctly slots encoders including unknown."""
    pytest.importorskip("numpy")

    # libx264 at index 0
    b_264 = encode_codec_block("libx264", preset_norm=0.5, crf_norm=0.3)
    assert b_264.shape == (14,)
    assert b_264[0] == pytest.approx(1.0)
    assert sum(b_264[:12]) == pytest.approx(1.0)
    assert b_264[12] == pytest.approx(0.5)
    assert b_264[13] == pytest.approx(0.3)

    # libvvenc at index 3
    b_vvenc = encode_codec_block("libvvenc", preset_norm=0.5, crf_norm=0.3)
    assert b_vvenc[3] == pytest.approx(1.0)
    assert sum(b_vvenc[:12]) == pytest.approx(1.0)

    # unknown at index 11
    b_unk = encode_codec_block("unknown", preset_norm=0.5, crf_norm=0.3)
    assert b_unk[11] == pytest.approx(1.0)
    assert sum(b_unk[:12]) == pytest.approx(1.0)

    # allow_unknown=True maps unrecognized encoder to unknown slot 11
    b_custom = encode_codec_block(
        "custom_codec_foo",
        preset_norm=0.5,
        crf_norm=0.3,
        allow_unknown=True,
    )
    assert b_custom[11] == pytest.approx(1.0)
    assert sum(b_custom[:12]) == pytest.approx(1.0)


def test_normalise_features_matches_go_twin_exact_values() -> None:
    """Feature normalisation matches the exact output of Go pkg/fast twin."""
    raw = [0.98923, 0.898144, 0.987491, 0.993269, 0.995706, 6.510201]
    expected_norm = [
        1.0043383257414282,
        2.411333336270086,
        1.051841549428872,
        0.9004389382947547,
        0.8143562068093163,
        -0.5624692192876282,
    ]
    got_norm = normalise_features(raw)
    for g, e in zip(got_norm, expected_norm, strict=True):
        assert g == pytest.approx(e, abs=1e-7)


# Probe parameters shared by both sides. The Go integration test
# (pkg/fast/integration_test.go::TestProbePipelineExtractsRealFeatures) pins
# libx264 / ultrafast / CRF 28 / a 1 s chunk over the in-tree 48-frame fixture;
# the Python extractor below is driven with the identical settings so the two
# runs encode, decode and score the same window.
_PARITY_CRF = 28
_PARITY_ENCODER = "libx264"
_GO_TEST_NAME = "TestProbePipelineExtractsRealFeatures"
_GO_RAW_RE = re.compile(rf"canonical-6 at CRF {_PARITY_CRF}: \[([^\]]*)\] \(([0-9.]+) kbps\)")
_GO_NORM_RE = re.compile(rf"normalised canonical-6 at CRF {_PARITY_CRF}: \[([^\]]*)\]")


def _parse_go_vector(text: str) -> list[float]:
    """Parse the ``%v`` rendering of a Go ``[]float64`` (space separated)."""
    return [float(tok) for tok in text.split()]


def _run_go_twin(repo_root: Path, vmaf_bin: str) -> tuple[list[float], list[float], float]:
    """Drive the Go twin's probe pipeline and return (raw, normalised, kbps).

    The Go integration test logs both vectors at ``-v``; running it through
    ``go test`` keeps the Go side on its production code path (encoder
    adapter, decode, libvmaf argv, pooled-key parser, sidecar scaler) with
    nothing re-implemented on the Python side.
    """
    env = dict(os.environ)
    env["PATH"] = f"{Path(vmaf_bin).resolve().parent}{os.pathsep}{env.get('PATH', '')}"
    env.setdefault("GOTOOLCHAIN", "auto")
    completed = subprocess.run(
        ["go", "test", "-count=1", "-v", "-run", f"^{_GO_TEST_NAME}$", "./pkg/fast/"],
        cwd=repo_root,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout=600,
    )
    output = completed.stdout + completed.stderr
    if "--- SKIP" in output:
        pytest.skip(f"Go twin skipped {_GO_TEST_NAME}: {output[-400:]}")
    assert (
        completed.returncode == 0
    ), f"go test failed (rc={completed.returncode}):\n{output[-2000:]}"
    raw_match = _GO_RAW_RE.search(output)
    norm_match = _GO_NORM_RE.search(output)
    assert raw_match and norm_match, f"Go twin did not log the parity vectors:\n{output[-2000:]}"
    return (
        _parse_go_vector(raw_match.group(1)),
        _parse_go_vector(norm_match.group(1)),
        float(raw_match.group(2)),
    )


def test_e2e_probe_extraction_parity(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Python and Go fast paths extract identical canonical-6 vectors.

    Runs both implementations on the same tiny clip with the same probe
    parameters and asserts the six raw pooled means match exactly and the
    six StandardScaler-normalised features match within 1e-6. Skips (with
    the reason) when ffmpeg, the vmaf CLI, the Go toolchain, or the fixture
    is unavailable on the host.
    """
    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin:
        pytest.skip("ffmpeg not on PATH")
    vmaf_bin = _resolve_vmaf_bin()
    if not vmaf_bin:
        pytest.skip("vmaf binary not reachable")
    if not shutil.which("go"):
        pytest.skip("go toolchain not on PATH; cannot drive the Go twin")

    repo_root = Path(__file__).resolve().parents[3]
    ref_yuv = repo_root / "testdata" / "ref_576x324_48f.yuv"
    if not ref_yuv.exists():
        pytest.skip(f"fixture {ref_yuv} missing")
    if not (repo_root / "go.mod").exists():
        pytest.skip(f"go.mod not found under {repo_root}; cannot drive the Go twin")

    # Spy on the normalisation step so the raw pooled means are observable
    # alongside the normalised vector the extractor returns.
    import vmaftune.proxy as proxy_module

    raw_seen: list[list[float]] = []
    real_normalise = proxy_module.normalise_features

    def _spy(features, *args, **kwargs):
        raw_seen.append([float(v) for v in features])
        return real_normalise(features, *args, **kwargs)

    monkeypatch.setattr(proxy_module, "normalise_features", _spy)

    args = argparse.Namespace(
        width=576,
        height=324,
        pix_fmt="yuv420p",
        framerate=24.0,
        preset="ultrafast",
        sample_chunk_seconds=1.0,
        ffmpeg_bin=ffmpeg_bin,
        vmaf_bin=vmaf_bin,
        vmaf_model="vmaf_v0.6.1",
    )
    workdir = tmp_path / "probes"
    extractor = cli_module._build_fast_sample_extractor(args, workdir)

    py_norm, py_kbps = extractor(ref_yuv, _PARITY_CRF, _PARITY_ENCODER)
    assert len(raw_seen) == 1
    py_raw = raw_seen[0]

    # Real bitrate must be positive; 6 finite, not-all-zero features.
    assert py_kbps > 0.0
    assert len(py_norm) == 6
    assert all(math.isfinite(f) for f in py_norm)
    assert not all(f == 0.0 for f in py_norm)

    # Decoded temporary file must be cleaned up.
    decoded_files = list(workdir.glob("*.decoded.yuv"))
    assert len(decoded_files) == 0, f"temporary decoded YUV not cleaned up: {decoded_files}"

    go_raw, go_norm, go_kbps = _run_go_twin(repo_root, vmaf_bin)
    assert len(go_raw) == 6 and len(go_norm) == 6

    # Raw pooled means come straight out of libvmaf's JSON on both sides —
    # the same encode, decode and score must yield the same numbers.
    for name, py_v, go_v in zip(cli_module._CANONICAL_6_KEYS, py_raw, go_raw, strict=True):
        assert py_v == pytest.approx(go_v, abs=1e-9), f"raw {name}: python={py_v} go={go_v}"
    # Normalised features are the proxy's actual input; pin to 1e-6.
    for name, py_v, go_v in zip(cli_module._CANONICAL_6_KEYS, py_norm, go_norm, strict=True):
        assert py_v == pytest.approx(go_v, abs=1e-6), f"normalised {name}: python={py_v} go={go_v}"
    # Go logs kbps at one decimal.
    assert py_kbps == pytest.approx(go_kbps, abs=0.1)

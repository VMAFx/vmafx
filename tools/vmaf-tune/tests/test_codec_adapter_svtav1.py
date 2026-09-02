# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""SVT-AV1 codec adapter unit + smoke tests.

The harness mocks ``subprocess.run`` so these tests pass without
``ffmpeg`` or a libsvtav1 build on the runner. The integration smoke
that actually invokes ``ffmpeg -c:v libsvtav1`` is gated to the CI
runner that ships the codec.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install (matches test_corpus).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CORPUS_ROW_KEYS
from vmaftune.codec_adapters import get_adapter, known_codecs
from vmaftune.codec_adapters.svtav1 import (
    PRESET_NAME_TO_INT,
    SvtAv1Adapter,
    preset_to_int,
)
from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows
from vmaftune.encode import parse_versions


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# --- Adapter shape ---------------------------------------------------


def test_svtav1_registered_in_known_codecs():
    assert "libsvtav1" in known_codecs()
    a = get_adapter("libsvtav1")
    assert a.name == "libsvtav1"
    assert a.encoder == "libsvtav1"
    assert a.quality_knob == "crf"
    assert a.invert_quality is True


def test_svtav1_quality_range_is_av1_window():
    a = get_adapter("libsvtav1")
    assert a.quality_range == (20, 50)
    # SVT-AV1's absolute CRF range is 0..63 (vs x264's 0..51).
    assert isinstance(a, SvtAv1Adapter)
    assert a.crf_min == 0
    assert a.crf_max == 63
    assert a.preset_min == 0
    assert a.preset_max == 13


# --- Preset name -> int mapping --------------------------------------


def test_preset_name_to_int_mapping_matches_spec():
    # Lock in the documented mapping. If this test fails, the docs in
    # docs/usage/vmaf-tune.md and ADR-0277 must be updated in lockstep.
    assert PRESET_NAME_TO_INT == {
        "placebo": 0,
        "slowest": 1,
        "slower": 3,
        "slow": 5,
        "medium": 7,
        "fast": 9,
        "faster": 11,
        "veryfast": 13,
    }


def test_preset_to_int_translates_known_names():
    assert preset_to_int("medium") == 7
    assert preset_to_int("placebo") == 0
    assert preset_to_int("veryfast") == 13


def test_preset_to_int_rejects_unknown():
    with pytest.raises(ValueError, match="unknown svtav1 preset"):
        preset_to_int("ludicrous")


def test_adapter_ffmpeg_preset_token_is_int_string():
    a = SvtAv1Adapter()
    # The corpus harness emits the integer-as-string into argv.
    assert a.ffmpeg_preset_token("medium") == "7"
    assert a.ffmpeg_preset_token("placebo") == "0"
    assert a.ffmpeg_preset_token("veryfast") == "13"


# --- Validation ------------------------------------------------------


def test_validate_accepts_phase_a_window():
    a = SvtAv1Adapter()
    a.validate("medium", 35)
    a.validate("slow", 20)
    a.validate("fast", 50)


def test_validate_rejects_unknown_preset():
    a = SvtAv1Adapter()
    with pytest.raises(ValueError, match="unknown svtav1 preset"):
        a.validate("ultrafast", 35)


def test_validate_rejects_crf_above_63_with_clear_message():
    # AV1 hard-limit guard — must fire before the Phase A window check
    # so users see "absolute range" rather than "Phase A range".
    a = SvtAv1Adapter()
    with pytest.raises(ValueError, match=r"absolute range \[0, 63\]"):
        a.validate("medium", 64)
    with pytest.raises(ValueError, match=r"absolute range \[0, 63\]"):
        a.validate("medium", 100)


def test_validate_rejects_negative_crf():
    a = SvtAv1Adapter()
    with pytest.raises(ValueError, match=r"absolute range \[0, 63\]"):
        a.validate("medium", -1)


def test_validate_rejects_phase_a_out_of_window_but_legal_crf():
    a = SvtAv1Adapter()
    # CRF 10 is a legal AV1 value but outside Phase A's informative
    # window — must reject with the Phase A message, not the
    # absolute-range one.
    with pytest.raises(ValueError, match=r"Phase A range \[20, 50\]"):
        a.validate("medium", 10)
    with pytest.raises(ValueError, match=r"Phase A range \[20, 50\]"):
        a.validate("medium", 60)


# --- Encode banner parsing -------------------------------------------


def test_parse_versions_recognises_svtav1_banner():
    stderr = "ffmpeg version n8.1 built with gcc\n" "Svt[info]:SVT-AV1 Encoder Lib v2.1.0\n"
    ffm, enc = parse_versions(stderr)
    assert ffm == "n8.1"
    # ``v`` prefix in the banner is stripped by the parser so the row
    # carries a bare version token.
    assert enc == "libsvtav1-2.1.0"


def test_parse_versions_x264_takes_priority_when_both_present():
    # Defensive: a stream-copy log shouldn't surprise us; x264 banner
    # wins because it comes first in the matcher list.
    stderr = "ffmpeg version 6.1\n" "x264 - core 164 r3107\n" "SVT-AV1 Encoder Lib v2.1.0\n"
    _, enc = parse_versions(stderr)
    assert enc.startswith("libx264")


# --- Mocked end-to-end smoke ----------------------------------------


def _make_yuv(path: Path, nbytes: int = 1024) -> Path:
    path.write_bytes(b"\x80" * nbytes)
    return path


def test_smoke_corpus_with_svtav1_mocked(tmp_path: Path):
    """Run the corpus loop with libsvtav1 + a mocked subprocess.

    Verifies (a) the FFmpeg argv carries the integer preset token,
    (b) the corpus row records the human-readable preset name, and
    (c) the SVT-AV1 banner produces a recognisable encoder_version.
    """
    src = _make_yuv(tmp_path / "ref.yuv")
    captured_cmds: list[list[str]] = []

    def fake_encode_run(cmd, capture_output, text, check):
        captured_cmds.append(list(cmd))
        out_path = Path(cmd[-1])
        out_path.write_bytes(b"\x00" * 8192)
        return _FakeCompleted(
            returncode=0,
            stderr=("ffmpeg version 7.0.1\n" "Svt[info]:SVT-AV1 Encoder Lib v2.1.0\n"),
        )

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 88.7}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 35), ("slow", 28)),
    )
    opts = CorpusOptions(
        encoder="libsvtav1",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))

    assert len(rows) == 2
    for r in rows:
        assert set(CORPUS_ROW_KEYS) == set(r.keys())
        assert r["encoder"] == "libsvtav1"
        assert r["encoder_version"] == "libsvtav1-2.1.0"
        assert r["vmaf_score"] == pytest.approx(88.7)
        assert r["exit_status"] == 0

    # Row preset is the human name; argv preset is the int token.
    presets_in_rows = [r["preset"] for r in rows]
    assert presets_in_rows == ["medium", "slow"]

    for cmd, expected_int in zip(captured_cmds, ("7", "5"), strict=True):
        assert "-c:v" in cmd
        assert cmd[cmd.index("-c:v") + 1] == "libsvtav1"
        assert cmd[cmd.index("-preset") + 1] == expected_int

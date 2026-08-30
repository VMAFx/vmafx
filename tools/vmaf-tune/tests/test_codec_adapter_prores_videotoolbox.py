# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ProRes VideoToolbox codec adapter smoke tests.

Mirrors ``test_codec_adapter_videotoolbox.py``: mocks ``subprocess.run``
so the suite has no runtime dependency on a macOS host or a working
ProRes hardware block. Production callers run on Apple Silicon
(M1 Pro / Max / Ultra and later); the test gate only verifies the
adapter contract and the encode-driver argv shape.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    ProresVideoToolboxAdapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._videotoolbox_common import (
    PRORES_PROFILE_4444,
    PRORES_PROFILE_DEFAULT,
    PRORES_PROFILE_HQ,
    PRORES_PROFILE_LT,
    PRORES_PROFILE_NAMES,
    PRORES_PROFILE_PROXY,
    PRORES_PROFILE_RANGE,
    PRORES_PROFILE_STANDARD,
    PRORES_PROFILE_XQ,
    prores_profile_name,
    validate_prores_videotoolbox,
)
from vmaftune.encode import (
    EncodeRequest,
    build_ffmpeg_command,
    run_encode,
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def test_prores_videotoolbox_adapter_contract():
    a = ProresVideoToolboxAdapter()
    assert a.name == "prores_videotoolbox"
    assert a.encoder == "prores_videotoolbox"
    assert a.quality_knob == "profile:v"
    assert a.quality_range == (0, 5)
    assert a.quality_default == PRORES_PROFILE_HQ
    assert a.invert_quality is False  # higher tier = better quality


def test_prores_videotoolbox_registered_in_registry():
    assert "prores_videotoolbox" in known_codecs()
    assert get_adapter("prores_videotoolbox").encoder == "prores_videotoolbox"


def test_prores_profile_id_to_name_round_trip():
    """Each integer tier id maps to its canonical FFmpeg alias."""
    assert prores_profile_name(PRORES_PROFILE_PROXY) == "proxy"
    assert prores_profile_name(PRORES_PROFILE_LT) == "lt"
    assert prores_profile_name(PRORES_PROFILE_STANDARD) == "standard"
    assert prores_profile_name(PRORES_PROFILE_HQ) == "hq"
    assert prores_profile_name(PRORES_PROFILE_4444) == "4444"
    assert prores_profile_name(PRORES_PROFILE_XQ) == "xq"


def test_prores_profile_names_table_matches_range():
    """``PRORES_PROFILE_NAMES`` must align with the integer range."""
    lo, hi = PRORES_PROFILE_RANGE
    assert lo == 0
    assert hi == 5
    assert len(PRORES_PROFILE_NAMES) == hi - lo + 1


def test_prores_profile_name_rejects_out_of_range():
    with pytest.raises(ValueError):
        prores_profile_name(-1)
    with pytest.raises(ValueError):
        prores_profile_name(6)


def test_prores_validate_accepts_full_range():
    a = ProresVideoToolboxAdapter()
    a.validate("medium", PRORES_PROFILE_PROXY)
    a.validate("ultrafast", PRORES_PROFILE_LT)
    a.validate("veryslow", PRORES_PROFILE_XQ)


def test_prores_validate_rejects_out_of_range():
    a = ProresVideoToolboxAdapter()
    with pytest.raises(ValueError):
        a.validate("medium", -1)
    with pytest.raises(ValueError):
        a.validate("medium", 6)
    with pytest.raises(ValueError):
        a.validate("nope", PRORES_PROFILE_HQ)


def test_validate_prores_videotoolbox_helper_matches_constants():
    validate_prores_videotoolbox("medium", PRORES_PROFILE_PROXY)
    validate_prores_videotoolbox("medium", PRORES_PROFILE_XQ)
    with pytest.raises(ValueError):
        validate_prores_videotoolbox("medium", PRORES_PROFILE_XQ + 1)


@pytest.mark.parametrize(
    "tier_id,tier_name",
    [
        (PRORES_PROFILE_PROXY, "proxy"),
        (PRORES_PROFILE_LT, "lt"),
        (PRORES_PROFILE_STANDARD, "standard"),
        (PRORES_PROFILE_HQ, "hq"),
        (PRORES_PROFILE_4444, "4444"),
        (PRORES_PROFILE_XQ, "xq"),
    ],
)
def test_prores_ffmpeg_codec_args_per_tier(tier_id: int, tier_name: str):
    """Every tier emits ``-c:v prores_videotoolbox -realtime N
    -profile:v <name>`` with the canonical FFmpeg alias."""
    a = ProresVideoToolboxAdapter()
    args = a.ffmpeg_codec_args("medium", tier_id)
    # Codec selection.
    assert args[0] == "-c:v"
    assert args[1] == "prores_videotoolbox"
    # Realtime mapping (medium → 0).
    assert args[2] == "-realtime"
    assert args[3] == "0"
    # Profile alias (the named CONST FFmpeg accepts on the AVOption).
    assert args[4] == "-profile:v"
    assert args[5] == tier_name


def test_prores_realtime_mapping_for_fast_preset():
    """Fast presets emit ``-realtime 1``."""
    a = ProresVideoToolboxAdapter()
    args = a.ffmpeg_codec_args("ultrafast", PRORES_PROFILE_HQ)
    assert "-realtime" in args
    assert args[args.index("-realtime") + 1] == "1"


def test_prores_probe_args_uses_proxy_tier():
    """The predictor probe runs at the smallest tier."""
    a = ProresVideoToolboxAdapter()
    probe = a.probe_args()
    assert "-profile:v" in probe
    assert probe[probe.index("-profile:v") + 1] == "proxy"


def test_prores_extra_params_is_empty():
    a = ProresVideoToolboxAdapter()
    assert a.extra_params() == ()


def test_prores_default_quality_is_hq():
    """422 HQ is the broadcast acquisition standard; default to it."""
    assert PRORES_PROFILE_DEFAULT == PRORES_PROFILE_HQ


def test_prores_gop_args_round_trip():
    """ProRes is intra-only but the harness still emits ``-g`` uniformly."""
    a = ProresVideoToolboxAdapter()
    args = a.gop_args(48)
    assert args == ("-g", "48")


def test_prores_force_keyframes_args_round_trip():
    """Keyframe pinning is harmless on ProRes (every frame is a keyframe)."""
    a = ProresVideoToolboxAdapter()
    assert a.force_keyframes_args(()) == ()
    args = a.force_keyframes_args((1.0, 2.5))
    assert args[0] == "-force_key_frames"


def test_prores_encode_argv_shape_via_mock(tmp_path: Path):
    """Smoke: build_ffmpeg_command + run_encode with a mocked subprocess."""
    captured: dict = {}

    def fake_run(cmd, capture_output, text, check):
        captured["cmd"] = cmd
        Path(cmd[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 8.1.1\n")

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x80" * 1024)
    out = tmp_path / "out.mov"

    req = EncodeRequest(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv422p10le",
        framerate=24.0,
        encoder="prores_videotoolbox",
        preset="medium",
        crf=PRORES_PROFILE_HQ,
        output=out,
    )
    cmd = build_ffmpeg_command(req)
    assert "prores_videotoolbox" in cmd
    assert "-c:v" in cmd

    res = run_encode(req, runner=fake_run)
    assert res.exit_status == 0
    assert "prores_videotoolbox" in captured["cmd"]


def test_prores_videotoolbox_outside_encoder_vocab_v2():
    """Sanity guard: ProRes is intentionally outside ENCODER_VOCAB_V2.

    ADR-0291 froze the v2 12-slot vocab; per ADR-0283 the H.264 / HEVC
    VT adapters ship without a vocab bump. ProRes follows the same
    pattern — the proxy fast path raises ``ProxyError`` for ProRes
    until a future retrain expands the vocab. The harness's live-encode
    path (this adapter) still works; only the ONNX proxy is gated.
    """
    from vmaftune.proxy import ENCODER_VOCAB_V2

    assert "prores_videotoolbox" not in ENCODER_VOCAB_V2

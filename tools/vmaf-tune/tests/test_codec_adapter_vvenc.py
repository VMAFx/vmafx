# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the libvvenc (VVC / H.266) codec adapter.

The adapter exposes a curated set of real VVenC 1.14.0 tuning knobs
forwarded via FFmpeg's opaque ``-vvenc-params key=value:...`` channel.
The legacy ``nnvc_intra`` toggle (which fabricated a non-existent
``IntraNN`` config key) was removed on 2026-05-09; see ADR-0285.

These tests cover:
    1. Default-off bit-exact baseline (empty ``extra_params()`` so the
       pre-existing Phase A grid keeps reproducing the same argv).
    2. Per-toggle ``-vvenc-params`` emission (one test per knob).
    3. Combined-toggle byte-stable ordering (search-loop / cache-key
       relies on deterministic argv).
    4. Validation rejects bad inputs.
    5. Mocked subprocess smoke for ~3 representative configurations.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import VVenCAdapter, get_adapter, known_codecs  # noqa: E402
from vmaftune.codec_adapters.vvenc import native_presets, supported_tiers  # noqa: E402
from vmaftune.encode import EncodeRequest, build_ffmpeg_command, run_encode  # noqa: E402

# ---------- Registry / contract -----------------------------------------------


def test_vvenc_registered_in_registry():
    assert "libvvenc" in known_codecs()
    a = get_adapter("libvvenc")
    assert a.encoder == "libvvenc"
    assert a.quality_knob == "qp"
    assert a.invert_quality is True
    lo, hi = a.quality_range
    assert 0 <= lo < hi <= 63


def test_vvenc_native_preset_compresses_seven_to_five():
    a = VVenCAdapter()
    # The fork's canonical superset compresses onto VVenC's 5 levels.
    assert a.native_preset("placebo") == "slower"
    assert a.native_preset("slowest") == "slower"
    assert a.native_preset("slower") == "slower"
    assert a.native_preset("slow") == "slow"
    assert a.native_preset("medium") == "medium"
    assert a.native_preset("fast") == "fast"
    assert a.native_preset("faster") == "faster"
    assert a.native_preset("veryfast") == "faster"
    assert a.native_preset("superfast") == "faster"
    assert a.native_preset("ultrafast") == "faster"


def test_vvenc_native_preset_vocabulary_matches_encoder():
    # The five preset names VVenC's CLI / FFmpeg wrapper accept.
    assert native_presets() == ("faster", "fast", "medium", "slow", "slower")


def test_vvenc_supported_tiers_match_vvenc_app_cfg():
    # VVEncAppCfg.h v1.14.0 line 183 — TierToEnumMap.
    assert supported_tiers() == ("high", "main")


def test_vvenc_native_preset_rejects_unknown():
    a = VVenCAdapter()
    with pytest.raises(ValueError):
        a.native_preset("warpspeed")


def test_vvenc_quality_range_is_h266_canonical_window():
    a = VVenCAdapter()
    lo, hi = a.quality_range
    # Default sits inside the surfaced window.
    assert lo <= a.quality_default <= hi


def test_vvenc_adapter_version_bumped_for_v2_surface():
    # Cache-key invalidation: the surface changed in 2026-05-09 (NNVC
    # toggle removed, real tuning knobs added) — the bump is mandatory
    # per ADR-0298 so stale cached results don't survive the rev.
    a = VVenCAdapter()
    assert a.adapter_version == "2"


# ---------- Default-off bit-exact baseline ------------------------------------


def test_vvenc_extra_params_default_off():
    """All tuning knobs default to ``None`` → no ``-vvenc-params`` emitted.

    This is the bit-exact-baseline gate. If a future change makes a knob
    default to a non-None value, the Phase A grid's encode argv shifts
    and the cached results invalidate without an explicit
    ``adapter_version`` bump. Pin it.
    """
    a = VVenCAdapter()
    assert a.extra_params() == ()


def test_vvenc_validate_default_passes():
    a = VVenCAdapter()
    a.validate("medium", 32)


# ---------- Per-toggle ``-vvenc-params`` emission -----------------------------


def _payload(adapter: VVenCAdapter) -> str:
    """Pull the colon-joined KV string out of ``extra_params()``."""
    extras = adapter.extra_params()
    assert "-vvenc-params" in extras, extras
    return extras[extras.index("-vvenc-params") + 1]


def test_vvenc_perceptual_qpa_emits_perceptqpa_key():
    assert "PerceptQPA=1" in _payload(VVenCAdapter(perceptual_qpa=True))
    assert "PerceptQPA=0" in _payload(VVenCAdapter(perceptual_qpa=False))


def test_vvenc_internal_bitdepth_emits_internalbitdepth_key():
    assert _payload(VVenCAdapter(internal_bitdepth=10)) == "InternalBitDepth=10"
    assert _payload(VVenCAdapter(internal_bitdepth=8)) == "InternalBitDepth=8"


def test_vvenc_tier_emits_tier_key():
    assert _payload(VVenCAdapter(tier="main")) == "Tier=main"
    assert _payload(VVenCAdapter(tier="high")) == "Tier=high"


def test_vvenc_tiles_emits_nxm_geometry():
    # VVenC's ``Tiles`` parser splits on 'x' (VVEncAppCfg.h:547).
    assert _payload(VVenCAdapter(tiles=(2, 2))) == "Tiles=2x2"
    assert _payload(VVenCAdapter(tiles=(4, 1))) == "Tiles=4x1"


def test_vvenc_max_parallel_frames_emits_key():
    assert _payload(VVenCAdapter(max_parallel_frames=4)) == "MaxParallelFrames=4"
    assert _payload(VVenCAdapter(max_parallel_frames=0)) == "MaxParallelFrames=0"


def test_vvenc_rpr_emits_key():
    for v in (0, 1, 2):
        assert _payload(VVenCAdapter(rpr=v)) == f"RPR={v}"


def test_vvenc_sao_emits_key():
    assert _payload(VVenCAdapter(sao=True)) == "SAO=1"
    assert _payload(VVenCAdapter(sao=False)) == "SAO=0"


def test_vvenc_alf_emits_key():
    assert _payload(VVenCAdapter(alf=True)) == "ALF=1"
    assert _payload(VVenCAdapter(alf=False)) == "ALF=0"


def test_vvenc_ccalf_emits_key():
    assert _payload(VVenCAdapter(ccalf=True)) == "CCALF=1"
    assert _payload(VVenCAdapter(ccalf=False)) == "CCALF=0"


# ---------- Combined-toggle byte-stable ordering ------------------------------


def test_vvenc_combined_toggles_byte_stable_order():
    """Field declaration order pins the emitted KV order.

    The cache key (ADR-0298) and the corpus-row ``encoder_extra_params``
    column hash on argv. If the order drifted with insertion order or
    set ordering, equivalent encodes would cache-miss and the predictor
    would see two distinct strings for one config. Pin it.
    """
    a = VVenCAdapter(
        perceptual_qpa=True,
        internal_bitdepth=10,
        tier="main",
        tiles=(2, 2),
        max_parallel_frames=4,
        rpr=1,
        sao=True,
        alf=True,
        ccalf=True,
    )
    payload = _payload(a)
    expected = (
        "PerceptQPA=1:"
        "InternalBitDepth=10:"
        "Tier=main:"
        "Tiles=2x2:"
        "MaxParallelFrames=4:"
        "RPR=1:"
        "SAO=1:"
        "ALF=1:"
        "CCALF=1"
    )
    assert payload == expected


def test_vvenc_two_toggles_argv_is_byte_stable():
    """A 2-toggle config should produce one well-known argv string."""
    a = VVenCAdapter(perceptual_qpa=True, internal_bitdepth=10)
    assert a.extra_params() == ("-vvenc-params", "PerceptQPA=1:InternalBitDepth=10")


# ---------- Validation -------------------------------------------------------


def test_vvenc_validate_rejects_bad_preset_qp():
    a = VVenCAdapter()
    with pytest.raises(ValueError):
        a.validate("warpspeed", 32)
    with pytest.raises(ValueError):
        a.validate("medium", -1)
    with pytest.raises(ValueError):
        a.validate("medium", 64)


def test_vvenc_validate_rejects_bad_tier():
    with pytest.raises(ValueError):
        VVenCAdapter(tier="medium").validate("medium", 32)


def test_vvenc_validate_rejects_bad_internal_bitdepth():
    with pytest.raises(ValueError):
        VVenCAdapter(internal_bitdepth=12).validate("medium", 32)


def test_vvenc_validate_rejects_bad_tiles():
    with pytest.raises(ValueError):
        VVenCAdapter(tiles=(0, 1)).validate("medium", 32)


def test_vvenc_validate_rejects_bad_max_parallel_frames():
    with pytest.raises(ValueError):
        VVenCAdapter(max_parallel_frames=-1).validate("medium", 32)


def test_vvenc_validate_rejects_bad_rpr():
    with pytest.raises(ValueError):
        VVenCAdapter(rpr=3).validate("medium", 32)


# ---------- build_ffmpeg_command integration ---------------------------------


def test_vvenc_ffmpeg_command_carries_native_preset_and_qp():
    a = VVenCAdapter(perceptual_qpa=True, tiles=(2, 2))
    a.validate("slower", 27)
    req = EncodeRequest(
        source=Path("/tmp/ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=a.encoder,
        preset=a.native_preset("slower"),
        crf=27,  # encoder-agnostic name; carries QP for VVenC
        output=Path("/tmp/out.mkv"),
        extra_params=a.extra_params(),
    )
    cmd = build_ffmpeg_command(req)
    assert "-c:v" in cmd
    assert cmd[cmd.index("-c:v") + 1] == "libvvenc"
    assert "-preset" in cmd
    assert cmd[cmd.index("-preset") + 1] == "slower"
    # Tuning knobs ride through extra_params as -vvenc-params.
    assert "-vvenc-params" in cmd
    payload = cmd[cmd.index("-vvenc-params") + 1]
    assert "PerceptQPA=1" in payload
    assert "Tiles=2x2" in payload


def test_vvenc_ffmpeg_command_no_vvenc_params_when_default():
    """Default-off adapter produces argv with no ``-vvenc-params`` token.

    This is the byte-stable-baseline gate against the pre-existing
    Phase A grid: encodes that did not pass tuning knobs before this
    revision must produce the same argv after.
    """
    a = VVenCAdapter()
    req = EncodeRequest(
        source=Path("/tmp/ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=a.encoder,
        preset=a.native_preset("medium"),
        crf=32,
        output=Path("/tmp/out.mkv"),
        extra_params=a.extra_params(),
    )
    cmd = build_ffmpeg_command(req)
    assert "-vvenc-params" not in cmd


# ---------- Mocked-subprocess smoke ------------------------------------------


class _FakeCompleted:
    def __init__(self, stderr: str = "", returncode: int = 0):
        self.stderr = stderr
        self.returncode = returncode
        self.stdout = ""


def _fake_runner_factory(captured: list[list[str]]):
    """Return a runner stub that records argv and returns a clean exit."""

    def _runner(cmd, capture_output, text, check):  # noqa: ARG001
        captured.append(list(cmd))
        return _FakeCompleted(stderr="ffmpeg version 8.1\n")

    return _runner


def test_vvenc_subprocess_smoke_default_off(tmp_path):
    """Smoke #1: default-off encode produces a clean argv with no knobs."""
    captured: list[list[str]] = []
    a = VVenCAdapter()
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=640,
        height=360,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=a.encoder,
        preset=a.native_preset("medium"),
        crf=32,
        output=tmp_path / "out.mkv",
        extra_params=a.extra_params(),
    )
    result = run_encode(req, runner=_fake_runner_factory(captured))
    assert result.exit_status == 0
    assert len(captured) == 1
    assert "-vvenc-params" not in captured[0]


def test_vvenc_subprocess_smoke_perceptual_qpa(tmp_path):
    """Smoke #2: perceptual_qpa=True forwards PerceptQPA=1 to ffmpeg."""
    captured: list[list[str]] = []
    a = VVenCAdapter(perceptual_qpa=True)
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder=a.encoder,
        preset=a.native_preset("slow"),
        crf=27,
        output=tmp_path / "out.mkv",
        extra_params=a.extra_params(),
    )
    result = run_encode(req, runner=_fake_runner_factory(captured))
    assert result.exit_status == 0
    cmd = captured[0]
    idx = cmd.index("-vvenc-params")
    assert cmd[idx + 1] == "PerceptQPA=1"


def test_vvenc_subprocess_smoke_full_combined(tmp_path):
    """Smoke #3: a representative production-shaped multi-knob encode."""
    captured: list[list[str]] = []
    a = VVenCAdapter(
        perceptual_qpa=True,
        internal_bitdepth=10,
        tier="main",
        tiles=(2, 2),
        max_parallel_frames=4,
        sao=True,
        alf=True,
        ccalf=True,
    )
    req = EncodeRequest(
        source=tmp_path / "ref.yuv",
        width=3840,
        height=2160,
        pix_fmt="yuv420p10le",
        framerate=24.0,
        encoder=a.encoder,
        preset=a.native_preset("slower"),
        crf=27,
        output=tmp_path / "out.mkv",
        extra_params=a.extra_params(),
    )
    result = run_encode(req, runner=_fake_runner_factory(captured))
    assert result.exit_status == 0
    cmd = captured[0]
    idx = cmd.index("-vvenc-params")
    payload = cmd[idx + 1]
    assert payload == (
        "PerceptQPA=1:"
        "InternalBitDepth=10:"
        "Tier=main:"
        "Tiles=2x2:"
        "MaxParallelFrames=4:"
        "SAO=1:"
        "ALF=1:"
        "CCALF=1"
    )


# ---------- ffmpeg_codec_args (CodecAdapter Protocol) ------------------------


def test_vvenc_ffmpeg_codec_args_shape():
    a = VVenCAdapter()
    args = a.ffmpeg_codec_args("medium", 32)
    assert args == ["-c:v", "libvvenc", "-preset", "medium", "-qp", "32"]


def test_vvenc_ffmpeg_codec_args_compresses_canonical_preset():
    a = VVenCAdapter()
    # "placebo" projects onto "slower" via the static map.
    args = a.ffmpeg_codec_args("placebo", 25)
    assert args == ["-c:v", "libvvenc", "-preset", "slower", "-qp", "25"]

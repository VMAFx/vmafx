# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Real 2-pass argv coverage for every codec adapter (ADR-0546).

Extends the Phase F suite (``test_codec_adapter_x265_two_pass.py``) to
cover the 14 adapters that previously raised ``NotImplementedError``:

- Software (true two-invocation 2-pass): ``libsvtav1``, ``libaom-av1``,
  ``libvvenc``.
- Hardware single-invocation analogues: ``h264_nvenc`` / ``hevc_nvenc``
  / ``av1_nvenc`` (multipass fullres), ``h264_qsv`` / ``hevc_qsv`` /
  ``av1_qsv`` (extended-BRC look-ahead), ``h264_amf`` / ``hevc_amf`` /
  ``av1_amf`` (pre-analysis).
- API-limited refusal: ``h264_videotoolbox`` / ``hevc_videotoolbox`` /
  ``av1_videotoolbox`` (and ``prores_videotoolbox`` for completeness)
  raise :class:`VideoToolboxTwoPassUnsupportedError`.

Test strategy:

1. **Argv shape** — each adapter's ``two_pass_args(1, p)`` and
   ``two_pass_args(2, p)`` returns a real tuple of ffmpeg flags
   matching the per-vendor contract.
2. **Pass 0 sentinel** — ``two_pass_args(0, p)`` returns ``()`` so
   callers can forward unconditionally.
3. **Out-of-range pass numbers** — software adapters raise
   ``ValueError`` for any pass number outside ``{0, 1, 2}``.
4. **VideoToolbox refusal** — always raises
   :class:`VideoToolboxTwoPassUnsupportedError`.
5. **supports_two_pass flag** — software adapters set ``True``;
   hardware adapters (including VideoToolbox) set ``False`` so the
   :func:`vmaftune.encode.run_two_pass_encode` driver falls back to
   single-pass instead of running two redundant invocations.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (
    AV1AMFAdapter,
    Av1NvencAdapter,
    Av1QsvAdapter,
    Av1VideoToolboxAdapter,
    H264AMFAdapter,
    H264NvencAdapter,
    H264QsvAdapter,
    H264VideoToolboxAdapter,
    HEVCAMFAdapter,
    HevcNvencAdapter,
    HevcQsvAdapter,
    HEVCVideoToolboxAdapter,
    LibaomAdapter,
    ProresVideoToolboxAdapter,
    SvtAv1Adapter,
    VVenCAdapter,
    get_adapter,
)
from vmaftune.codec_adapters._videotoolbox_common import (
    VideoToolboxTwoPassUnsupportedError,
)

# --- Software encoders: true two-invocation 2-pass --------------------


def test_svtav1_supports_two_pass_false_because_crf_mode():
    # SVT-AV1 enforces "CRF does not support multi-pass" at runtime;
    # the harness pins CRF as the canonical quality axis, so the
    # 2-pass driver falls back to single-pass for libsvtav1.
    a = SvtAv1Adapter()
    assert getattr(a, "supports_two_pass", False) is False


def test_svtav1_pass1_emits_generic_passlogfile():
    # The argv is still meaningful for callers that override into VBR
    # mode via extra_params; the harness simply doesn't drive the
    # two-invocation loop because the default mode forbids it.
    args = SvtAv1Adapter().two_pass_args(1, Path("/tmp/svt.stats"))
    assert args == ("-pass", "1", "-passlogfile", "/tmp/svt.stats")


def test_svtav1_pass2_emits_generic_passlogfile():
    args = SvtAv1Adapter().two_pass_args(2, Path("/tmp/svt.stats"))
    assert args == ("-pass", "2", "-passlogfile", "/tmp/svt.stats")


def test_svtav1_pass0_is_empty_tuple():
    assert SvtAv1Adapter().two_pass_args(0, Path("/tmp/svt.stats")) == ()


def test_svtav1_invalid_pass_number_raises():
    a = SvtAv1Adapter()
    with pytest.raises(ValueError):
        a.two_pass_args(3, Path("/tmp/svt.stats"))
    with pytest.raises(ValueError):
        a.two_pass_args(-1, Path("/tmp/svt.stats"))


def test_libaom_advertises_two_pass_support():
    a = LibaomAdapter()
    assert getattr(a, "supports_two_pass", False) is True


def test_libaom_pass1_emits_generic_passlogfile():
    args = LibaomAdapter().two_pass_args(1, Path("/tmp/aom.stats"))
    assert args == ("-pass", "1", "-passlogfile", "/tmp/aom.stats")


def test_libaom_pass2_emits_generic_passlogfile():
    args = LibaomAdapter().two_pass_args(2, Path("/tmp/aom.stats"))
    assert args == ("-pass", "2", "-passlogfile", "/tmp/aom.stats")


def test_libaom_pass0_is_empty_tuple():
    assert LibaomAdapter().two_pass_args(0, Path("/tmp/aom.stats")) == ()


def test_libaom_invalid_pass_number_raises():
    with pytest.raises(ValueError):
        LibaomAdapter().two_pass_args(5, Path("/tmp/aom.stats"))


def test_vvenc_advertises_two_pass_support():
    a = VVenCAdapter()
    assert getattr(a, "supports_two_pass", False) is True


def test_vvenc_pass1_emits_generic_passlogfile():
    args = VVenCAdapter().two_pass_args(1, Path("/tmp/vvc.stats"))
    assert args == ("-pass", "1", "-passlogfile", "/tmp/vvc.stats")


def test_vvenc_pass2_emits_generic_passlogfile():
    args = VVenCAdapter().two_pass_args(2, Path("/tmp/vvc.stats"))
    assert args == ("-pass", "2", "-passlogfile", "/tmp/vvc.stats")


def test_vvenc_pass0_is_empty_tuple():
    assert VVenCAdapter().two_pass_args(0, Path("/tmp/vvc.stats")) == ()


def test_vvenc_invalid_pass_number_raises():
    with pytest.raises(ValueError):
        VVenCAdapter().two_pass_args(99, Path("/tmp/vvc.stats"))


# --- NVENC family: hardware multipass ---------------------------------


@pytest.mark.parametrize(
    "cls",
    [H264NvencAdapter, HevcNvencAdapter, Av1NvencAdapter],
)
def test_nvenc_supports_two_pass_is_false(cls):
    # Hardware analog runs inside one invocation; the 2-pass driver
    # should fall back to single-pass rather than spawn two redundant
    # encode jobs.
    assert getattr(cls(), "supports_two_pass", False) is False


@pytest.mark.parametrize(
    "cls",
    [H264NvencAdapter, HevcNvencAdapter, Av1NvencAdapter],
)
def test_nvenc_pass1_emits_multipass_fullres(cls):
    args = cls().two_pass_args(1, Path("/tmp/nv.stats"))
    assert args == ("-multipass", "fullres")


@pytest.mark.parametrize(
    "cls",
    [H264NvencAdapter, HevcNvencAdapter, Av1NvencAdapter],
)
def test_nvenc_pass2_is_empty(cls):
    # The multipass analysis already ran inside the single pass-1
    # invocation; a second encode would just be a separate single-
    # pass run.
    assert cls().two_pass_args(2, Path("/tmp/nv.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264NvencAdapter, HevcNvencAdapter, Av1NvencAdapter],
)
def test_nvenc_pass0_is_empty(cls):
    assert cls().two_pass_args(0, Path("/tmp/nv.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264NvencAdapter, HevcNvencAdapter, Av1NvencAdapter],
)
def test_nvenc_invalid_pass_number_raises(cls):
    with pytest.raises(ValueError):
        cls().two_pass_args(7, Path("/tmp/nv.stats"))


# --- QSV family: extended-BRC look-ahead -------------------------------


@pytest.mark.parametrize(
    "cls",
    [H264QsvAdapter, HevcQsvAdapter, Av1QsvAdapter],
)
def test_qsv_supports_two_pass_is_false(cls):
    assert getattr(cls(), "supports_two_pass", False) is False


@pytest.mark.parametrize(
    "cls",
    [H264QsvAdapter, HevcQsvAdapter, Av1QsvAdapter],
)
def test_qsv_pass1_emits_lookahead(cls):
    args = cls().two_pass_args(1, Path("/tmp/qsv.stats"))
    assert args == ("-extbrc", "1", "-look_ahead_depth", "40")


@pytest.mark.parametrize(
    "cls",
    [H264QsvAdapter, HevcQsvAdapter, Av1QsvAdapter],
)
def test_qsv_pass2_is_empty(cls):
    assert cls().two_pass_args(2, Path("/tmp/qsv.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264QsvAdapter, HevcQsvAdapter, Av1QsvAdapter],
)
def test_qsv_pass0_is_empty(cls):
    assert cls().two_pass_args(0, Path("/tmp/qsv.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264QsvAdapter, HevcQsvAdapter, Av1QsvAdapter],
)
def test_qsv_invalid_pass_number_raises(cls):
    with pytest.raises(ValueError):
        cls().two_pass_args(4, Path("/tmp/qsv.stats"))


# --- AMF family: pre-analysis -----------------------------------------


@pytest.mark.parametrize(
    "cls",
    [H264AMFAdapter, HEVCAMFAdapter, AV1AMFAdapter],
)
def test_amf_supports_two_pass_is_false(cls):
    assert getattr(cls(), "supports_two_pass", False) is False


@pytest.mark.parametrize(
    "cls",
    [H264AMFAdapter, HEVCAMFAdapter, AV1AMFAdapter],
)
def test_amf_pass1_emits_preanalysis(cls):
    args = cls().two_pass_args(1, Path("/tmp/amf.stats"))
    assert args == ("-preanalysis", "true")


@pytest.mark.parametrize(
    "cls",
    [H264AMFAdapter, HEVCAMFAdapter, AV1AMFAdapter],
)
def test_amf_pass2_is_empty(cls):
    assert cls().two_pass_args(2, Path("/tmp/amf.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264AMFAdapter, HEVCAMFAdapter, AV1AMFAdapter],
)
def test_amf_pass0_is_empty(cls):
    assert cls().two_pass_args(0, Path("/tmp/amf.stats")) == ()


@pytest.mark.parametrize(
    "cls",
    [H264AMFAdapter, HEVCAMFAdapter, AV1AMFAdapter],
)
def test_amf_invalid_pass_number_raises(cls):
    with pytest.raises(ValueError):
        cls().two_pass_args(6, Path("/tmp/amf.stats"))


# --- VideoToolbox family: single-pass only by API ----------------------


@pytest.mark.parametrize(
    "cls",
    [
        H264VideoToolboxAdapter,
        HEVCVideoToolboxAdapter,
        Av1VideoToolboxAdapter,
        ProresVideoToolboxAdapter,
    ],
)
def test_videotoolbox_supports_two_pass_is_false(cls):
    assert getattr(cls(), "supports_two_pass", False) is False


@pytest.mark.parametrize(
    "cls",
    [
        H264VideoToolboxAdapter,
        HEVCVideoToolboxAdapter,
        Av1VideoToolboxAdapter,
        ProresVideoToolboxAdapter,
    ],
)
def test_videotoolbox_pass1_raises_unsupported(cls):
    with pytest.raises(VideoToolboxTwoPassUnsupportedError) as exc_info:
        cls().two_pass_args(1, Path("/tmp/vt.stats"))
    # Error message must name the encoder + point users at the software
    # fallback list; otherwise users have no path forward.
    msg = str(exc_info.value)
    assert "single-pass only" in msg
    assert "libx264" in msg


@pytest.mark.parametrize(
    "cls",
    [
        H264VideoToolboxAdapter,
        HEVCVideoToolboxAdapter,
        Av1VideoToolboxAdapter,
        ProresVideoToolboxAdapter,
    ],
)
def test_videotoolbox_pass2_raises_unsupported(cls):
    with pytest.raises(VideoToolboxTwoPassUnsupportedError):
        cls().two_pass_args(2, Path("/tmp/vt.stats"))


def test_videotoolbox_error_subclasses_notimplemented():
    # Existing callers that catch NotImplementedError (the prior
    # default base-class behaviour) keep working.
    assert issubclass(VideoToolboxTwoPassUnsupportedError, NotImplementedError)


# --- Registry-wide coverage ------------------------------------------


_SUPPORTED_TWO_PASS = {"libx264", "libx265", "libvpx-vp9", "libaom-av1", "libvvenc"}
_UNSUPPORTED_BUT_RESPONSIVE = {
    # SVT-AV1 forbids multi-pass in CRF mode (the harness default), so
    # the driver short-circuits to single-pass; the adapter still
    # returns meaningful VBR-mode argv for explicit-override callers.
    "libsvtav1",
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "h264_amf",
    "hevc_amf",
    "av1_amf",
}
_UNSUPPORTED_AND_REFUSING = {
    "h264_videotoolbox",
    "hevc_videotoolbox",
    "av1_videotoolbox",
    "prores_videotoolbox",
}


def test_every_adapter_responds_to_two_pass_call():
    """No adapter should silently raise the bare protocol-default
    NotImplementedError. Either it returns argv, or it raises the
    documented VideoToolbox-unsupported error."""
    for codec in _SUPPORTED_TWO_PASS | _UNSUPPORTED_BUT_RESPONSIVE:
        a = get_adapter(codec)
        result = a.two_pass_args(1, Path("/tmp/foo.stats"))
        assert isinstance(result, tuple)
        assert len(result) > 0, f"{codec} returned empty argv for pass 1"

    for codec in _UNSUPPORTED_AND_REFUSING:
        a = get_adapter(codec)
        with pytest.raises(VideoToolboxTwoPassUnsupportedError):
            a.two_pass_args(1, Path("/tmp/foo.stats"))

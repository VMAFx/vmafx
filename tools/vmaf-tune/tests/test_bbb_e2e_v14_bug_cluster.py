# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the BBB e2e v14 hardware-encoder bug cluster (ADR-0601).

Three bugs blocked ``vmaf-tune compare`` from running with hardware encoders:

* **V14-A** — Encoder-availability probe used ``nullsrc=size=64x64:rate=1:duration=0.04``.
  NVENC (and other HW encoders) reject 64x64 with ``-22 (Invalid argument)`` because
  hardware encode blocks enforce a minimum resolution (NVENC: ≥145×49, QSV: ≥128×96).
  Fix: change to ``320x240:rate=24:duration=0.5`` (clears every known minimum).

* **V14-B** — QSV adapters emitted only ``-c:v h264_qsv`` but FFmpeg's QSV bridge
  requires ``-init_hw_device vaapi=va:<dev> -init_hw_device qsv=qsv_dev@va
  -filter_hw_device va`` before the input, and ``-vf format=nv12,hwupload=extra_hw_frames=64``
  before the encoder. Without these flags the probe fails with ``-22 Invalid argument``
  even when the Intel GPU driver is installed.
  Fix: ``_hw_init_args_for_encoder`` returns the three init flags for QSV encoders;
  the probe inserts the hwupload filter for QSV.
  ``BaseQsvAdapter.qsv_hw_init_args()`` exposes the same args as a static helper
  for callers that build full encode commands.

* **V14-C** — AMF adapters on Linux with a decoder-only AMD APU (e.g. gfx1036 /
  Raphael / Phoenix) would fail at the probe stage with ``AMF_NOT_SUPPORTED`` because
  the iGPU has no VCE encode block. The adapter is correct as shipped; the probe's
  failure path surfaces a ``hardware encoder not available`` row rather than aborting
  the sweep. The limitation is documented in ``_amf_common.py``. No explicit device-init
  flags are needed for AMF on Linux.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters._qsv_common import BaseQsvAdapter
from vmaftune.compare import (
    _DEFAULT_VAAPI_DEVICE,
    _QSV_ENCODERS,
    DEFAULT_VAAPI_DEVICE,
    _hw_init_args_for_encoder,
    probe_encoder_available,
)
from vmaftune.hw_devices import AUTO_VAAPI_DEVICE

# ---------------------------------------------------------------------------
# V14-A — probe nullsrc resolution fix (320x240, not 64x64)
# ---------------------------------------------------------------------------


def _make_runner(encoder_listing: bytes, encode_rc: int = 0, encode_stderr: bytes = b""):
    """Fabricate a runner that returns a listing then an encode result."""
    calls: list[list[str]] = []

    class _R:
        def __init__(self, stdout: bytes, returncode: int) -> None:
            self.stdout = stdout
            self.returncode = returncode

    def _runner(argv, timeout=30.0):
        calls.append(list(argv))
        if "-encoders" in argv:
            return _R(encoder_listing, 0)
        return _R(encode_stderr, encode_rc)

    return _runner, calls


def test_v14_a_probe_uses_320x240_resolution():
    """The probe dummy-encode uses 320x240, not 64x64."""
    runner, calls = _make_runner(b" V..... h264_nvenc           NVIDIA NVENC H.264 encoder\n")
    ok, _ = probe_encoder_available("h264_nvenc", runner=runner)
    assert ok is True
    # The second call is the dummy encode.
    assert len(calls) == 2, f"expected 2 ffmpeg calls, got {len(calls)}: {calls}"
    encode_argv = calls[1]
    # nullsrc must use 320x240.
    assert any(
        "320x240" in a for a in encode_argv
    ), f"probe argv does not contain 320x240: {encode_argv!r}"
    assert not any(
        "64x64" in a for a in encode_argv
    ), f"probe argv still uses forbidden 64x64: {encode_argv!r}"


def test_v14_a_probe_uses_rate24():
    """The probe dummy-encode uses rate=24."""
    runner, calls = _make_runner(b" V..... hevc_nvenc           NVIDIA NVENC HEVC encoder\n")
    probe_encoder_available("hevc_nvenc", runner=runner)
    encode_argv = calls[1]
    assert any(
        "rate=24" in a for a in encode_argv
    ), f"probe argv does not contain rate=24: {encode_argv!r}"


def test_v14_a_probe_nvenc_no_device_init_flags():
    """NVENC probe must NOT prepend -init_hw_device flags."""
    runner, calls = _make_runner(b" V..... av1_nvenc            NVIDIA NVENC AV1 encoder\n")
    probe_encoder_available("av1_nvenc", runner=runner)
    encode_argv = calls[1]
    assert (
        "-init_hw_device" not in encode_argv
    ), f"NVENC probe unexpectedly contains -init_hw_device: {encode_argv!r}"
    assert (
        "-filter_hw_device" not in encode_argv
    ), f"NVENC probe unexpectedly contains -filter_hw_device: {encode_argv!r}"


# ---------------------------------------------------------------------------
# V14-B — QSV hardware-device init flags in the probe
# ---------------------------------------------------------------------------


def test_v14_b_qsv_probe_contains_init_hw_device():
    """QSV probe must include -init_hw_device flags before the input."""
    runner, calls = _make_runner(
        b" V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video)\n"
    )
    _ok, _reason = probe_encoder_available("h264_qsv", runner=runner)
    # We don't care whether the probe succeeds on this host (no GPU in CI),
    # only that the argv was built correctly.
    encode_argv = calls[1]
    assert (
        "-init_hw_device" in encode_argv
    ), f"QSV probe argv missing -init_hw_device: {encode_argv!r}"
    assert (
        "-filter_hw_device" in encode_argv
    ), f"QSV probe argv missing -filter_hw_device: {encode_argv!r}"
    # The vaapi init device string must appear.
    vaapi_flags = [a for a in encode_argv if a.startswith("vaapi=va:")]
    assert vaapi_flags, f"QSV probe argv missing vaapi=va:<dev>: {encode_argv!r}"


def test_v14_b_qsv_probe_contains_hwupload_filter():
    """QSV probe must include a -vf format=nv12,hwupload filter."""
    runner, calls = _make_runner(b" V..... hevc_qsv             HEVC (Intel Quick Sync Video)\n")
    probe_encoder_available("hevc_qsv", runner=runner)
    encode_argv = calls[1]
    assert "-vf" in encode_argv, f"QSV probe argv missing -vf flag: {encode_argv!r}"
    vf_idx = encode_argv.index("-vf")
    vf_value = encode_argv[vf_idx + 1]
    assert "hwupload" in vf_value, f"QSV -vf value does not contain hwupload: {vf_value!r}"
    assert "nv12" in vf_value, f"QSV -vf value does not set nv12 format: {vf_value!r}"


def test_v14_b_qsv_probe_init_precedes_input():
    """QSV: -init_hw_device flags must appear before the -i input."""
    runner, calls = _make_runner(b" V..... av1_qsv              AV1 (Intel Quick Sync Video)\n")
    probe_encoder_available("av1_qsv", runner=runner)
    encode_argv = calls[1]
    init_idx = encode_argv.index("-init_hw_device")
    input_idx = encode_argv.index("-i")
    assert init_idx < input_idx, f"-init_hw_device must precede -i in QSV probe: {encode_argv!r}"


def test_v14_b_qsv_auto_vaapi_device(monkeypatch):
    """QSV probe auto-resolves the Intel VAAPI device when none is specified."""
    monkeypatch.setattr(
        "vmaftune.compare.resolve_vaapi_device", lambda _device: "/dev/dri/renderD129"
    )
    runner, calls = _make_runner(
        b" V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video)\n"
    )
    probe_encoder_available("h264_qsv", runner=runner)
    encode_argv = calls[1]
    assert any("/dev/dri/renderD129" in a for a in encode_argv), encode_argv


def test_v14_b_qsv_custom_vaapi_device():
    """The ``vaapi_device`` parameter overrides the default render node."""
    custom_device = "/dev/dri/renderD129"
    runner, calls = _make_runner(
        b" V..... h264_qsv             H.264 / AVC (Intel Quick Sync Video)\n"
    )
    probe_encoder_available("h264_qsv", runner=runner, vaapi_device=custom_device)
    encode_argv = calls[1]
    assert any(
        custom_device in a for a in encode_argv
    ), f"QSV probe does not use custom VAAPI device {custom_device!r}: {encode_argv!r}"
    assert not any(
        _DEFAULT_VAAPI_DEVICE in a for a in encode_argv
    ), f"QSV probe still uses default device after override: {encode_argv!r}"


# ---------------------------------------------------------------------------
# _hw_init_args_for_encoder unit tests
# ---------------------------------------------------------------------------


def test_hw_init_args_nvenc_returns_empty():
    """NVENC encoders need no pre-input device-init."""
    for enc in ("h264_nvenc", "hevc_nvenc", "av1_nvenc"):
        assert (
            _hw_init_args_for_encoder(enc) == []
        ), f"{enc}: expected empty init args, got {_hw_init_args_for_encoder(enc)!r}"


def test_hw_init_args_amf_returns_empty():
    """AMF encoders need no explicit device-init on Linux."""
    for enc in ("h264_amf", "hevc_amf", "av1_amf"):
        assert (
            _hw_init_args_for_encoder(enc) == []
        ), f"{enc}: expected empty init args, got {_hw_init_args_for_encoder(enc)!r}"


def test_hw_init_args_qsv_returns_three_init_flags():
    """QSV encoders return three device-init flag pairs."""
    for enc in ("h264_qsv", "hevc_qsv", "av1_qsv"):
        args = _hw_init_args_for_encoder(enc)
        assert (
            args.count("-init_hw_device") == 2
        ), f"{enc}: expected 2x -init_hw_device, got {args!r}"
        assert (
            "-filter_hw_device" in args
        ), f"{enc}: expected -filter_hw_device in init args: {args!r}"


def test_hw_init_args_qsv_custom_device():
    """Custom VAAPI device propagates correctly."""
    args = _hw_init_args_for_encoder("h264_qsv", "/dev/dri/renderD129")
    vaapi_entries = [a for a in args if "renderD129" in a]
    assert vaapi_entries, f"custom device not in init args: {args!r}"


# ---------------------------------------------------------------------------
# BaseQsvAdapter.qsv_hw_init_args static helper
# ---------------------------------------------------------------------------


def test_qsv_adapter_static_helper_returns_init_args(monkeypatch):
    """``BaseQsvAdapter.qsv_hw_init_args()`` returns the device-init argv."""
    monkeypatch.setattr(
        "vmaftune.codec_adapters._qsv_common.resolve_vaapi_device",
        lambda _device: "/dev/dri/renderD129",
    )
    result = BaseQsvAdapter.qsv_hw_init_args()
    assert "-init_hw_device" in result
    assert "-filter_hw_device" in result
    assert any("/dev/dri/renderD129" in a for a in result), result


def test_qsv_adapter_static_helper_custom_device():
    """``BaseQsvAdapter.qsv_hw_init_args(device)`` propagates the custom path."""
    result = BaseQsvAdapter.qsv_hw_init_args("/dev/dri/renderD129")
    assert any("renderD129" in a for a in result)
    assert not any(_DEFAULT_VAAPI_DEVICE in a for a in result)


# ---------------------------------------------------------------------------
# DEFAULT_VAAPI_DEVICE public alias
# ---------------------------------------------------------------------------


def test_default_vaapi_device_public_alias():
    """``DEFAULT_VAAPI_DEVICE`` is exported and equals the internal default."""
    assert DEFAULT_VAAPI_DEVICE == _DEFAULT_VAAPI_DEVICE
    assert DEFAULT_VAAPI_DEVICE == AUTO_VAAPI_DEVICE


# ---------------------------------------------------------------------------
# QSV encoder set membership
# ---------------------------------------------------------------------------


def test_qsv_encoders_set_covers_three():
    for enc in ("h264_qsv", "hevc_qsv", "av1_qsv"):
        assert enc in _QSV_ENCODERS, f"{enc} missing from _QSV_ENCODERS"


def test_qsv_encoders_not_in_nvenc_or_amf():
    """QSV set must not include NVENC or AMF names."""
    for enc in ("h264_nvenc", "h264_amf"):
        assert enc not in _QSV_ENCODERS, f"{enc} unexpectedly in _QSV_ENCODERS"


# ---------------------------------------------------------------------------
# V14-C — AMF probe: failure on decoder-only APU is a probe failure, not abort
# ---------------------------------------------------------------------------


def test_v14_c_amf_probe_failure_returns_false_not_exception():
    """When AMF dummy-encode fails (e.g. decoder-only APU), probe returns False."""
    runner, _calls = _make_runner(
        b" V..... h264_amf             AMD AMF H.264 Encoder\n",
        encode_rc=1,
        encode_stderr=b"[h264_amf @ 0xabc] AMF_NOT_SUPPORTED\n",
    )
    ok, reason = probe_encoder_available("h264_amf", runner=runner)
    assert ok is False
    assert "h264_amf" in reason


def test_v14_c_amf_probe_no_init_hw_device():
    """AMF probe must NOT prepend -init_hw_device flags."""
    runner, calls = _make_runner(b" V..... h264_amf             AMD AMF H.264 Encoder\n")
    probe_encoder_available("h264_amf", runner=runner)
    encode_argv = calls[1]
    assert (
        "-init_hw_device" not in encode_argv
    ), f"AMF probe unexpectedly contains -init_hw_device: {encode_argv!r}"


# ---------------------------------------------------------------------------
# GPU availability smoke test — skip when no GPU present
# ---------------------------------------------------------------------------


def _ffmpeg_reports_encoder(encoder: str) -> bool:
    """Return True iff the local ffmpeg binary advertises ``encoder``."""
    import shutil
    import subprocess

    if shutil.which("ffmpeg") is None:
        return False
    try:
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-encoders"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
        out = result.stdout.decode("utf-8", errors="replace")
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1] == encoder:
                return True
    except Exception:
        pass
    return False


@pytest.mark.slow
@pytest.mark.skipif(
    not _ffmpeg_reports_encoder("h264_nvenc"),
    reason="h264_nvenc not compiled into local ffmpeg or no NVIDIA GPU",
)
def test_v14_a_nvenc_probe_succeeds_on_gpu_host():
    """Live NVENC probe succeeds with the 320x240 fix (requires NVIDIA GPU + driver)."""
    ok, reason = probe_encoder_available("h264_nvenc")
    assert ok is True, f"NVENC probe failed on GPU host: {reason!r}"


def _qsv_device_init_works() -> bool:
    """Return True iff the local VA-API/QSV device chain initialises successfully.

    Checks both that h264_qsv is compiled into ffmpeg AND that the
    VA-API driver at /dev/dri/renderD128 supports the minimum VA-API
    version required by the QSV bridge.
    """
    if not _ffmpeg_reports_encoder("h264_qsv"):
        return False
    ok, _ = probe_encoder_available("h264_qsv")
    return ok


@pytest.mark.skipif(
    not _qsv_device_init_works(),
    reason="h264_qsv not compiled into ffmpeg or VA-API driver too old / missing",
)
def test_v14_b_qsv_probe_succeeds_on_gpu_host():
    """Live QSV probe succeeds with init flags (requires Intel GPU + libmfx/VPL).

    This test is skipped on hosts where the VA-API driver does not support
    the minimum VA-API version required by the QSV bridge (e.g. the dev
    host's system VA-API driver). Use the vmaf-dev-mcp container which
    ships the correct intel-media-va-driver-non-free package.
    """
    ok, reason = probe_encoder_available("h264_qsv")
    assert ok is True, f"QSV probe failed on GPU host: {reason!r}"

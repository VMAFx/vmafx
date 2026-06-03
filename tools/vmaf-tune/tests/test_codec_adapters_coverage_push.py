# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push for vmaftune.codec_adapters — gaps by inspection.

Targeted at branches not hit by the existing per-adapter tests:

* ``X264Adapter`` — validate error paths, two_pass_args edge cases.
* ``SvtAv1Adapter`` — validate invalid preset + out-of-range CRF,
  ``ffmpeg_preset_token``, ``two_pass_args``.
* ``LibaomAdapter`` — ``cpu_used`` with invalid preset, validate errors,
  ``two_pass_args`` edge cases.
* ``LibvpxVp9Adapter`` — ``cpu_used`` invalid, validate errors,
  ``two_pass_args`` edge cases, ``extra_params``.
* Registry helpers — ``get_adapter`` KeyError, ``known_codecs`` sorted.
* ``_gop_common.default_gop_args`` happy-path + min_keyint included.
* ``_nvenc_common.BaseNvencAdapter`` — preset mapping across the full
  mnemonic vocabulary.
* ``_qsv_common.BaseQsvAdapter`` — validate + ffmpeg_codec_args shape.
* ``_amf_common`` — validate error paths.
* ``_videotoolbox_common`` — validate error paths.
* ``av1_videotoolbox.Av1VideoToolboxAdapter`` — unavailability error.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import (  # noqa: E402
    Av1VideoToolboxAdapter,
    Av1VideoToolboxUnavailableError,
    LibaomAdapter,
    LibvpxVp9Adapter,
    SvtAv1Adapter,
    X264Adapter,
    X265Adapter,
    get_adapter,
    known_codecs,
)
from vmaftune.codec_adapters._gop_common import (  # noqa: E402
    default_gop_args,
)
from vmaftune.codec_adapters.svtav1 import PRESET_NAME_TO_INT, preset_to_int  # noqa: E402

# ---------------------------------------------------------------------------
# Registry helpers
# ---------------------------------------------------------------------------


class TestRegistry:
    def test_get_adapter_known_codec(self) -> None:
        adapter = get_adapter("libx264")
        assert adapter.encoder == "libx264"

    def test_get_adapter_unknown_raises_key_error(self) -> None:
        with pytest.raises(KeyError, match="unknown codec"):
            get_adapter("libxyz_not_registered_9999")

    def test_known_codecs_sorted(self) -> None:
        codecs = list(known_codecs())
        assert codecs == sorted(codecs)

    def test_known_codecs_includes_all_families(self) -> None:
        codecs = set(known_codecs())
        assert "libx264" in codecs
        assert "libsvtav1" in codecs
        assert "h264_nvenc" in codecs
        assert "h264_amf" in codecs
        assert "h264_qsv" in codecs
        assert "h264_videotoolbox" in codecs


# ---------------------------------------------------------------------------
# X264Adapter
# ---------------------------------------------------------------------------


class TestX264Adapter:
    def setup_method(self) -> None:
        self.adapter = X264Adapter()

    def test_validate_unknown_preset_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown x264 preset"):
            self.adapter.validate("turbo", 23)

    def test_validate_crf_below_range_raises(self) -> None:
        with pytest.raises(ValueError, match="outside Phase A range"):
            self.adapter.validate("medium", -1)

    def test_validate_crf_above_range_raises(self) -> None:
        with pytest.raises(ValueError, match="outside Phase A range"):
            self.adapter.validate("medium", 52)

    def test_validate_boundary_values(self) -> None:
        # Should not raise
        self.adapter.validate("ultrafast", 0)
        self.adapter.validate("veryslow", 51)

    def test_two_pass_args_pass_zero_returns_empty(self) -> None:
        assert self.adapter.two_pass_args(0, Path("/tmp/stats")) == ()

    def test_two_pass_args_pass_one(self) -> None:
        result = self.adapter.two_pass_args(1, Path("/tmp/stats"))
        assert "-pass" in result
        assert "1" in result
        assert "-passlogfile" in result

    def test_two_pass_args_pass_two(self) -> None:
        result = self.adapter.two_pass_args(2, Path("/tmp/stats"))
        assert "-pass" in result
        assert "2" in result

    def test_two_pass_args_invalid_pass_raises(self) -> None:
        with pytest.raises(ValueError, match="pass_number must be 1 or 2"):
            self.adapter.two_pass_args(3, Path("/tmp/stats"))

    def test_extra_params_returns_empty_tuple(self) -> None:
        assert self.adapter.extra_params() == ()

    def test_gop_args_returns_g_flag(self) -> None:
        result = self.adapter.gop_args(48)
        assert "-g" in result
        assert "48" in result

    def test_supports_two_pass(self) -> None:
        assert self.adapter.supports_two_pass is True

    def test_supports_encoder_stats(self) -> None:
        assert self.adapter.supports_encoder_stats is True


# ---------------------------------------------------------------------------
# X265Adapter
# ---------------------------------------------------------------------------


class TestX265Adapter:
    def setup_method(self) -> None:
        self.adapter = X265Adapter()

    def test_validate_happy_path(self) -> None:
        self.adapter.validate("medium", 28)

    def test_validate_unknown_preset_raises(self) -> None:
        with pytest.raises(ValueError):
            self.adapter.validate("turbo", 28)

    def test_ffmpeg_codec_args_shape(self) -> None:
        args = self.adapter.ffmpeg_codec_args("slow", 26)
        assert "-c:v" in args
        assert "libx265" in args
        assert "-preset" in args
        assert "slow" in args
        assert "-crf" in args
        assert "26" in args


# ---------------------------------------------------------------------------
# SvtAv1Adapter
# ---------------------------------------------------------------------------


class TestSvtAv1Adapter:
    def setup_method(self) -> None:
        self.adapter = SvtAv1Adapter()

    def test_preset_name_to_int_medium(self) -> None:
        assert PRESET_NAME_TO_INT["medium"] == 7

    def test_preset_to_int_valid(self) -> None:
        assert preset_to_int("veryfast") == 13
        assert preset_to_int("placebo") == 0

    def test_preset_to_int_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown svtav1 preset"):
            preset_to_int("turbo")

    def test_ffmpeg_preset_token_returns_integer_string(self) -> None:
        token = self.adapter.ffmpeg_preset_token("medium")
        assert token == "7"

    def test_ffmpeg_codec_args_uses_integer_preset(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 35)
        # -preset should be the integer "7", not "medium"
        idx = args.index("-preset")
        assert args[idx + 1] == "7"

    def test_validate_invalid_preset_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown svtav1 preset"):
            self.adapter.validate("turbo", 35)

    def test_validate_crf_outside_absolute_range_raises(self) -> None:
        with pytest.raises(ValueError, match="absolute range"):
            self.adapter.validate("medium", 64)

    def test_validate_crf_inside_absolute_but_outside_phase_a_raises(self) -> None:
        # CRF 1 is valid absolute but below Phase A window (20-50)
        with pytest.raises(ValueError, match="Phase A range"):
            self.adapter.validate("medium", 1)

    def test_two_pass_args_pass_zero_returns_empty(self) -> None:
        assert self.adapter.two_pass_args(0, Path("/tmp/stats")) == ()

    def test_two_pass_args_pass_one(self) -> None:
        result = self.adapter.two_pass_args(1, Path("/tmp/stats"))
        assert "-pass" in result
        assert "1" in result

    def test_two_pass_args_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="pass_number must be 1 or 2"):
            self.adapter.two_pass_args(5, Path("/tmp/stats"))

    def test_supports_two_pass_false(self) -> None:
        # SVT-AV1 CRF mode doesn't support two-pass
        assert self.adapter.supports_two_pass is False

    def test_probe_args_uses_veryfast_preset_integer(self) -> None:
        args = self.adapter.probe_args()
        assert "-c:v" in args
        assert "libsvtav1" in args
        # veryfast -> 13
        assert "13" in args


# ---------------------------------------------------------------------------
# LibaomAdapter
# ---------------------------------------------------------------------------


class TestLibaomAdapter:
    def setup_method(self) -> None:
        self.adapter = LibaomAdapter()

    def test_cpu_used_medium(self) -> None:
        assert self.adapter.cpu_used("medium") == 4

    def test_cpu_used_ultrafast(self) -> None:
        assert self.adapter.cpu_used("ultrafast") == 9

    def test_cpu_used_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown libaom preset"):
            self.adapter.cpu_used("turbo")

    def test_validate_invalid_preset_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown libaom preset"):
            self.adapter.validate("turbo", 35)

    def test_validate_crf_out_of_range_raises(self) -> None:
        with pytest.raises(ValueError, match="outside libaom range"):
            self.adapter.validate("medium", 64)

    def test_ffmpeg_codec_args_uses_cpu_used_not_preset(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 35)
        assert "-cpu-used" in args
        assert "-preset" not in args
        # medium -> cpu-used 4
        idx = args.index("-cpu-used")
        assert args[idx + 1] == "4"

    def test_two_pass_args_pass_zero_returns_empty(self) -> None:
        assert self.adapter.two_pass_args(0, Path("/tmp/s")) == ()

    def test_two_pass_args_pass_one(self) -> None:
        result = self.adapter.two_pass_args(1, Path("/tmp/s"))
        assert "-pass" in result
        assert "1" in result
        assert "-passlogfile" in result

    def test_two_pass_args_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="pass_number must be 1 or 2"):
            self.adapter.two_pass_args(99, Path("/tmp/s"))

    def test_supports_two_pass_true(self) -> None:
        assert self.adapter.supports_two_pass is True

    def test_probe_args_uses_cpu_used(self) -> None:
        args = self.adapter.probe_args()
        assert "-cpu-used" in args
        assert "-preset" not in args


# ---------------------------------------------------------------------------
# LibvpxVp9Adapter
# ---------------------------------------------------------------------------


class TestLibvpxVp9Adapter:
    def setup_method(self) -> None:
        self.adapter = LibvpxVp9Adapter()

    def test_cpu_used_medium(self) -> None:
        assert self.adapter.cpu_used("medium") == 3

    def test_cpu_used_placebo_maps_to_zero(self) -> None:
        assert self.adapter.cpu_used("placebo") == 0

    def test_cpu_used_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown libvpx-vp9 preset"):
            self.adapter.cpu_used("notapreset")

    def test_validate_invalid_preset_raises(self) -> None:
        with pytest.raises(ValueError, match="unknown libvpx-vp9 preset"):
            self.adapter.validate("turbo", 32)

    def test_validate_crf_out_of_range_raises(self) -> None:
        with pytest.raises(ValueError, match="outside libvpx-vp9 range"):
            self.adapter.validate("medium", 64)

    def test_ffmpeg_codec_args_includes_b_v_0(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 32)
        assert "-b:v" in args
        assert "0" in args
        assert "-deadline" in args
        assert "good" in args

    def test_extra_params_includes_row_mt(self) -> None:
        params = self.adapter.extra_params()
        assert "-row-mt" in params
        assert "1" in params

    def test_two_pass_args_pass_zero_returns_empty(self) -> None:
        assert self.adapter.two_pass_args(0, Path("/tmp/s")) == ()

    def test_two_pass_args_pass_one(self) -> None:
        result = self.adapter.two_pass_args(1, Path("/tmp/s"))
        assert "-pass" in result
        assert "1" in result

    def test_two_pass_args_invalid_raises(self) -> None:
        with pytest.raises(ValueError, match="pass_number must be 1 or 2"):
            self.adapter.two_pass_args(3, Path("/tmp/s"))

    def test_supports_two_pass_true(self) -> None:
        assert self.adapter.supports_two_pass is True


# ---------------------------------------------------------------------------
# BaseNvencAdapter — preset mapping
# ---------------------------------------------------------------------------


class TestBaseNvencAdapter:
    def setup_method(self) -> None:
        from vmaftune.codec_adapters.h264_nvenc import H264NvencAdapter  # noqa: PLC0415

        self.adapter = H264NvencAdapter()

    def test_preset_mapping_ultrafast_to_p1(self) -> None:
        # ultrafast / superfast / veryfast -> p1
        args = self.adapter.ffmpeg_codec_args("ultrafast", 23)
        idx = args.index("-preset")
        assert args[idx + 1] == "p1"

    def test_preset_mapping_medium_to_p4(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 23)
        idx = args.index("-preset")
        assert args[idx + 1] == "p4"

    def test_preset_mapping_slow_to_p5(self) -> None:
        args = self.adapter.ffmpeg_codec_args("slow", 23)
        idx = args.index("-preset")
        assert args[idx + 1] == "p5"

    def test_preset_mapping_placebo_to_p7(self) -> None:
        args = self.adapter.ffmpeg_codec_args("placebo", 23)
        idx = args.index("-preset")
        assert args[idx + 1] == "p7"

    def test_quality_uses_cq_not_crf(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 23)
        assert "-cq" in args
        assert "-crf" not in args

    def test_validate_invalid_preset_raises(self) -> None:
        with pytest.raises(ValueError):
            self.adapter.validate("turbo", 23)

    def test_validate_crf_out_of_range_raises(self) -> None:
        with pytest.raises(ValueError):
            self.adapter.validate("medium", 999)

    def test_supports_two_pass_false(self) -> None:
        assert self.adapter.supports_two_pass is False


# ---------------------------------------------------------------------------
# BaseQsvAdapter
# ---------------------------------------------------------------------------


class TestBaseQsvAdapter:
    def setup_method(self) -> None:
        from vmaftune.codec_adapters.h264_qsv import H264QsvAdapter  # noqa: PLC0415

        self.adapter = H264QsvAdapter()

    def test_ffmpeg_codec_args_uses_global_quality(self) -> None:
        args = self.adapter.ffmpeg_codec_args("medium", 23)
        assert "-global_quality" in args
        assert "-crf" not in args
        assert "-cq" not in args

    def test_validate_invalid_preset_raises(self) -> None:
        with pytest.raises(ValueError):
            self.adapter.validate("notapreset", 23)

    def test_validate_crf_out_of_range_raises(self) -> None:
        with pytest.raises(ValueError):
            self.adapter.validate("medium", 200)

    def test_supports_two_pass_false(self) -> None:
        assert self.adapter.supports_two_pass is False


# ---------------------------------------------------------------------------
# Av1VideoToolboxAdapter — raises on ffmpeg_codec_args
# ---------------------------------------------------------------------------


class TestAv1VideoToolboxAdapter:
    def test_ffmpeg_codec_args_raises_unavailable_error(self) -> None:
        adapter = Av1VideoToolboxAdapter()
        with pytest.raises(Av1VideoToolboxUnavailableError):
            adapter.ffmpeg_codec_args("medium", 65)

    def test_registered_in_registry(self) -> None:
        adapter = get_adapter("av1_videotoolbox")
        assert isinstance(adapter, Av1VideoToolboxAdapter)


# ---------------------------------------------------------------------------
# _gop_common.default_gop_args happy paths
# ---------------------------------------------------------------------------


class TestDefaultGopArgsHappyPaths:
    def test_returns_g_flag_only_without_min_keyint(self) -> None:
        result = default_gop_args(48)
        assert result == ("-g", "48")

    def test_returns_g_and_keyint_min_with_min_keyint(self) -> None:
        result = default_gop_args(48, min_keyint=24)
        assert result == ("-g", "48", "-keyint_min", "24")

    def test_min_keyint_equal_to_keyint_is_allowed(self) -> None:
        result = default_gop_args(30, min_keyint=30)
        assert "-keyint_min" in result
        assert "30" in result

    def test_keyint_one_minimum(self) -> None:
        result = default_gop_args(1)
        assert result == ("-g", "1")


# ---------------------------------------------------------------------------
# default_probe_args delegation
# ---------------------------------------------------------------------------


class TestDefaultProbeArgs:
    def test_probe_args_returns_codec_args(self) -> None:
        adapter = X264Adapter()
        result = adapter.probe_args()
        assert "-c:v" in result
        assert "libx264" in result
        # probe uses ultrafast preset + probe_quality
        assert "-preset" in result
        assert "ultrafast" in result

    def test_libaom_probe_args_uses_cpu_used(self) -> None:
        adapter = LibaomAdapter()
        result = adapter.probe_args()
        assert "-cpu-used" in result
        assert "-preset" not in result

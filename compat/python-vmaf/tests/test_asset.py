# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/asset.py
#
# Covers Asset path generation, ID fields, dimension properties, YUV type handling,
# start/end frame logic, and repr/hash/equality. Does not invoke the vmaf binary
# and requires no video fixtures.
"""Pytest cases for vmaf.core.asset — Asset + NorefAsset."""

from __future__ import annotations

import pytest

from vmaf.core.asset import Asset, NorefAsset

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_REF = "/data/ref.yuv"
_DIS = "/data/dis.yuv"


def _make(
    dataset: str = "ds",
    content_id: int = 0,
    asset_id: int = 0,
    ref_path: str = _REF,
    dis_path: str = _DIS,
    asset_dict: dict | None = None,
) -> Asset:
    if asset_dict is None:
        asset_dict = {"width": 576, "height": 324}
    return Asset(dataset, content_id, asset_id, ref_path, dis_path, asset_dict)


# ---------------------------------------------------------------------------
# Construction & basic field access
# ---------------------------------------------------------------------------


class TestAssetConstruction:
    def test_basic_fields(self):
        a = _make(dataset="myds", content_id=5, asset_id=7)
        assert a.dataset == "myds"
        assert a.content_id == 5
        assert a.asset_id == 7
        assert a.ref_path == _REF
        assert a.dis_path == _DIS

    def test_default_yuv_type(self):
        a = _make()
        assert a.ref_yuv_type == "yuv420p"
        assert a.dis_yuv_type == "yuv420p"

    def test_default_resampling_type(self):
        a = _make()
        assert a.ref_resampling_type == "bicubic"
        assert a.dis_resampling_type == "bicubic"

    def test_groundtruth_none_by_default(self):
        a = _make()
        assert a.groundtruth is None

    def test_groundtruth_from_dict(self):
        a = _make(asset_dict={"width": 576, "height": 324, "groundtruth": 85.0})
        assert a.groundtruth == pytest.approx(85.0)

    def test_groundtruth_std(self):
        a = _make(asset_dict={"width": 576, "height": 324, "groundtruth_std": 2.5})
        assert a.groundtruth_std == pytest.approx(2.5)

    def test_raw_groundtruth(self):
        a = _make(asset_dict={"width": 576, "height": 324, "raw_groundtruth": [80, 90]})
        assert a.raw_groundtruth == [80, 90]

    def test_fps_property(self):
        a = _make(asset_dict={"width": 576, "height": 324, "fps": 24.0})
        assert a.fps == pytest.approx(24.0)

    def test_fps_none_when_absent(self):
        a = _make()
        assert a.fps is None

    def test_fps_must_be_positive(self):
        with pytest.raises(AssertionError):
            _make(asset_dict={"width": 576, "height": 324, "fps": -1.0})

    def test_rebuf_indices(self):
        a = _make(asset_dict={"width": 576, "height": 324, "rebuf_indices": [5, 10]})
        assert a.rebuf_indices == [5, 10]

    def test_rebuf_indices_none_when_absent(self):
        a = _make()
        assert a.rebuf_indices is None

    def test_rebuf_negative_index_raises(self):
        with pytest.raises(AssertionError):
            _make(asset_dict={"width": 576, "height": 324, "rebuf_indices": [-1, 5]})


# ---------------------------------------------------------------------------
# YUV type handling
# ---------------------------------------------------------------------------


class TestAssetYuvType:
    def test_shared_yuv_type(self):
        a = _make(asset_dict={"width": 576, "height": 324, "yuv_type": "yuv444p"})
        assert a.ref_yuv_type == "yuv444p"
        assert a.dis_yuv_type == "yuv444p"

    def test_separate_ref_dis_yuv_types(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "ref_yuv_type": "yuv420p10le",
                "dis_yuv_type": "yuv444p",
            }
        )
        assert a.ref_yuv_type == "yuv420p10le"
        assert a.dis_yuv_type == "yuv444p"

    def test_notyuv_ref(self):
        # ref_yuv_type notyuv requires that ref_width_height resolves to None.
        # Use dis-specific keys so ref's shared-key lookup finds nothing.
        a = Asset(
            "ds",
            0,
            0,
            _REF,
            _DIS,
            {"ref_yuv_type": "notyuv", "dis_width": 576, "dis_height": 324},
        )
        assert a.ref_yuv_type == "notyuv"
        assert a.ref_width_height is None

    def test_notyuv_ref_must_not_set_ref_width_height(self):
        """notyuv ref + explicit ref_width rejects because _assert fires."""
        with pytest.raises(AssertionError):
            Asset(
                "ds",
                0,
                0,
                _REF,
                _DIS,
                {
                    "ref_yuv_type": "notyuv",
                    "ref_width": 1920,
                    "ref_height": 1080,
                    "dis_width": 576,
                    "dis_height": 324,
                },
            )

    def test_unsupported_yuv_type_raises(self):
        with pytest.raises(AssertionError):
            _make(asset_dict={"width": 576, "height": 324, "yuv_type": "yuv999p"})

    def test_all_supported_yuv_types_accepted(self):
        for yuv in Asset.SUPPORTED_YUV_TYPES:
            if yuv == "notyuv":
                # notyuv requires omitting width/height
                Asset("ds", 0, 0, _REF, _DIS, {"yuv_type": yuv})
            else:
                Asset("ds", 0, 0, _REF, _DIS, {"width": 576, "height": 324, "yuv_type": yuv})

    def test_workfile_yuv_type_default(self):
        a = _make()
        assert a.workfile_yuv_type == "yuv420p"

    def test_workfile_yuv_type_override(self):
        a = _make(asset_dict={"width": 576, "height": 324, "workfile_yuv_type": "yuv444p"})
        assert a.workfile_yuv_type == "yuv444p"

    def test_clear_up_yuv_type(self):
        a = _make(asset_dict={"width": 576, "height": 324, "yuv_type": "yuv444p"})
        a.clear_up_yuv_type()
        assert a.ref_yuv_type == "yuv420p"


# ---------------------------------------------------------------------------
# Width / height / quality dimensions
# ---------------------------------------------------------------------------


class TestAssetDimensions:
    def test_shared_width_height(self):
        a = _make(asset_dict={"width": 1920, "height": 1080})
        assert a.ref_width_height == (1920, 1080)
        assert a.dis_width_height == (1920, 1080)
        assert a.quality_width_height == (1920, 1080)

    def test_separate_ref_dis_dimensions(self):
        a = _make(
            asset_dict={
                "ref_width": 1920,
                "ref_height": 1080,
                "dis_width": 1280,
                "dis_height": 720,
                "quality_width": 1920,
                "quality_height": 1080,
            }
        )
        assert a.ref_width_height == (1920, 1080)
        assert a.dis_width_height == (1280, 720)
        assert a.quality_width_height == (1920, 1080)

    def test_dimensions_none_when_absent(self):
        a = Asset("ds", 0, 0, _REF, _DIS, {"yuv_type": "notyuv"})
        assert a.ref_width_height is None
        assert a.dis_width_height is None

    def test_clear_up_width_height(self):
        a = _make(asset_dict={"width": 576, "height": 324})
        a.clear_up_width_height()
        assert a.ref_width_height is None

    def test_dis_encode_width_height_fallback(self):
        a = _make()
        assert a.dis_encode_width_height == a.dis_width_height

    def test_dis_encode_width_height_override(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "dis_enc_width": 1920,
                "dis_enc_height": 1080,
            }
        )
        assert a.dis_encode_width_height == (1920, 1080)

    def test_dis_encode_bitdepth_default_8bit(self):
        a = _make()
        assert a.dis_encode_bitdepth == 8

    def test_dis_encode_bitdepth_override_10(self):
        a = _make(asset_dict={"width": 576, "height": 324, "dis_enc_bitdepth": 10})
        assert a.dis_encode_bitdepth == 10

    def test_dis_encode_bitdepth_invalid_raises(self):
        with pytest.raises(AssertionError):
            a = _make(asset_dict={"width": 576, "height": 324, "dis_enc_bitdepth": 99})
            _ = a.dis_encode_bitdepth


# ---------------------------------------------------------------------------
# Start/end frame and duration
# ---------------------------------------------------------------------------


class TestAssetFrameRange:
    def test_explicit_start_end_frame(self):
        a = _make(asset_dict={"width": 576, "height": 324, "start_frame": 10, "end_frame": 49})
        assert a.ref_start_end_frame == (10, 49)
        assert a.dis_start_end_frame == (10, 49)

    def test_no_frame_range_returns_none(self):
        a = _make()
        assert a.ref_start_end_frame is None
        assert a.dis_start_end_frame is None

    def test_start_end_sec_with_fps(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "start_sec": 0.0,
                "end_sec": 2.0,
                "fps": 25.0,
            }
        )
        start, end = a.ref_start_end_frame
        assert start == 0
        assert end == 49  # 2.0*25 - 1

    def test_duration_sec_with_fps(self):
        a = _make(asset_dict={"width": 576, "height": 324, "duration_sec": 4.0, "fps": 25.0})
        start, end = a.ref_start_end_frame
        assert start == 0
        assert end == 99  # 4.0*25 - 1

    def test_separate_ref_dis_start_end(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "ref_start_frame": 0,
                "ref_end_frame": 9,
                "dis_start_frame": 5,
                "dis_end_frame": 14,
            }
        )
        assert a.ref_start_end_frame == (0, 9)
        assert a.dis_start_end_frame == (5, 14)

    def test_ref_duration_sec_from_frames_fps(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "start_frame": 0,
                "end_frame": 24,
                "fps": 25.0,
            }
        )
        # 25 frames / 25fps = 1.0 sec
        assert a.ref_duration_sec == pytest.approx(1.0)

    def test_clear_up_start_end_frame(self):
        a = _make(asset_dict={"width": 576, "height": 324, "start_frame": 0, "end_frame": 9})
        a.clear_up_start_end_frame()
        assert a.ref_start_end_frame is None

    def test_ref_start_sec_none_without_fps(self):
        a = _make(asset_dict={"width": 576, "height": 324, "start_frame": 0, "end_frame": 9})
        assert a.ref_start_sec is None

    def test_ref_start_sec_computed(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "start_frame": 25,
                "end_frame": 49,
                "fps": 25.0,
            }
        )
        assert a.ref_start_sec == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# String representation and hashing
# ---------------------------------------------------------------------------


class TestAssetStringAndHash:
    def test_str_returns_string(self):
        a = _make()
        assert isinstance(str(a), str)

    def test_str_contains_dataset(self):
        a = _make(dataset="testdset")
        assert "testdset" in str(a)

    def test_str_changes_with_yuv_type(self):
        a1 = _make()
        a2 = _make(asset_dict={"width": 576, "height": 324, "yuv_type": "yuv444p"})
        assert str(a1) != str(a2)

    def test_hash_stable(self):
        a = _make()
        h1 = hash(a)
        h2 = hash(a)
        assert h1 == h2

    def test_equality(self):
        a1 = _make()
        a2 = _make()
        assert a1 == a2

    def test_inequality_different_asset_id(self):
        a1 = _make(asset_id=0)
        a2 = _make(asset_id=1)
        assert a1 != a2

    def test_repr_and_from_repr_round_trip(self):
        a = _make(dataset="reptest", content_id=3, asset_id=7)
        rp = repr(a)
        b = Asset.from_repr(rp)
        assert b.dataset == "reptest"
        assert b.content_id == 3
        assert b.asset_id == 7

    def test_to_full_repr_is_string(self):
        a = _make()
        assert isinstance(a.to_full_repr(), str)

    def test_long_string_hashed_to_fixed_length(self):
        # Construct an asset whose string repr is guaranteed to exceed 196 chars.
        # Use very long ref_path and dis_path names.
        long_ref = "/very/long/path/" + "x" * 200 + "/ref.yuv"
        long_dis = "/very/long/path/" + "y" * 200 + "/dis.yuv"
        a = Asset("ds", 0, 0, long_ref, long_dis, {"width": 576, "height": 324})
        s = str(a)
        # SHA-1 hex digest is 40 chars; allow a little slack for edge cases.
        assert len(s) <= 200


# ---------------------------------------------------------------------------
# Workfile and procfile paths
# ---------------------------------------------------------------------------


class TestAssetWorkfilePaths:
    def test_workfile_path_uses_workdir(self):
        a = _make()
        ref_wp = a.ref_workfile_path
        dis_wp = a.dis_workfile_path
        assert a.workdir in ref_wp
        assert a.workdir in dis_wp

    def test_use_path_as_workpath(self):
        a = _make(asset_dict={"width": 576, "height": 324, "use_path_as_workpath": 1})
        assert a.use_path_as_workpath is True
        assert a.ref_workfile_path == _REF
        assert a.dis_workfile_path == _DIS

    def test_use_path_as_workpath_setter(self):
        a = _make()
        a.use_path_as_workpath = True
        assert a.use_path_as_workpath is True
        a.use_path_as_workpath = False
        assert a.use_path_as_workpath is False

    def test_use_workpath_as_procpath(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "use_path_as_workpath": 1,
                "use_workpath_as_procpath": 1,
            }
        )
        assert a.ref_procfile_path == a.ref_workfile_path
        assert a.dis_procfile_path == a.dis_workfile_path


# ---------------------------------------------------------------------------
# Filter command accessors
# ---------------------------------------------------------------------------


class TestAssetFilterCommands:
    def test_crop_cmd_none_when_absent(self):
        a = _make()
        assert a.crop_cmd is None

    def test_crop_cmd_from_dict(self):
        a = _make(asset_dict={"width": 576, "height": 324, "crop_cmd": "iw:ih:0:0"})
        assert a.crop_cmd == "iw:ih:0:0"

    def test_ref_crop_cmd_fallback_to_generic(self):
        a = _make(asset_dict={"width": 576, "height": 324, "crop_cmd": "iw:ih:0:0"})
        assert a.ref_crop_cmd == "iw:ih:0:0"

    def test_ref_specific_crop_overrides_generic(self):
        a = _make(
            asset_dict={
                "width": 576,
                "height": 324,
                "crop_cmd": "generic",
                "ref_crop_cmd": "ref_specific",
            }
        )
        assert a.ref_crop_cmd == "ref_specific"
        assert a.dis_crop_cmd == "generic"

    def test_invalid_filter_key_raises(self):
        a = _make()
        with pytest.raises(AssertionError):
            a.get_filter_cmd("not_a_filter")

    def test_invalid_target_raises(self):
        a = _make()
        with pytest.raises(AssertionError):
            a.get_filter_cmd("crop", "invalid_target")

    def test_pad_fps_cmd_none(self):
        a = _make()
        assert a.pad_cmd is None
        assert a.fps_cmd is None


# ---------------------------------------------------------------------------
# copy() method
# ---------------------------------------------------------------------------


class TestAssetCopy:
    def test_copy_identical(self):
        a = _make(dataset="orig", content_id=1)
        b = a.copy()
        assert b == a

    def test_copy_with_override(self):
        a = _make(dataset="orig")
        b = a.copy(dataset="new")
        assert b.dataset == "new"
        assert b.asset_id == a.asset_id

    def test_copy_strips_use_path_as_workpath(self):
        a = _make(asset_dict={"width": 576, "height": 324, "use_path_as_workpath": 1})
        b = a.copy()
        assert b.use_path_as_workpath is False

    def test_copy_with_extra_asset_dict_entries(self):
        a = _make()
        b = a.copy(asset_dict={"groundtruth": 99.0})
        assert b.groundtruth == pytest.approx(99.0)
        assert a.groundtruth is None  # original unchanged


# ---------------------------------------------------------------------------
# NorefAsset
# ---------------------------------------------------------------------------


class TestNorefAsset:
    def _make_noref(self, asset_dict=None) -> NorefAsset:
        if asset_dict is None:
            asset_dict = {"width": 576, "height": 324}
        return NorefAsset("nrds", 0, 0, _DIS, asset_dict)

    def test_ref_equals_dis_path(self):
        a = self._make_noref()
        assert a.ref_path == a.dis_path == _DIS

    def test_ref_width_height_mirrors_dis(self):
        a = self._make_noref({"width": 1920, "height": 1080})
        assert a.ref_width_height == a.dis_width_height

    def test_ref_yuv_type_mirrors_dis(self):
        a = self._make_noref({"width": 576, "height": 324, "yuv_type": "yuv444p"})
        assert a.ref_yuv_type == "yuv444p"
        assert a.ref_yuv_type == a.dis_yuv_type

    def test_noref_str_has_no_vs(self):
        a = self._make_noref()
        s = str(a)
        assert "_vs_" not in s

    def test_noref_copy_stays_noref(self):
        a = self._make_noref()
        b = a.copy()
        assert isinstance(b, NorefAsset)

    def test_copy_as_asset_returns_asset_class(self):
        a = self._make_noref()
        b = a.copy_as_asset()
        assert type(b) is Asset

    def test_ref_start_end_frame_mirrors_dis(self):
        a = self._make_noref({"width": 576, "height": 324, "start_frame": 5, "end_frame": 10})
        assert a.ref_start_end_frame == a.dis_start_end_frame == (5, 10)

    def test_ref_duration_sec_mirrors_dis(self):
        a = self._make_noref({"width": 576, "height": 324, "duration_sec": 3.0, "fps": 30.0})
        assert a.ref_duration_sec == a.dis_duration_sec

    def test_dis_start_sec_computed(self):
        a = self._make_noref(
            {"width": 576, "height": 324, "start_frame": 30, "end_frame": 59, "fps": 30.0}
        )
        # 30 frames / 30 fps = 1.0 s
        assert a.dis_start_sec == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# Bitrate helpers (file absent — should return None gracefully)
# ---------------------------------------------------------------------------


class TestAssetBitrate:
    def test_bitrate_returns_none_when_file_missing(self):
        a = _make()
        assert a.ref_bitrate_kbps_for_entire_file is None
        assert a.dis_bitrate_kbps_for_entire_file is None

    def test_bitrate_returns_none_without_duration(self):
        a = _make()
        assert a.ref_bitrate_kbps_for_entire_file is None

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/result_store.py
#
# Covers FileSystemResultStore save/load/delete round-trip and the
# numpy-native-type coercion path. Does not invoke the vmaf binary.
"""Pytest cases for vmaf.core.result_store — FileSystemResultStore."""

from __future__ import annotations

import os

import numpy as np
import pytest

from vmaf.core.asset import Asset
from vmaf.core.result import Result
from vmaf.core.result_store import FileSystemResultStore

# ---------------------------------------------------------------------------
# Fixtures / helpers
# ---------------------------------------------------------------------------


def _make_asset(
    dataset: str = "test",
    content_id: int = 0,
    asset_id: int = 0,
    ref_path: str = "/ref/ref.yuv",
    dis_path: str = "/dis/dis.yuv",
) -> Asset:
    return Asset(
        dataset,
        content_id,
        asset_id,
        ref_path,
        dis_path,
        {"width": 576, "height": 324},
    )


def _make_result(asset: Asset, executor_id: str = "FAKE_V1.0") -> Result:
    rd = {
        "FAKE_feature_vif_scores": [0.9, 0.85, 0.92],
        "FAKE_scores": np.array([75.0, 80.0, 78.0]),
    }
    return Result(asset, executor_id, rd)


@pytest.fixture
def store(tmp_path):
    """FileSystemResultStore backed by a per-test temp directory."""
    return FileSystemResultStore(result_store_dir=str(tmp_path / "result_store"))


# ---------------------------------------------------------------------------
# save / load round-trip
# ---------------------------------------------------------------------------


class TestFileSystemResultStoreRoundTrip:
    def test_save_and_load_returns_result(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        assert loaded is not None

    def test_load_missing_returns_none(self, store):
        asset = _make_asset()
        loaded = store.load(asset, "MISSING_EX_V1.0")
        assert loaded is None

    def test_round_trip_scores_preserved(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        original_scores = list(result.result_dict["FAKE_scores"])
        loaded_scores = list(loaded.result_dict["FAKE_scores"])
        assert len(loaded_scores) == len(original_scores)
        for orig, load in zip(original_scores, loaded_scores):
            assert load == pytest.approx(orig, rel=1e-5)

    def test_round_trip_feature_scores_preserved(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        orig = result.result_dict["FAKE_feature_vif_scores"]
        load = loaded.result_dict["FAKE_feature_vif_scores"]
        for o, l in zip(orig, load):
            assert l == pytest.approx(o, rel=1e-5)

    def test_round_trip_executor_id_preserved(self, store):
        asset = _make_asset()
        result = _make_result(asset, "MY_EX_V2.5")
        store.save(result)
        loaded = store.load(asset, "MY_EX_V2.5")
        assert loaded.executor_id == "MY_EX_V2.5"

    def test_round_trip_dataset_preserved(self, store):
        asset = _make_asset(dataset="myset")
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        assert loaded.asset.dataset == "myset"

    def test_round_trip_content_id_preserved(self, store):
        asset = _make_asset(content_id=42)
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        assert loaded.asset.content_id == 42

    def test_round_trip_asset_id_preserved(self, store):
        asset = _make_asset(asset_id=7)
        result = _make_result(asset)
        store.save(result)
        loaded = store.load(asset, "FAKE_V1.0")
        assert loaded.asset.asset_id == 7

    def test_overwrite_saves_new_result(self, store):
        asset = _make_asset()
        r1 = Result(asset, "FAKE_V1.0", {"FAKE_scores": np.array([50.0, 60.0, 70.0])})
        r2 = Result(asset, "FAKE_V1.0", {"FAKE_scores": np.array([80.0, 90.0, 95.0])})
        store.save(r1)
        store.save(r2)
        loaded = store.load(asset, "FAKE_V1.0")
        assert list(loaded.result_dict["FAKE_scores"]) == pytest.approx([80.0, 90.0, 95.0])

    def test_different_assets_independent(self, store):
        a1 = _make_asset(asset_id=1)
        a2 = _make_asset(asset_id=2)
        r1 = Result(a1, "EX_V1", {"FAKE_scores": np.array([10.0])})
        r2 = Result(a2, "EX_V1", {"FAKE_scores": np.array([20.0])})
        store.save(r1)
        store.save(r2)
        l1 = store.load(a1, "EX_V1")
        l2 = store.load(a2, "EX_V1")
        assert list(l1.result_dict["FAKE_scores"]) == pytest.approx([10.0])
        assert list(l2.result_dict["FAKE_scores"]) == pytest.approx([20.0])

    def test_different_executors_independent(self, store):
        asset = _make_asset()
        r1 = Result(asset, "EX_V1", {"FAKE_scores": np.array([10.0])})
        r2 = Result(asset, "EX_V2", {"FAKE_scores": np.array([20.0])})
        store.save(r1)
        store.save(r2)
        l1 = store.load(asset, "EX_V1")
        l2 = store.load(asset, "EX_V2")
        assert list(l1.result_dict["FAKE_scores"]) == pytest.approx([10.0])
        assert list(l2.result_dict["FAKE_scores"]) == pytest.approx([20.0])


# ---------------------------------------------------------------------------
# delete
# ---------------------------------------------------------------------------


class TestFileSystemResultStoreDelete:
    def test_delete_removes_result(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        assert store.load(asset, "FAKE_V1.0") is not None
        store.delete(asset, "FAKE_V1.0")
        assert store.load(asset, "FAKE_V1.0") is None

    def test_delete_nonexistent_is_noop(self, store):
        asset = _make_asset()
        # Should not raise when the file does not exist.
        store.delete(asset, "NONEXISTENT_EX_V1.0")

    def test_clean_up_removes_all(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        store.clean_up()
        assert store.load(asset, "FAKE_V1.0") is None


# ---------------------------------------------------------------------------
# Workfile helpers
# ---------------------------------------------------------------------------


class TestFileSystemResultStoreWorkfile:
    def test_save_workfile_creates_file(self, store, tmp_path):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        # Create a dummy workfile to copy.
        workfile = tmp_path / "dummy.raw"
        workfile.write_bytes(b"\x00\x01\x02")
        store.save_workfile(result, str(workfile), ".raw")
        assert store.has_workfile(asset, "FAKE_V1.0", ".raw")

    def test_has_workfile_false_when_absent(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        assert not store.has_workfile(asset, "FAKE_V1.0", ".raw")

    def test_delete_workfile_removes_it(self, store, tmp_path):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        workfile = tmp_path / "dummy.raw"
        workfile.write_bytes(b"\x00\x01\x02")
        store.save_workfile(result, str(workfile), ".raw")
        assert store.has_workfile(asset, "FAKE_V1.0", ".raw")
        store.delete_workfile(asset, "FAKE_V1.0", ".raw")
        assert not store.has_workfile(asset, "FAKE_V1.0", ".raw")

    def test_delete_workfile_nonexistent_is_noop(self, store):
        asset = _make_asset()
        result = _make_result(asset)
        store.save(result)
        # Must not raise even if workfile is absent.
        store.delete_workfile(asset, "FAKE_V1.0", ".raw")


# ---------------------------------------------------------------------------
# _to_python_natives — numpy scalar coercion
# ---------------------------------------------------------------------------


class TestToPythonNatives:
    """The static helper coerces numpy scalars so ast.literal_eval survives them."""

    _fn = staticmethod(FileSystemResultStore._to_python_natives)

    def test_numpy_float64_coerced(self):
        out = self._fn(np.float64(3.14))
        assert isinstance(out, float)
        assert out == pytest.approx(3.14)

    def test_numpy_int64_coerced(self):
        out = self._fn(np.int64(42))
        assert isinstance(out, int)
        assert out == 42

    def test_numpy_array_coerced_to_list(self):
        out = self._fn(np.array([1.0, 2.0, 3.0]))
        assert isinstance(out, list)
        assert all(isinstance(x, float) for x in out)

    def test_plain_float_passes_through(self):
        out = self._fn(3.14)
        assert out == pytest.approx(3.14)

    def test_plain_int_passes_through(self):
        out = self._fn(7)
        assert out == 7

    def test_string_passes_through(self):
        out = self._fn("hello")
        assert out == "hello"

    def test_none_passes_through(self):
        out = self._fn(None)
        assert out is None

    def test_bool_passes_through(self):
        out = self._fn(True)
        assert out is True

    def test_nested_dict_coerced(self):
        d = {"a": np.float64(1.5), "b": [np.int64(2), np.int64(3)]}
        out = self._fn(d)
        assert isinstance(out["a"], float)
        assert all(isinstance(x, int) for x in out["b"])

    def test_nested_list_of_arrays(self):
        inner = [np.array([1.0, 2.0]), np.array([3.0, 4.0])]
        out = self._fn(inner)
        assert isinstance(out, list)
        for item in out:
            assert isinstance(item, list)


# ---------------------------------------------------------------------------
# Static save_result / load_result without a store instance
# ---------------------------------------------------------------------------


class TestStaticSaveLoadResult:
    def test_save_result_is_readable(self, tmp_path):
        asset = _make_asset()
        result = _make_result(asset)
        path = str(tmp_path / "result.txt")
        FileSystemResultStore.save_result(result, path)
        assert os.path.isfile(path)

    def test_load_result_returns_result_object(self, tmp_path):
        asset = _make_asset()
        result = _make_result(asset)
        path = str(tmp_path / "result.txt")
        FileSystemResultStore.save_result(result, path)
        loaded = FileSystemResultStore.load_result(path)
        assert isinstance(loaded, Result)

    def test_static_round_trip_executor_id(self, tmp_path):
        asset = _make_asset()
        result = _make_result(asset, executor_id="STATIC_EX_V3")
        path = str(tmp_path / "result.txt")
        FileSystemResultStore.save_result(result, path)
        loaded = FileSystemResultStore.load_result(path)
        assert loaded.executor_id == "STATIC_EX_V3"

    def test_static_round_trip_scores(self, tmp_path):
        asset = _make_asset()
        result = _make_result(asset)
        path = str(tmp_path / "result.txt")
        FileSystemResultStore.save_result(result, path)
        loaded = FileSystemResultStore.load_result(path)
        orig = list(result.result_dict["FAKE_scores"])
        load = list(loaded.result_dict["FAKE_scores"])
        for o, l in zip(orig, load):
            assert l == pytest.approx(o, rel=1e-5)

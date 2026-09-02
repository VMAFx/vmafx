# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage gap-fill tests for ai/src modules.

Exercises error paths and branches missed by the primary test suite:
  - aiutils.file_utils — write_text_atomic error path (cleanup on exception)
  - aiutils.__init__ — lazy-load path and AttributeError on unknown names
  - vmaf_train.tune — _read_best_metric all-NaN branch
  - vmaf_train.registry — _sanitize_nonfinite, dumps_registry_json,
    write_registry_json, _hash_file
  - vmaf_train.data.frame_dataset — FrameMOSDataset + PairedFrameDataset
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pandas as pd
import pytest
import torch

# ---------------------------------------------------------------------------
# aiutils.file_utils — error-path branch in write_text_atomic (lines 34-36)
# ---------------------------------------------------------------------------


def test_write_text_atomic_cleans_up_on_write_error(tmp_path: Path) -> None:
    """If writing raises, the temp file is removed and the error re-raised."""
    from aiutils.file_utils import write_text_atomic

    dest = tmp_path / "out.txt"

    # Patch os.replace to raise so we hit the except branch.
    with (
        patch("aiutils.file_utils.os.replace", side_effect=OSError("replace failed")),
        pytest.raises(OSError, match="replace failed"),
    ):
        write_text_atomic(dest, "hello")

    # The destination must not exist (we never completed the rename).
    assert not dest.exists()


def test_write_text_atomic_succeeds(tmp_path: Path) -> None:
    """Happy path: file content is correct after an atomic write."""
    from aiutils.file_utils import write_text_atomic

    dest = tmp_path / "out.txt"
    write_text_atomic(dest, "content")
    assert dest.read_text(encoding="utf-8") == "content"


# ---------------------------------------------------------------------------
# aiutils.__init__ — lazy-load path and AttributeError
# ---------------------------------------------------------------------------


def test_aiutils_init_lazy_load_parquet_utils() -> None:
    """Verify that parquet helpers exposed via __getattr__ are callable."""
    import aiutils

    # These attrs should resolve through __getattr__ to parquet_utils.
    assert callable(aiutils.apply_standard_column_order)
    assert callable(aiutils.detect_schema_version)
    assert callable(aiutils.read_parquet_with_schema)
    assert callable(aiutils.write_parquet_atomic)


def test_aiutils_init_getattr_raises_for_unknown() -> None:
    """__getattr__ re-raises AttributeError for non-parquet unknown names."""
    import aiutils

    with pytest.raises(AttributeError, match="no attribute"):
        _ = aiutils.nonexistent_function_xyz  # type: ignore[attr-defined]


# ---------------------------------------------------------------------------
# vmaf_train.tune — _read_best_metric all-NaN branch
# ---------------------------------------------------------------------------


def test_read_best_metric_returns_inf_for_all_nan() -> None:
    """All-NaN column → float('inf') with a warning (ADR-0963)."""
    from vmaf_train.tune import _read_best_metric

    df = pd.DataFrame({"val/mse": [float("nan"), float("nan")]})
    result = _read_best_metric(df, "val/mse")
    assert math.isinf(result) and result > 0


def test_read_best_metric_returns_min_for_valid_values() -> None:
    """Mixed NaN + valid → returns the finite minimum."""
    from vmaf_train.tune import _read_best_metric

    df = pd.DataFrame({"val/mse": [float("nan"), 3.0, 1.5, float("nan")]})
    result = _read_best_metric(df, "val/mse")
    assert result == pytest.approx(1.5)


def test_read_best_metric_returns_inf_for_empty_column() -> None:
    """Empty column (after dropna) → float('inf')."""
    from vmaf_train.tune import _read_best_metric

    df = pd.DataFrame({"val/mse": pd.Series([], dtype=float)})
    result = _read_best_metric(df, "val/mse")
    assert math.isinf(result) and result > 0


# ---------------------------------------------------------------------------
# vmaf_train.registry — _sanitize_nonfinite, dumps/write helpers
# ---------------------------------------------------------------------------


def test_sanitize_nonfinite_replaces_nan_with_none() -> None:
    from vmaf_train.registry import _sanitize_nonfinite

    obj = {"score": float("nan"), "mean": 0.5}
    result = _sanitize_nonfinite(obj)
    assert result["score"] is None
    assert result["mean"] == pytest.approx(0.5)


def test_sanitize_nonfinite_replaces_inf_with_none() -> None:
    from vmaf_train.registry import _sanitize_nonfinite

    obj = {"a": float("inf"), "b": float("-inf"), "c": 1.0}
    result = _sanitize_nonfinite(obj)
    assert result["a"] is None
    assert result["b"] is None
    assert result["c"] == pytest.approx(1.0)


def test_sanitize_nonfinite_recurses_into_lists_and_dicts() -> None:
    from vmaf_train.registry import _sanitize_nonfinite

    obj = {"nested": [float("nan"), {"inner": float("inf"), "ok": 2.0}]}
    result = _sanitize_nonfinite(obj)
    assert result["nested"][0] is None
    assert result["nested"][1]["inner"] is None
    assert result["nested"][1]["ok"] == pytest.approx(2.0)


def test_sanitize_nonfinite_passthrough_non_float() -> None:
    from vmaf_train.registry import _sanitize_nonfinite

    obj = {"s": "hello", "i": 42, "n": None}
    assert _sanitize_nonfinite(obj) == {"s": "hello", "i": 42, "n": None}


def test_dumps_registry_json_serialises_nonfinite_as_null() -> None:
    from vmaf_train.registry import dumps_registry_json

    payload = {"score": float("nan"), "label": "test"}
    text = dumps_registry_json(payload)
    parsed = json.loads(text)
    assert parsed["score"] is None
    assert parsed["label"] == "test"


def test_dumps_registry_json_produces_sorted_indented_output() -> None:
    from vmaf_train.registry import dumps_registry_json

    payload = {"z": 1, "a": 2}
    text = dumps_registry_json(payload)
    # Should be sorted: "a" before "z"
    assert text.index('"a"') < text.index('"z"')
    # Should be pretty-printed (contains newlines)
    assert "\n" in text


def test_write_registry_json_writes_with_trailing_newline(tmp_path: Path) -> None:
    from vmaf_train.registry import write_registry_json

    path = tmp_path / "registry.json"
    write_registry_json(path, {"key": "value"})
    content = path.read_text(encoding="utf-8")
    assert content.endswith("\n")
    parsed = json.loads(content)
    assert parsed["key"] == "value"


def test_hash_file_is_deterministic(tmp_path: Path) -> None:
    from vmaf_train.registry import _hash_file

    f = tmp_path / "data.bin"
    f.write_bytes(b"hello")
    h1 = _hash_file(f)
    h2 = _hash_file(f)
    assert h1 == h2
    assert len(h1) == 64  # SHA-256 hex digest length


def test_hash_file_changes_with_content(tmp_path: Path) -> None:
    from vmaf_train.registry import _hash_file

    f = tmp_path / "data.bin"
    f.write_bytes(b"hello")
    h1 = _hash_file(f)
    f.write_bytes(b"world")
    h2 = _hash_file(f)
    assert h1 != h2


# ---------------------------------------------------------------------------
# vmaf_train.data.frame_dataset — FrameMOSDataset + PairedFrameDataset
# ---------------------------------------------------------------------------


def _write_npy_luma(path: Path, h: int = 16, w: int = 16) -> None:
    """Write a synthetic uint8 luma .npy file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = np.zeros((h, w), dtype=np.uint8)
    np.save(path, arr)


def _write_frame_mos_parquet(tmp: Path, n: int = 3) -> Path:
    rows = []
    for i in range(n):
        p = tmp / f"frame_{i}.npy"
        _write_npy_luma(p)
        rows.append({"key": f"clip_{i}", "frame_path": str(p), "mos": float(60 + i)})
    df = pd.DataFrame(rows)
    out = tmp / "mos.parquet"
    df.to_parquet(out, index=False)
    return out


def _write_paired_parquet(tmp: Path, n: int = 3) -> Path:
    rows = []
    for i in range(n):
        deg = tmp / f"deg_{i}.npy"
        clean = tmp / f"clean_{i}.npy"
        _write_npy_luma(deg)
        _write_npy_luma(clean)
        rows.append({"key": f"clip_{i}", "deg_path": str(deg), "clean_path": str(clean)})
    df = pd.DataFrame(rows)
    out = tmp / "paired.parquet"
    df.to_parquet(out, index=False)
    return out


def test_frame_mos_dataset_len(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    pq = _write_frame_mos_parquet(tmp_path, n=4)
    ds = FrameMOSDataset(pq)
    assert len(ds) == 4


def test_frame_mos_dataset_getitem_returns_correct_types(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    pq = _write_frame_mos_parquet(tmp_path, n=2)
    ds = FrameMOSDataset(pq)
    x, y = ds[0]
    assert isinstance(x, torch.Tensor)
    assert isinstance(y, torch.Tensor)
    # luma frame: (1, H, W) shape with values in [0, 1]
    assert x.shape[0] == 1
    assert x.dtype == torch.float32
    assert y.dtype == torch.float32


def test_frame_mos_dataset_mos_values(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    pq = _write_frame_mos_parquet(tmp_path, n=3)
    ds = FrameMOSDataset(pq)
    for i in range(3):
        _, y = ds[i]
        assert y.item() == pytest.approx(60.0 + i)


def test_frame_mos_dataset_keys_populated(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    pq = _write_frame_mos_parquet(tmp_path, n=2)
    ds = FrameMOSDataset(pq)
    assert ds.keys == ["clip_0", "clip_1"]


def test_frame_mos_dataset_missing_column_raises(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    # Parquet without 'mos' column
    df = pd.DataFrame({"frame_path": ["a.npy"]})
    pq = tmp_path / "bad.parquet"
    df.to_parquet(pq, index=False)
    with pytest.raises(ValueError, match="mos"):
        FrameMOSDataset(pq)


def test_frame_mos_dataset_missing_frame_path_column_raises(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import FrameMOSDataset

    df = pd.DataFrame({"mos": [80.0]})
    pq = tmp_path / "bad.parquet"
    df.to_parquet(pq, index=False)
    with pytest.raises(ValueError, match="frame_path"):
        FrameMOSDataset(pq)


def test_paired_frame_dataset_len(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import PairedFrameDataset

    pq = _write_paired_parquet(tmp_path, n=5)
    ds = PairedFrameDataset(pq)
    assert len(ds) == 5


def test_paired_frame_dataset_getitem_types(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import PairedFrameDataset

    pq = _write_paired_parquet(tmp_path, n=2)
    ds = PairedFrameDataset(pq)
    deg, clean = ds[0]
    assert isinstance(deg, torch.Tensor)
    assert isinstance(clean, torch.Tensor)
    assert deg.shape == clean.shape
    assert deg.shape[0] == 1  # channel dim


def test_paired_frame_dataset_keys(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import PairedFrameDataset

    pq = _write_paired_parquet(tmp_path, n=3)
    ds = PairedFrameDataset(pq)
    assert ds.keys == ["clip_0", "clip_1", "clip_2"]


def test_paired_frame_dataset_missing_column_raises(tmp_path: Path) -> None:
    from vmaf_train.data.frame_dataset import PairedFrameDataset

    df = pd.DataFrame({"deg_path": ["a.npy"]})
    pq = tmp_path / "bad.parquet"
    df.to_parquet(pq, index=False)
    with pytest.raises(ValueError, match="clean_path"):
        PairedFrameDataset(pq)


def test_paired_frame_dataset_no_key_column(tmp_path: Path) -> None:
    """PairedFrameDataset works even when 'key' column is absent."""
    from vmaf_train.data.frame_dataset import PairedFrameDataset

    deg = tmp_path / "deg.npy"
    clean = tmp_path / "clean.npy"
    _write_npy_luma(deg)
    _write_npy_luma(clean)
    df = pd.DataFrame({"deg_path": [str(deg)], "clean_path": [str(clean)]})
    pq = tmp_path / "nokeycolumn.parquet"
    df.to_parquet(pq, index=False)
    ds = PairedFrameDataset(pq)
    assert ds.keys == []
    assert len(ds) == 1

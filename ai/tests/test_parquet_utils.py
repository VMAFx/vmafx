# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for aiutils.parquet_utils.

Covers the ``write_parquet_atomic`` helper end-to-end and the rollback
path on serialisation failure.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from aiutils.parquet_utils import write_parquet_atomic


def test_write_parquet_atomic_round_trips_dataframe(tmp_path: Path) -> None:
    """Atomic write produces a Parquet file readable by pandas."""
    df = pd.DataFrame({"a": [1, 2, 3], "b": ["x", "y", "z"]})
    out = tmp_path / "out.parquet"

    write_parquet_atomic(df, out, index=False)

    assert out.is_file()
    round_tripped = pd.read_parquet(out)
    assert list(round_tripped.columns) == ["a", "b"]
    assert round_tripped["a"].tolist() == [1, 2, 3]
    assert round_tripped["b"].tolist() == ["x", "y", "z"]


def test_write_parquet_atomic_replaces_existing_file(tmp_path: Path) -> None:
    """Second write overwrites prior contents at the target path."""
    out = tmp_path / "existing.parquet"
    pd.DataFrame({"a": [99]}).to_parquet(out, index=False)
    assert out.is_file()

    write_parquet_atomic(pd.DataFrame({"a": [1, 2]}), out, index=False)

    round_tripped = pd.read_parquet(out)
    assert round_tripped["a"].tolist() == [1, 2]


def test_write_parquet_atomic_cleans_up_temp_on_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Serialisation exception leaves no temp/output artefact behind.

    write_parquet_atomic delegates the actual write to _write_v2 (which uses
    pyarrow directly, not df.to_parquet).  Inject the failure via monkeypatch
    so the atomic-rename + cleanup path is exercised.
    """
    import aiutils.parquet_utils as _pqu

    def _boom(*args: object, **kwargs: object) -> None:
        raise RuntimeError("parquet writer exploded")

    monkeypatch.setattr(_pqu, "_write_v2", _boom)

    df = pd.DataFrame({"a": [1]})
    out = tmp_path / "should_not_exist.parquet"

    with pytest.raises(RuntimeError, match="parquet writer exploded"):
        write_parquet_atomic(df, out, index=False)

    # No leftover output file.
    assert not out.exists()
    # No leftover temp file in the output directory.
    assert list(tmp_path.iterdir()) == []

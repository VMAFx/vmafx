# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the parquet schema-v2 contract (ADR-0926)."""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pyarrow.parquet as pq
import pytest

from aiutils.parquet_utils import (
    _VMAFX_PARQUET_SCHEMA,
    DEFAULT_ZSTD_LEVEL,
    PIPELINE_HASH_KEY,
    SCHEMA_VERSION_KEY,
    apply_standard_column_order,
    detect_schema_version,
    read_parquet_with_schema,
    write_parquet_atomic,
)


def _make_frame() -> pd.DataFrame:
    return pd.DataFrame(
        {
            # Deliberately scrambled column order on input so the test
            # exercises the reorder code path.
            "vif_scale0": [0.91, 0.92],
            "mos": [4.1, 4.2],
            "adm2": [0.81, 0.82],
            "source": ["k150k", "k150k"],
            "frame_idx": [0, 1],
            "clip_id": ["clip_a", "clip_a"],
        }
    )


# ---------------------------------------------------------------------------
# Column-order helper
# ---------------------------------------------------------------------------


def test_apply_standard_column_order_canonical_layout() -> None:
    df = _make_frame()

    ordered = apply_standard_column_order(df)

    assert list(ordered.columns) == [
        "clip_id",
        "frame_idx",
        "adm2",  # features sorted alphabetically
        "vif_scale0",
        "mos",  # label block
        "source",  # metadata block
    ]


def test_apply_standard_column_order_preserves_values_unchanged() -> None:
    # Reorder is a column-axis operation only; cell values must round-trip
    # exactly (no row reshuffle, no dtype coercion).
    df = _make_frame()

    ordered = apply_standard_column_order(df)

    for column in df.columns:
        pd.testing.assert_series_equal(
            ordered[column].reset_index(drop=True),
            df[column].reset_index(drop=True),
            check_names=False,
        )


def test_apply_standard_column_order_explicit_labels_override_heuristics() -> None:
    df = pd.DataFrame(
        {
            "adm2": [0.5],
            "custom_target": [4.0],
            "vmaf": [90.0],  # would normally be a label by heuristic
            "frame_idx": [0],
        }
    )

    ordered = apply_standard_column_order(df, labels=["custom_target"])

    # ``vmaf`` falls back to feature since the caller supplied an explicit
    # label list that doesn't contain it.
    assert list(ordered.columns) == [
        "frame_idx",
        "adm2",
        "vmaf",
        "custom_target",
    ]


def test_apply_standard_column_order_empty_frame_is_noop() -> None:
    df = pd.DataFrame()
    assert apply_standard_column_order(df) is df


# ---------------------------------------------------------------------------
# Write path: v2 defaults
# ---------------------------------------------------------------------------


def test_write_parquet_atomic_writes_v2_schema_metadata(tmp_path: Path) -> None:
    out = tmp_path / "features.parquet"

    write_parquet_atomic(_make_frame(), out)

    assert out.is_file()
    assert detect_schema_version(out) == _VMAFX_PARQUET_SCHEMA == 2

    schema = pq.read_schema(out)
    metadata = schema.metadata or {}
    assert metadata.get(SCHEMA_VERSION_KEY) == b"2"
    # Pipeline-hash key is always present; value may be empty when git is
    # unavailable, but it must round-trip without a KeyError.
    assert PIPELINE_HASH_KEY in metadata


def test_write_parquet_atomic_default_compression_is_zstd(tmp_path: Path) -> None:
    out = tmp_path / "features.parquet"
    write_parquet_atomic(_make_frame(), out)

    pf = pq.ParquetFile(out)
    rg = pf.metadata.row_group(0)
    # zstd is reported per-column.  Spot-check every column.
    for col_idx in range(rg.num_columns):
        assert rg.column(col_idx).compression.upper() == "ZSTD"


def test_write_parquet_atomic_persists_canonical_column_order(tmp_path: Path) -> None:
    out = tmp_path / "features.parquet"
    write_parquet_atomic(_make_frame(), out)

    roundtrip = pd.read_parquet(out)
    assert list(roundtrip.columns) == [
        "clip_id",
        "frame_idx",
        "adm2",
        "vif_scale0",
        "mos",
        "source",
    ]


def test_write_parquet_atomic_roundtrip_preserves_values(tmp_path: Path) -> None:
    out = tmp_path / "features.parquet"
    original = _make_frame()

    write_parquet_atomic(original, out)
    roundtrip = pd.read_parquet(out)

    # Reorder original to match canonical layout for comparison.
    expected = apply_standard_column_order(original).reset_index(drop=True)
    pd.testing.assert_frame_equal(
        roundtrip.reset_index(drop=True),
        expected,
        check_like=False,
    )


# ---------------------------------------------------------------------------
# Backward-compat: explicit compression override
# ---------------------------------------------------------------------------


def test_write_parquet_atomic_honours_explicit_snappy_override(tmp_path: Path) -> None:
    out = tmp_path / "snappy.parquet"
    write_parquet_atomic(_make_frame(), out, compression="snappy")

    pf = pq.ParquetFile(out)
    rg = pf.metadata.row_group(0)
    assert rg.column(0).compression.upper() == "SNAPPY"
    # Schema-version metadata still attaches even with non-zstd compression.
    assert detect_schema_version(out) == 2


def test_write_parquet_atomic_honours_explicit_zstd_level(tmp_path: Path) -> None:
    out = tmp_path / "zstd9.parquet"
    write_parquet_atomic(_make_frame(), out, compression="zstd", compression_level=9)
    # We can't introspect the level from the file itself, but the write
    # must succeed and produce a readable v2 file.
    assert detect_schema_version(out) == 2


def test_write_parquet_atomic_rejects_unknown_kwarg(tmp_path: Path) -> None:
    out = tmp_path / "x.parquet"
    with pytest.raises(TypeError, match="unexpected keyword"):
        write_parquet_atomic(_make_frame(), out, bogus=1)


# ---------------------------------------------------------------------------
# Read path: schema-version detection
# ---------------------------------------------------------------------------


def test_detect_schema_version_returns_one_for_legacy_file(tmp_path: Path) -> None:
    # Simulate a v1 file: raw pandas to_parquet with no fork metadata.
    out = tmp_path / "legacy.parquet"
    _make_frame().to_parquet(out, index=False)

    assert detect_schema_version(out) == 1


def test_read_parquet_with_schema_handles_v1_and_v2(tmp_path: Path) -> None:
    v1_path = tmp_path / "v1.parquet"
    v2_path = tmp_path / "v2.parquet"

    _make_frame().to_parquet(v1_path, index=False)
    write_parquet_atomic(_make_frame(), v2_path)

    v1_df, v1_version = read_parquet_with_schema(v1_path)
    v2_df, v2_version = read_parquet_with_schema(v2_path)

    assert v1_version == 1
    assert v2_version == 2
    # Both reads must surface the same set of columns, just in different
    # orders (v1 keeps the original input order, v2 enforces canonical).
    assert set(v1_df.columns) == set(v2_df.columns)


def test_read_parquet_with_schema_forwards_kwargs(tmp_path: Path) -> None:
    out = tmp_path / "features.parquet"
    write_parquet_atomic(_make_frame(), out)

    df, version = read_parquet_with_schema(out, columns=["clip_id", "mos"])

    assert version == 2
    assert list(df.columns) == ["clip_id", "mos"]


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------


def test_default_zstd_level_is_three() -> None:
    # Locked by the ADR; ratcheting it up changes corpus-wide write cost.
    assert DEFAULT_ZSTD_LEVEL == 3

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Parquet file I/O utilities.

This module standardises parquet writes across the AI scripts and tooling so
that downstream consumers can rely on a stable column layout, a compact
on-disk encoding, and an explicit schema version embedded in the file
metadata.

Schema versions
---------------

* **v1** — historic format: pandas/pyarrow defaults, ``snappy`` compression,
  no fork-specific file metadata.  Files of this shape are still produced by
  legacy ad-hoc ``df.to_parquet(...)`` calls in older sweeps.
* **v2** — current format produced by :func:`write_parquet_atomic` when
  invoked with default options (see ADR-0926):

  - Columns reordered to ``[clip_id, frame_idx, ...features (alphabetical),
    ...labels, ...metadata]`` so consumers no longer have to "find the score
    column" by hand.
  - ``compression='zstd'`` at ``compression_level=3`` (~30 % smaller than
    snappy at comparable read/write speed for K150K / CHUG-shape data).
  - File-level pyarrow metadata carries ``vmafx_schema_version=2`` and a
    ``vmafx_pipeline_hash`` git commit pointer.

Backward compatibility
----------------------

:func:`read_parquet_with_schema` reads both v1 and v2 files transparently and
reports the detected version so callers can branch on it when needed.
Callers that pass an explicit ``compression=`` keyword to
:func:`write_parquet_atomic` keep their override; only the *defaults* changed.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

import pandas as pd

# ---------------------------------------------------------------------------
# Module-level constants
# ---------------------------------------------------------------------------

#: Bumped when the on-disk parquet shape changes.  Readers branch on this.
_VMAFX_PARQUET_SCHEMA = 2

#: pyarrow metadata key for the schema version.  Stored as a UTF-8 byte
#: string per the pyarrow custom-metadata contract.
SCHEMA_VERSION_KEY = b"vmafx_schema_version"

#: pyarrow metadata key for the git commit that produced the file.  Empty
#: string when ``git`` is unavailable or the working tree is not a repo.
PIPELINE_HASH_KEY = b"vmafx_pipeline_hash"

#: Default zstd compression level for v2 writes.  Level 3 is the sweet spot
#: between ratio and CPU on K150K-shape data; raise to 9+ only for cold
#: archival of frozen reports.
DEFAULT_ZSTD_LEVEL = 3

# Columns that always appear first (in this order) when present in the
# frame.  Anchoring on a stable prefix makes joins, dedup, and resume-set
# computation cheap because consumers can read just these columns.
_LEADING_COLUMNS: tuple[str, ...] = ("clip_id", "clip_name", "frame_idx", "frame_index")

# Heuristic suffix / name sets used by ``apply_standard_column_order`` when
# the caller does not pass explicit ``labels=`` / ``metadata=`` lists.
_LABEL_NAMES: frozenset[str] = frozenset(
    {
        "mos",
        "mos_z",
        "dmos",
        "score",
        "vmaf",
        "vmaf_score",
        "label",
    }
)
_LABEL_SUFFIXES: tuple[str, ...] = ("_mos", "_dmos", "_label", "_target")
_METADATA_NAMES: frozenset[str] = frozenset(
    {
        "source",
        "split",
        "fold",
        "codec",
        "preset",
        "crf",
        "bitrate",
        "resolution",
        "width",
        "height",
        "fps",
        "duration",
        "manifest_hash",
        "pipeline_hash",
    }
)
_METADATA_SUFFIXES: tuple[str, ...] = ("_manifest", "_meta", "_hash")


# ---------------------------------------------------------------------------
# Column-order helpers
# ---------------------------------------------------------------------------


def _classify(
    column: str,
    labels: Iterable[str] | None,
    metadata: Iterable[str] | None,
) -> str:
    """Return ``"leading" | "label" | "metadata" | "feature"`` for ``column``."""
    if column in _LEADING_COLUMNS:
        return "leading"
    if labels is not None and column in set(labels):
        return "label"
    if metadata is not None and column in set(metadata):
        return "metadata"
    if labels is None and (column in _LABEL_NAMES or column.endswith(_LABEL_SUFFIXES)):
        return "label"
    if metadata is None and (column in _METADATA_NAMES or column.endswith(_METADATA_SUFFIXES)):
        return "metadata"
    return "feature"


def apply_standard_column_order(
    df: pd.DataFrame,
    *,
    labels: Sequence[str] | None = None,
    metadata: Sequence[str] | None = None,
) -> pd.DataFrame:
    """Return ``df`` reordered into the v2 canonical column layout.

    Layout: ``[clip_id, frame_idx, ...features (alphabetical),
    ...labels, ...metadata]``.

    Args:
        df: Input DataFrame.  Returned unchanged (as a view-free copy of
            the column list) if it already matches the canonical order.
        labels: Explicit label column names.  When ``None``, label columns
            are inferred from a small built-in name / suffix allowlist
            (``mos``, ``dmos``, ``score``, ``*_label`` ...).
        metadata: Explicit metadata column names.  When ``None``, metadata
            columns are inferred from a built-in allowlist (``source``,
            ``split``, ``codec`` ...).

    Returns:
        A new DataFrame whose column index follows the canonical order.
        Row data is not modified; this is a column-axis operation only.
    """
    if df.columns.empty:
        return df

    leading: list[str] = []
    features: list[str] = []
    label_cols: list[str] = []
    meta_cols: list[str] = []

    # Preserve the user-supplied order for the explicit ``leading`` block.
    seen = set()
    for name in _LEADING_COLUMNS:
        if name in df.columns and name not in seen:
            leading.append(name)
            seen.add(name)

    for column in df.columns:
        if column in seen:
            continue
        kind = _classify(str(column), labels, metadata)
        if kind == "label":
            label_cols.append(str(column))
        elif kind == "metadata":
            meta_cols.append(str(column))
        else:
            features.append(str(column))

    ordered = (
        leading
        + sorted(features)
        + (list(labels) if labels is not None else sorted(label_cols))
        + (list(metadata) if metadata is not None else sorted(meta_cols))
    )

    # Filter any explicit label/metadata names the caller passed that are
    # not actually present, so we never raise KeyError on reindex.
    ordered = [c for c in ordered if c in df.columns]

    if list(df.columns) == ordered:
        return df
    return df[ordered]


# ---------------------------------------------------------------------------
# Pipeline-hash detection
# ---------------------------------------------------------------------------


def _detect_pipeline_hash() -> str:
    """Best-effort short git commit hash for the current working tree.

    Returns the empty string on any failure (no git, detached worktree,
    network-restricted CI, etc.).  Never raises — the metadata is
    informational, not a hard contract.
    """
    override = os.environ.get("VMAFX_PIPELINE_HASH")
    if override:
        return override.strip()
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def _build_schema_metadata(
    extra: dict[bytes, bytes] | None,
) -> dict[bytes, bytes]:
    """Assemble the pyarrow custom-metadata block for a v2 write."""
    metadata: dict[bytes, bytes] = {
        SCHEMA_VERSION_KEY: str(_VMAFX_PARQUET_SCHEMA).encode("ascii"),
        PIPELINE_HASH_KEY: _detect_pipeline_hash().encode("ascii"),
    }
    if extra:
        metadata.update(extra)
    return metadata


# ---------------------------------------------------------------------------
# Write path
# ---------------------------------------------------------------------------


def _write_v2(
    df: pd.DataFrame,
    target: Path,
    *,
    compression: str,
    compression_level: int | None,
    extra_metadata: dict[bytes, bytes] | None,
    index: bool,
) -> None:
    """Write ``df`` as a v2 parquet (zstd, ordered columns, schema metadata).

    Kept private so callers go through :func:`write_parquet_atomic`, which
    owns the atomic-rename pattern.
    """
    # Imported lazily so ``aiutils`` stays importable without pyarrow on the
    # path (matches the ``__getattr__`` lazy import in __init__.py).
    import pyarrow as pa
    import pyarrow.parquet as pq

    ordered = apply_standard_column_order(df)
    table = pa.Table.from_pandas(ordered, preserve_index=index)
    existing = table.schema.metadata or {}
    merged = {**existing, **_build_schema_metadata(extra_metadata)}
    table = table.replace_schema_metadata(merged)

    write_kwargs: dict[str, Any] = {"compression": compression}
    if compression == "zstd" and compression_level is not None:
        write_kwargs["compression_level"] = compression_level
    pq.write_table(table, str(target), **write_kwargs)


def write_parquet_atomic(
    df: pd.DataFrame,
    output: Path,
    *,
    extra_metadata: dict[bytes, bytes] | None = None,
    **kwargs: Any,
) -> None:
    """Write a DataFrame to Parquet atomically using a temp file.

    Writes to a temporary file in the same directory as the output, then
    renames it to the target path.  On exception, the temp file is cleaned
    up automatically.

    By default this produces a **schema-v2** file (see module docstring):
    zstd-3 compression, canonical column order, file-level metadata that
    embeds ``vmafx_schema_version`` and ``vmafx_pipeline_hash``.

    Args:
        df: DataFrame to write.
        output: Target Parquet file path.
        extra_metadata: Optional additional pyarrow custom-metadata entries.
            Keys and values must be ``bytes``.  Merged with (and overridden
            by) the fork's own ``vmafx_*`` keys.
        **kwargs: Forwarded to the underlying writer.  Recognised keys:

            * ``compression`` (str) — ``"zstd"`` (default), ``"snappy"``,
              ``"gzip"``, ``"brotli"``, ``"lz4"``, or ``"none"``.
            * ``compression_level`` (int) — only honoured when
              ``compression="zstd"``.  Defaults to ``DEFAULT_ZSTD_LEVEL``.
            * ``index`` (bool) — whether to persist the DataFrame index.
              Default ``False`` (most pipelines use range indexes).
            * ``engine`` (str) — accepted for backward compatibility but
              ignored: v2 writes always go through pyarrow.

    Backward compatibility
    ----------------------
    Callers that explicitly pass ``compression="snappy"`` keep snappy
    compression; only the default changed (snappy → zstd-3).  Callers that
    pass ``index=True`` keep the index column.  The v2 metadata block is
    always written regardless of compression choice.
    """
    compression = kwargs.pop("compression", "zstd")
    compression_level = kwargs.pop("compression_level", None)
    if compression == "zstd" and compression_level is None:
        compression_level = DEFAULT_ZSTD_LEVEL
    index = bool(kwargs.pop("index", False))
    # ``engine`` is accepted for source-compat with old callers (pandas
    # idiom) but ignored — pyarrow is the only writer.
    kwargs.pop("engine", None)
    if kwargs:
        unexpected = ", ".join(sorted(kwargs))
        raise TypeError(f"write_parquet_atomic() got unexpected keyword argument(s): {unexpected}")

    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=output.parent,
        delete=False,
    ) as tmp_fh:
        tmp = Path(tmp_fh.name)
    try:
        _write_v2(
            df,
            tmp,
            compression=compression,
            compression_level=compression_level,
            extra_metadata=extra_metadata,
            index=index,
        )
        tmp.replace(output)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise


# ---------------------------------------------------------------------------
# Read path
# ---------------------------------------------------------------------------


def detect_schema_version(path: Path) -> int:
    """Return the parquet schema version stamped in the file's metadata.

    Returns ``1`` when the file carries no ``vmafx_schema_version`` key
    (legacy v1 layout written by raw ``df.to_parquet(...)`` calls).
    """
    import pyarrow.parquet as pq

    schema = pq.read_schema(str(path))
    metadata = schema.metadata or {}
    raw = metadata.get(SCHEMA_VERSION_KEY)
    if raw is None:
        return 1
    try:
        return int(raw.decode("ascii"))
    except (UnicodeDecodeError, ValueError):
        return 1


def read_parquet_with_schema(
    path: Path,
    **kwargs: Any,
) -> tuple[pd.DataFrame, int]:
    """Read a parquet file and return ``(dataframe, schema_version)``.

    Reads both v1 and v2 files transparently.  ``schema_version`` is ``1``
    for legacy files and ``2`` for files written by
    :func:`write_parquet_atomic`.  Additional kwargs are forwarded to
    :func:`pandas.read_parquet`.
    """
    version = detect_schema_version(path)
    df = pd.read_parquet(path, **kwargs)
    return df, version

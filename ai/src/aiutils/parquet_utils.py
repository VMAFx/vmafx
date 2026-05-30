# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Parquet file I/O utilities."""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Any

import pandas as pd


def write_parquet_atomic(df: pd.DataFrame, output: Path, **kwargs: Any) -> None:
    """Write a DataFrame to Parquet atomically using a temp file.

    Writes to a temporary file in the same directory as the output,
    then renames it to the target path. On exception, the temp file
    is cleaned up automatically.

    Args:
        df: DataFrame to write.
        output: Target Parquet file path.
        **kwargs: Additional keyword arguments passed to df.to_parquet(),
            e.g., index=False, compression='snappy'.
    """
    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=output.parent,
        delete=False,
    ) as tmp_fh:
        tmp = Path(tmp_fh.name)
    try:
        df.to_parquet(tmp, **kwargs)
        tmp.replace(output)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise

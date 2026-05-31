# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Additional branch-coverage tests for :mod:`vmaf_train.data.manifest_scan`.

Complements ``test_manifest_scan.py`` by covering the remaining uncovered
paths: blank-key CSV rows, bad MOS values, NotADirectoryError on scan,
multi-chunk SHA-256.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from vmaf_train.data import manifest_scan
from vmaf_train.data.manifest_scan import _key_from_relpath, _sha256, load_mos_csv, scan


def test_load_mos_csv_skips_blank_keys(tmp_path: Path) -> None:
    """Branch: ``if not key: continue`` — blank-keyed rows must be ignored."""
    csv = tmp_path / "mos.csv"
    csv.write_text(
        "key,mos\n"
        "\n"  # row with empty fields — blank key
        ",42.0\n"  # blank key, valid mos — should be skipped
        "good,55.5\n"
    )
    mos = load_mos_csv(csv)
    assert mos == {"good": 55.5}


def test_load_mos_csv_rejects_bad_mos_value(tmp_path: Path) -> None:
    """Branch: ``float(row["mos"])`` raises → wrap as ValueError with context."""
    csv = tmp_path / "mos.csv"
    csv.write_text("key,mos\nclip,not-a-number\n")
    with pytest.raises(ValueError, match="bad mos for key='clip'"):
        load_mos_csv(csv)


def test_scan_raises_on_missing_directory(tmp_path: Path) -> None:
    """Branch: ``if not root.is_dir(): raise NotADirectoryError``."""
    missing = tmp_path / "does-not-exist"
    with pytest.raises(NotADirectoryError):
        scan("nflx", missing)


def test_scan_raises_on_file_passed_as_root(tmp_path: Path) -> None:
    """A regular file is not a directory either."""
    f = tmp_path / "a-file"
    f.write_bytes(b"hi")
    with pytest.raises(NotADirectoryError):
        scan("nflx", f)


def test_scan_skips_unknown_suffixes(tmp_path: Path) -> None:
    (tmp_path / "vid.yuv").write_bytes(b"a")
    (tmp_path / "doc.txt").write_bytes(b"a")
    (tmp_path / "img.png").write_bytes(b"a")
    entries = scan("nflx", tmp_path)
    assert [e.key for e in entries] == ["vid"]


def test_sha256_handles_multi_chunk_payload(tmp_path: Path) -> None:
    """Exercise the chunked-read loop with a >1 MiB file."""
    payload = b"x" * ((1 << 20) + 17)  # 1 MiB + 17 bytes → two chunks
    f = tmp_path / "big.bin"
    f.write_bytes(payload)
    assert _sha256(f) == hashlib.sha256(payload).hexdigest()


def test_key_from_relpath_strips_suffix_and_joins_with_underscore() -> None:
    assert _key_from_relpath(Path("a.yuv")) == "a"
    assert _key_from_relpath(Path("sub/b.y4m")) == "sub_b"
    assert _key_from_relpath(Path("deep/nested/c.mp4")) == "deep_nested_c"


def test_scan_picks_up_all_documented_suffixes(tmp_path: Path) -> None:
    """Test the full ``_VIDEO_SUFFIXES`` allowlist (yuv/y4m/mp4/mkv/webm)."""
    for name in ("a.yuv", "b.y4m", "c.mp4", "d.mkv", "e.webm"):
        (tmp_path / name).write_bytes(b"x")
    entries = scan("nflx", tmp_path)
    assert sorted(e.key for e in entries) == ["a", "b", "c", "d", "e"]


def test_scan_suffix_match_is_case_insensitive(tmp_path: Path) -> None:
    """``.suffix.lower()`` branch — uppercase extensions still match."""
    (tmp_path / "X.YUV").write_bytes(b"x")
    entries = scan("nflx", tmp_path)
    assert len(entries) == 1
    assert entries[0].key == "X"


def test_load_mos_csv_with_whitespace_keys_trims(tmp_path: Path) -> None:
    csv = tmp_path / "mos.csv"
    csv.write_text("key,mos\n  spaced  ,1.5\n")
    mos = load_mos_csv(csv)
    assert mos == {"spaced": 1.5}


def test_scan_entry_dataclass_is_frozen() -> None:
    import dataclasses

    entry = manifest_scan.ScanEntry(key="k", path="p", sha256="s", mos=None)
    with pytest.raises(dataclasses.FrozenInstanceError):
        entry.mos = 1.0  # type: ignore[misc]

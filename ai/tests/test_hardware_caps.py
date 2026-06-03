# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``ai/scripts/hardware_caps_loader.py`` (ADR-0335)."""

from __future__ import annotations

import csv
import io
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from ai.scripts.hardware_caps_loader import (  # noqa: E402
    ENCODER_TO_CODEC,
    REQUIRED_COLUMNS,
    HardwareCapsError,
    HardwareCapsTable,
    cap_vector_for,
    row_as_dict,
)

_CSV_PATH = _REPO_ROOT / "ai" / "data" / "hardware_caps.csv"

# Architectures the digest committed to fingerprinting (the three
# named arches plus their predecessors).
EXPECTED_ARCHES = {
    "alchemist",
    "battlemage",
    "rdna3",
    "rdna4",
    "ada-lovelace",
    "blackwell",
}


# ---------------------------------------------------------------------------
# CSV-on-disk integrity (the deployed table)
# ---------------------------------------------------------------------------


def test_csv_loads_and_round_trips() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    assert len(table) >= 6
    # Every row serialises to a JSON-friendly dict that contains every
    # required column key.
    for row in table:
        d = row_as_dict(row)
        for col in REQUIRED_COLUMNS:
            assert col in d, f"row {row.arch_name}: missing key {col}"


def test_csv_covers_target_architectures() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    archs = {row.arch_name for row in table}
    missing = EXPECTED_ARCHES - archs
    assert not missing, f"missing required architectures: {missing}"


def test_csv_has_no_benchmark_columns() -> None:
    """The fingerprint table is prior-only; no perf numbers allowed."""
    raw = _CSV_PATH.read_text(encoding="utf-8")
    forbidden_substrings = (
        "fps_",
        "throughput",
        "mbps",
        "latency",
        "watts",
        "tdp",
        "score_",
        "vmaf_",
    )
    # Scan the header row only — the comment block above legitimately
    # references "benchmark" while explaining why those fields are absent.
    header = next(line for line in raw.splitlines() if line and not line.lstrip().startswith("#"))
    lower = header.lower()
    hits = [s for s in forbidden_substrings if s in lower]
    assert not hits, f"benchmark-shaped columns leaked into header: {hits}"


def test_csv_every_row_has_primary_source() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    for row in table:
        assert row.source_url.startswith("https://")
        assert "wikipedia" not in row.source_url
        assert "wikichip" not in row.source_url
        # Vendor primary source must be on a vendor-controlled domain.
        host = row.source_url.split("/")[2]
        if row.vendor == "intel":
            assert host.endswith("intel.com"), host
        elif row.vendor == "amd":
            # AMD ships docs on amd.com and gpuopen.com (also AMD).
            assert host.endswith(("amd.com", "gpuopen.com")), host
        elif row.vendor == "nvidia":
            assert host.endswith("nvidia.com"), host


def test_csv_every_row_verified_2026_05_08() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    for row in table:
        assert row.verified_date == "2026-05-08", row.arch_name


def test_csv_no_unverified_rows() -> None:
    """No row may ship with placeholder / UNVERIFIED markers."""
    table = HardwareCapsTable.load(_CSV_PATH)
    sentinels = ("unverified", "todo", "unknown", "placeholder", "xxx", "tbd")
    for row in table:
        for col_value in row_as_dict(row).values():
            text = str(col_value).lower()
            for sentinel in sentinels:
                assert sentinel not in text, (
                    f"row {row.arch_name}: column contains {sentinel!r}: " f"{col_value!r}"
                )


# ---------------------------------------------------------------------------
# Schema validation (the parser's invariants)
# ---------------------------------------------------------------------------


_HEADER = ",".join(REQUIRED_COLUMNS) + "\n"


def _make_csv(rows: list[dict[str, str]]) -> str:
    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=list(REQUIRED_COLUMNS))
    writer.writeheader()
    for row in rows:
        writer.writerow(row)
    return buf.getvalue()


def _good_row(**overrides: str) -> dict[str, str]:
    base = {
        "arch_name": "blackwell",
        "vendor": "nvidia",
        "gen_year": "2025",
        "codecs_supported": "h264|hevc|av1",
        "max_resolution_per_codec": ("h264=4096x4096|hevc=8192x8192|av1=8192x8192"),
        "encoding_blocks": "3",
        "tensor_cores": "1",
        "npu_present": "0",
        "driver_min_version": "nvidia-570.86.10",
        "source_url": "https://docs.nvidia.com/x",
        "verified_date": "2026-05-08",
    }
    base.update(overrides)
    return base


def test_loader_rejects_missing_columns() -> None:
    text = "arch_name,vendor\nblackwell,nvidia\n"
    with pytest.raises(HardwareCapsError, match="missing required columns"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_empty_field() -> None:
    text = _make_csv([_good_row(driver_min_version="")])
    with pytest.raises(HardwareCapsError, match="is empty"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_bad_vendor() -> None:
    text = _make_csv([_good_row(vendor="apple")])
    with pytest.raises(HardwareCapsError, match="vendor must be"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_wikipedia_source() -> None:
    text = _make_csv([_good_row(source_url="https://en.wikipedia.org/wiki/Blackwell")])
    with pytest.raises(HardwareCapsError, match="community wiki"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_zero_encoding_blocks() -> None:
    text = _make_csv([_good_row(encoding_blocks="0")])
    with pytest.raises(HardwareCapsError, match="encoding_blocks must be"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_missing_codec_dims() -> None:
    text = _make_csv(
        [
            _good_row(
                codecs_supported="h264|hevc|av1",
                max_resolution_per_codec="h264=4096x4096|hevc=8192x8192",
            )
        ]
    )
    with pytest.raises(HardwareCapsError, match="missing entries"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_bad_date() -> None:
    text = _make_csv([_good_row(verified_date="May 8 2026")])
    with pytest.raises(HardwareCapsError, match="verified_date must be ISO"):
        HardwareCapsTable.loads(text)


def test_loader_rejects_duplicate_arch() -> None:
    text = _make_csv([_good_row(), _good_row()])
    with pytest.raises(HardwareCapsError, match="duplicate arch_name"):
        HardwareCapsTable.loads(text)


# ---------------------------------------------------------------------------
# cap_vector_for() — feature-vector emission
# ---------------------------------------------------------------------------


def test_cap_vector_blackwell_av1() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="av1_nvenc", encoder_arch_hint="blackwell")
    assert vec["hwcap_known"] == 1
    assert vec["hwcap_arch_name"] == "blackwell"
    assert vec["hwcap_vendor"] == "nvidia"
    assert vec["hwcap_codec"] == "av1"
    assert vec["hwcap_codec_supported"] == 1
    assert vec["hwcap_max_width"] == 8192
    assert vec["hwcap_max_height"] == 8192
    assert vec["hwcap_encoding_blocks"] == 3
    assert vec["hwcap_tensor_cores"] == 1
    assert vec["hwcap_verified_date"] == "2026-05-08"


def test_cap_vector_battlemage_hevc() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="hevc_qsv", encoder_arch_hint="battlemage")
    assert vec["hwcap_known"] == 1
    assert vec["hwcap_vendor"] == "intel"
    assert vec["hwcap_codec_supported"] == 1
    assert vec["hwcap_max_width"] == 8192


def test_cap_vector_rdna4_av1() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="av1_amf", encoder_arch_hint="rdna4")
    assert vec["hwcap_known"] == 1
    assert vec["hwcap_vendor"] == "amd"
    assert vec["hwcap_codec"] == "av1"
    assert vec["hwcap_codec_supported"] == 1


def test_cap_vector_unknown_arch_returns_blank() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="av1_nvenc", encoder_arch_hint="kepler")
    assert vec["hwcap_known"] == 0
    assert vec["hwcap_arch_name"] is None
    assert vec["hwcap_max_width"] is None


def test_cap_vector_no_arch_hint_returns_blank() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="av1_nvenc", encoder_arch_hint=None)
    assert vec["hwcap_known"] == 0


def test_cap_vector_software_encoder_returns_blank() -> None:
    """libx264 / libsvtav1 are CPU encoders — no GPU fingerprint applies."""
    table = HardwareCapsTable.load(_CSV_PATH)
    vec = cap_vector_for(table, encoder="libx264", encoder_arch_hint="blackwell")
    assert vec["hwcap_known"] == 0


def test_cap_vector_keys_are_stable() -> None:
    """Schema must be the same shape regardless of resolution success."""
    table = HardwareCapsTable.load(_CSV_PATH)
    a = cap_vector_for(table, encoder="av1_nvenc", encoder_arch_hint="blackwell")
    b = cap_vector_for(table, encoder="av1_nvenc", encoder_arch_hint="kepler")
    c = cap_vector_for(table, encoder="libx264", encoder_arch_hint=None)
    assert set(a.keys()) == set(b.keys()) == set(c.keys())


def test_encoder_codec_map_is_well_formed() -> None:
    for enc, codec in ENCODER_TO_CODEC.items():
        assert codec in {"h264", "hevc", "av1"}, enc


def test_arch_lookup_normalises_separators() -> None:
    table = HardwareCapsTable.load(_CSV_PATH)
    assert table.by_arch("ada lovelace") is not None
    assert table.by_arch("ADA-LOVELACE") is not None
    assert table.by_arch("ada_lovelace") is not None

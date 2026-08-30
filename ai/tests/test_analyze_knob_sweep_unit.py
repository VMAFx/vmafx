# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for helpers in ``ai/scripts/analyze_knob_sweep.py`` not covered
by the existing ``test_knob_sweep_analysis.py``.

The existing file covers ``pareto_frontier``, ``stratify``, and
``detect_recipe_regressions`` via a 20-row synthetic fixture.  This file
covers the five remaining helpers that that fixture does not exercise directly:

* ``_stable_knob_repr``    — dict → stable ``k=v,...`` string
* ``_slug``                — arbitrary string → filename-safe slug
* ``_closest_bare_at_bitrate`` — bare-default row lookup by bitrate tolerance
* ``write_slice_csv``      — writes a per-slice Pareto CSV
* ``write_summary_md``     — writes the aggregate markdown summary

All tests are CPU-only, require no model files, and run without calling
libvmaf or ffmpeg.
"""

from __future__ import annotations

import csv
import importlib.util
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "analyze_knob_sweep.py"


def _load_module():
    # Use a unique module-name to avoid collision with test_knob_sweep_analysis.py
    # which may have already registered "analyze_knob_sweep".  The dataclass
    # machinery resolves SweepRow.__module__ against sys.modules, so the
    # registration key MUST match the spec name passed to
    # spec_from_file_location.
    _MOD_NAME = "analyze_knob_sweep_unit"
    if _MOD_NAME in sys.modules:
        return sys.modules[_MOD_NAME]
    spec = importlib.util.spec_from_file_location(_MOD_NAME, _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[_MOD_NAME] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def aks():
    return _load_module()


# ---------------------------------------------------------------------------
# Helper: build a SweepRow without touching JSON or files
# ---------------------------------------------------------------------------


def _row(mod, source, codec, rc_mode, bitrate, vmaf, enc_ms, bare=False):
    return mod.SweepRow(
        source=source,
        codec=codec,
        rc_mode=rc_mode,
        preset="p4",
        quality="28",
        knob_combo="bare" if bare else "tuned",
        bitrate_kbps=bitrate,
        vmaf_score=vmaf,
        encode_time_ms=enc_ms,
        is_bare_default=bare,
        extras={},
    )


# ---------------------------------------------------------------------------
# _stable_knob_repr
# ---------------------------------------------------------------------------


class TestStableKnobRepr:
    def test_empty_dict_returns_empty_string(self, aks):
        assert aks._stable_knob_repr({}) == ""

    def test_single_entry(self, aks):
        assert aks._stable_knob_repr({"aq": "1"}) == "aq=1"

    def test_keys_sorted_alphabetically(self, aks):
        result = aks._stable_knob_repr({"z": "3", "a": "1", "m": "2"})
        assert result == "a=1,m=2,z=3"

    def test_non_dict_returns_empty_string(self, aks):
        # Branch: not a Mapping
        assert aks._stable_knob_repr(None) == ""
        assert aks._stable_knob_repr("not-a-dict") == ""
        assert aks._stable_knob_repr(42) == ""

    def test_dict_with_numeric_values_stringified(self, aks):
        result = aks._stable_knob_repr({"crf": 28, "preset": "slow"})
        assert result == "crf=28,preset=slow"

    def test_iteration_order_irrelevant(self, aks):
        # Two dicts with same keys but different insertion order
        d1 = {"b": "2", "a": "1"}
        d2 = {"a": "1", "b": "2"}
        assert aks._stable_knob_repr(d1) == aks._stable_knob_repr(d2)


# ---------------------------------------------------------------------------
# _slug
# ---------------------------------------------------------------------------


class TestSlug:
    def test_alphanumeric_unchanged(self, aks):
        assert aks._slug("libx264") == "libx264"

    def test_hyphen_and_underscore_preserved(self, aks):
        assert aks._slug("my-source_01") == "my-source_01"

    def test_spaces_replaced_with_underscore(self, aks):
        assert aks._slug("source a") == "source_a"

    def test_slashes_replaced(self, aks):
        assert aks._slug("path/to/file") == "path_to_file"

    def test_empty_string_returns_unknown(self, aks):
        assert aks._slug("") == "unknown"

    def test_all_special_chars_replaced(self, aks):
        result = aks._slug("!@#$%")
        assert all(c == "_" for c in result)
        assert len(result) == 5

    def test_numbers_preserved(self, aks):
        assert aks._slug("1080p") == "1080p"


# ---------------------------------------------------------------------------
# _closest_bare_at_bitrate
# ---------------------------------------------------------------------------


class TestClosestBareAtBitrate:
    def test_no_bare_rows_returns_none(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000, bare=False)
        assert aks._closest_bare_at_bitrate(candidate, [], bitrate_tol_pct=5.0) is None

    def test_returns_closest_within_tolerance(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000, bare=False)
        bare_close = _row(aks, "s", "codec", "cq", 2050, 91.0, 4000, bare=True)
        bare_far = _row(aks, "s", "codec", "cq", 3000, 95.0, 4000, bare=True)
        result = aks._closest_bare_at_bitrate(
            candidate, [bare_close, bare_far], bitrate_tol_pct=5.0
        )
        assert result is bare_close

    def test_returns_none_when_all_outside_tolerance(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000)
        bare_far = _row(aks, "s", "codec", "cq", 3000, 92.0, 4000, bare=True)
        result = aks._closest_bare_at_bitrate(candidate, [bare_far], bitrate_tol_pct=5.0)
        assert result is None

    def test_picks_closest_among_multiple_within_tolerance(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000)
        bare_near = _row(aks, "s", "codec", "cq", 2010, 91.0, 4000, bare=True)
        bare_mid = _row(aks, "s", "codec", "cq", 2060, 92.0, 4000, bare=True)
        # 5% of 2000 = 100 kbps tolerance; both are within tolerance
        result = aks._closest_bare_at_bitrate(candidate, [bare_mid, bare_near], bitrate_tol_pct=5.0)
        assert result is bare_near  # gap=10 < gap=60

    def test_exact_bitrate_match_returns_that_row(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000)
        bare = _row(aks, "s", "codec", "cq", 2000, 91.0, 4000, bare=True)
        result = aks._closest_bare_at_bitrate(candidate, [bare], bitrate_tol_pct=1.0)
        assert result is bare

    def test_tolerance_zero_requires_exact_match(self, aks):
        candidate = _row(aks, "s", "codec", "cq", 2000, 90.0, 4000)
        bare = _row(aks, "s", "codec", "cq", 2001, 91.0, 4000, bare=True)
        result = aks._closest_bare_at_bitrate(candidate, [bare], bitrate_tol_pct=0.0)
        assert result is None


# ---------------------------------------------------------------------------
# write_slice_csv
# ---------------------------------------------------------------------------


class TestWriteSliceCsv:
    def test_creates_file_with_correct_name(self, aks, tmp_path):
        key = ("sourceA", "libx264", "cq")
        hull = [_row(aks, "sourceA", "libx264", "cq", 2000, 92.0, 4000, bare=True)]
        path = aks.write_slice_csv(tmp_path, key, hull)
        assert path.exists()
        assert path.name == "pareto_sourceA_libx264_cq.csv"

    def test_csv_has_header_row(self, aks, tmp_path):
        key = ("s", "codec", "rc")
        hull = []
        path = aks.write_slice_csv(tmp_path, key, hull)
        with path.open(encoding="utf-8") as fh:
            reader = csv.reader(fh)
            header = next(reader)
        assert "source" in header
        assert "vmaf_score" in header
        assert "bitrate_kbps" in header
        assert "is_bare_default" in header

    def test_csv_contains_hull_rows(self, aks, tmp_path):
        key = ("clip1", "libx265", "vbr")
        hull = [
            _row(aks, "clip1", "libx265", "vbr", 1500, 88.5, 3000, bare=True),
            _row(aks, "clip1", "libx265", "vbr", 2500, 94.0, 3500, bare=False),
        ]
        path = aks.write_slice_csv(tmp_path, key, hull)
        with path.open(encoding="utf-8") as fh:
            rows = list(csv.DictReader(fh))
        assert len(rows) == 2
        # Values are stringified floats; check rough correctness
        assert float(rows[0]["vmaf_score"]) == pytest.approx(88.5, abs=1e-4)
        assert float(rows[1]["bitrate_kbps"]) == pytest.approx(2500.0, abs=1e-2)

    def test_special_chars_in_key_produce_safe_filename(self, aks, tmp_path):
        key = ("clip/01", "h264.nvenc", "cbr")
        hull = []
        path = aks.write_slice_csv(tmp_path, key, hull)
        assert "/" not in path.name
        assert "." not in path.name.replace(".csv", "")

    def test_creates_out_dir_if_missing(self, aks, tmp_path):
        key = ("s", "c", "r")
        subdir = tmp_path / "nested" / "reports"
        assert not subdir.exists()
        aks.write_slice_csv(subdir, key, [])
        assert subdir.exists()


# ---------------------------------------------------------------------------
# write_summary_md
# ---------------------------------------------------------------------------


class TestWriteSummaryMd:
    def _hull_row(self, mod, bitrate=2000, vmaf=92.0):
        return _row(mod, "src", "libx264", "cq", bitrate, vmaf, 4000, bare=True)

    def test_creates_summary_md(self, aks, tmp_path):
        hulls = {("src", "libx264", "cq"): [self._hull_row(aks)]}
        path = aks.write_summary_md(tmp_path, hulls, [])
        assert path.exists()
        assert path.name == "summary.md"

    def test_summary_contains_slice_count(self, aks, tmp_path):
        hulls = {
            ("src", "libx264", "cq"): [self._hull_row(aks)],
            ("src", "libx265", "vbr"): [self._hull_row(aks, 3000, 95.0)],
        }
        path = aks.write_summary_md(tmp_path, hulls, [])
        content = path.read_text(encoding="utf-8")
        assert "2" in content  # "Realised slices: **2**"
        assert "libx264" in content
        assert "libx265" in content

    def test_no_regressions_message_when_empty(self, aks, tmp_path):
        hulls = {("src", "codec", "rc"): [self._hull_row(aks)]}
        path = aks.write_summary_md(tmp_path, hulls, regressions=[])
        content = path.read_text(encoding="utf-8")
        assert "No regressions detected" in content

    def test_regression_table_when_regressions_present(self, aks, tmp_path):
        hulls = {("src", "libx264", "cq"): [self._hull_row(aks)]}
        regression = {
            "source": "src",
            "codec": "libx264",
            "rc_mode": "cq",
            "candidate_knob_combo": "aq=1",
            "candidate_vmaf": 88.5,
            "bare_vmaf": 92.0,
            "vmaf_delta": -3.5,
        }
        path = aks.write_summary_md(tmp_path, hulls, regressions=[regression])
        content = path.read_text(encoding="utf-8")
        assert "aq=1" in content
        assert "-3.50" in content
        assert "must not ship" in content

    def test_creates_out_dir_if_missing(self, aks, tmp_path):
        subdir = tmp_path / "new_reports"
        assert not subdir.exists()
        aks.write_summary_md(subdir, {}, [])
        assert subdir.exists()

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push for vmaftune.jsonio.

Exercises every documented branch in the three public functions:

* :func:`nan_to_none` — float NaN, float Inf, nested dict, nested list,
  nested tuple, passthrough of non-float scalars.
* :func:`dumps_strict` — round-trip, NaN replaced by null, sort_keys,
  no-indent option.
* :func:`write_json_strict` — atomic write, parent-mkdir, trailing
  newline toggling, tmp file renamed to target.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.jsonio import dumps_strict, nan_to_none, write_json_strict  # noqa: E402

# ---------------------------------------------------------------------------
# nan_to_none
# ---------------------------------------------------------------------------


class TestNanToNone:
    def test_nan_float_becomes_none(self) -> None:
        assert nan_to_none(float("nan")) is None

    def test_inf_float_becomes_none(self) -> None:
        assert nan_to_none(float("inf")) is None

    def test_neg_inf_float_becomes_none(self) -> None:
        assert nan_to_none(float("-inf")) is None

    def test_finite_float_unchanged(self) -> None:
        assert nan_to_none(3.14) == pytest.approx(3.14)

    def test_zero_float_unchanged(self) -> None:
        assert nan_to_none(0.0) == 0.0

    def test_integer_passthrough(self) -> None:
        assert nan_to_none(42) == 42

    def test_string_passthrough(self) -> None:
        assert nan_to_none("hello") == "hello"

    def test_none_passthrough(self) -> None:
        assert nan_to_none(None) is None

    def test_bool_passthrough(self) -> None:
        assert nan_to_none(True) is True
        assert nan_to_none(False) is False

    def test_dict_recurses(self) -> None:
        data = {"a": 1.0, "b": float("nan"), "c": {"d": float("inf")}}
        result = nan_to_none(data)
        assert result == {"a": 1.0, "b": None, "c": {"d": None}}

    def test_list_recurses(self) -> None:
        data = [1.0, float("nan"), "ok", float("inf")]
        result = nan_to_none(data)
        assert result == [1.0, None, "ok", None]

    def test_tuple_recurses_returns_list(self) -> None:
        data = (1.0, float("nan"), 3.0)
        result = nan_to_none(data)
        # tuples are processed as sequences and returned as list
        assert result == [1.0, None, 3.0]

    def test_nested_list_in_dict(self) -> None:
        data = {"scores": [92.0, float("nan"), 85.5]}
        result = nan_to_none(data)
        assert result == {"scores": [92.0, None, 85.5]}

    def test_empty_dict(self) -> None:
        assert nan_to_none({}) == {}

    def test_empty_list(self) -> None:
        assert nan_to_none([]) == []


# ---------------------------------------------------------------------------
# dumps_strict
# ---------------------------------------------------------------------------


class TestDumpsStrict:
    def test_nan_serialises_as_null(self) -> None:
        result = dumps_strict({"score": float("nan")})
        assert '"score": null' in result

    def test_inf_serialises_as_null(self) -> None:
        result = dumps_strict({"score": float("inf")})
        assert '"score": null' in result

    def test_finite_float_preserved(self) -> None:
        result = dumps_strict({"score": 92.5})
        parsed = json.loads(result)
        assert parsed["score"] == pytest.approx(92.5)

    def test_sort_keys_true_by_default(self) -> None:
        data = {"z": 1, "a": 2, "m": 3}
        result = dumps_strict(data)
        keys = [k for k in json.loads(result)]
        assert keys == sorted(keys)

    def test_sort_keys_false_preserves_order(self) -> None:
        data = {"z": 1, "a": 2, "m": 3}
        result = dumps_strict(data, sort_keys=False)
        parsed = json.loads(result)
        # In Python 3.7+ dicts preserve insertion order
        assert list(parsed.keys()) == ["z", "a", "m"]

    def test_indent_none_compact_output(self) -> None:
        result = dumps_strict({"a": 1}, indent=None)
        assert "\n" not in result

    def test_indent_2_produces_indented_output(self) -> None:
        result = dumps_strict({"a": 1}, indent=2)
        assert "  " in result

    def test_result_is_valid_json(self) -> None:
        data = {"a": [1, 2, 3], "b": {"c": float("nan")}}
        result = dumps_strict(data)
        parsed = json.loads(result)
        assert parsed["b"]["c"] is None

    def test_allow_nan_false_would_raise_on_nan(self) -> None:
        # Confirm that without nan_to_none, json.dumps would raise on nan
        # (this validates our wrapper is necessary)
        with pytest.raises((ValueError, TypeError)):
            json.dumps({"v": float("nan")}, allow_nan=False)


# ---------------------------------------------------------------------------
# write_json_strict
# ---------------------------------------------------------------------------


class TestWriteJsonStrict:
    def test_writes_file_with_trailing_newline(self, tmp_path: Path) -> None:
        out = tmp_path / "result.json"
        write_json_strict(out, {"a": 1})
        text = out.read_text(encoding="utf-8")
        assert text.endswith("\n")

    def test_trailing_newline_false(self, tmp_path: Path) -> None:
        out = tmp_path / "result.json"
        write_json_strict(out, {"a": 1}, trailing_newline=False)
        text = out.read_text(encoding="utf-8")
        assert not text.endswith("\n")

    def test_written_content_is_valid_json(self, tmp_path: Path) -> None:
        data = {"codec": "libx264", "vmaf": 92.4, "nan_val": float("nan")}
        out = tmp_path / "result.json"
        write_json_strict(out, data)
        parsed = json.loads(out.read_text(encoding="utf-8"))
        assert parsed["codec"] == "libx264"
        assert parsed["nan_val"] is None

    def test_creates_parent_dirs(self, tmp_path: Path) -> None:
        nested = tmp_path / "a" / "b" / "c" / "result.json"
        write_json_strict(nested, {"x": 1})
        assert nested.exists()
        assert json.loads(nested.read_text(encoding="utf-8")) == {"x": 1}

    def test_atomic_replace_no_tmp_file_left(self, tmp_path: Path) -> None:
        out = tmp_path / "result.json"
        write_json_strict(out, {"v": 1})
        # No .tmp file should remain after successful write
        tmp_files = list(tmp_path.glob("*.tmp"))
        assert tmp_files == []

    def test_overwrites_existing_file(self, tmp_path: Path) -> None:
        out = tmp_path / "result.json"
        out.write_text('{"old": true}', encoding="utf-8")
        write_json_strict(out, {"new": True})
        parsed = json.loads(out.read_text(encoding="utf-8"))
        assert "new" in parsed
        assert "old" not in parsed

    def test_nan_in_nested_list_serialised_as_null(self, tmp_path: Path) -> None:
        out = tmp_path / "scores.json"
        data = {"scores": [92.0, float("nan"), 85.5]}
        write_json_strict(out, data)
        parsed = json.loads(out.read_text(encoding="utf-8"))
        assert parsed["scores"] == [92.0, None, 85.5]

    def test_sort_keys_default_true(self, tmp_path: Path) -> None:
        out = tmp_path / "sorted.json"
        write_json_strict(out, {"z": 1, "a": 2})
        text = out.read_text(encoding="utf-8")
        # With sort_keys=True, "a" appears before "z" in the text
        assert text.index('"a"') < text.index('"z"')

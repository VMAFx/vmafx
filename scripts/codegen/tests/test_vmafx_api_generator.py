#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The VMAFx API generator and its gates (ADR-1852).

Every gate is shown refusing a planted defect: a hand-edited or missing
generated file fails the drift check; a reordered field, a removed function, a
renumbered constant, a field added to an unsized struct and an addition
without a version bump fail the append-only check; an incomplete compat field
map, an unknown type, an explicit `struct_size` and a misplaced error
parameter fail validation.
"""

from __future__ import annotations

import copy
import io
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any

import tomllib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts" / "codegen"))

from vmafx_api import abi_check, cli  # noqa: E402 -- path set above
from vmafx_api.layout import struct_layout  # noqa: E402
from vmafx_api.model import DefinitionError, parse  # noqa: E402

DEFINITION = ROOT / "core" / "api" / "vmafx.toml"


def document() -> dict[str, Any]:
    with DEFINITION.open("rb") as handle:
        return tomllib.load(handle)


def entry(items: list[dict[str, Any]], name: str) -> dict[str, Any]:
    return next(item for item in items if item["name"] == name)


def bumped(doc: dict[str, Any], version: str) -> dict[str, Any]:
    doc["api"]["abi_version"] = version
    return doc


def quiet(function: Callable[..., int], *args: object) -> int:
    with redirect_stdout(io.StringIO()):
        return function(*args)


class DriftCheckTest(unittest.TestCase):
    def test_committed_outputs_match(self) -> None:
        self.assertEqual(quiet(cli.main, ["--check", "--root", str(ROOT)]), 0)

    def test_hand_edit_and_missing_file_fail(self) -> None:
        files = cli.render(parse(document()))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            quiet(cli.write, root, files)
            self.assertEqual(quiet(cli.check, root, files), 0)
            header = root / "core/include/vmafx/vmafx.h"
            header.write_text(
                header.read_text().replace("VMAFX_E_RANGE = -8", "VMAFX_E_RANGE = -80")
            )
            self.assertEqual(quiet(cli.check, root, files), 1)
            quiet(cli.write, root, files)
            (root / "bindings/python/vmafx/_api.py").unlink()
            self.assertEqual(quiet(cli.check, root, files), 1)

    def test_write_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for path in cli.render(parse(document())):
                target = Path(tmp) / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / path, target)
            output = io.StringIO()
            with redirect_stdout(output):
                cli.main(["--write", "--root", tmp])
            self.assertEqual(output.getvalue(), "")


class AbiCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.base = document()
        self.old = parse(self.base)

    def findings(self, doc: dict[str, Any]) -> list[str]:
        result: list[str] = abi_check.compare(self.old, parse(doc))
        return result

    def test_unchanged_definition_passes(self) -> None:
        self.assertEqual(self.findings(copy.deepcopy(self.base)), [])

    def test_reordered_field_is_breaking(self) -> None:
        doc = bumped(copy.deepcopy(self.base), "0.2.0")
        fields = entry(doc["structs"], "VmafxScore")["fields"]
        fields[0], fields[1] = fields[1], fields[0]
        self.assertTrue(any("VmafxScore" in f for f in self.findings(doc)))

    def test_removed_function_is_breaking(self) -> None:
        doc = bumped(copy.deepcopy(self.base), "0.2.0")
        doc["functions"] = [f for f in doc["functions"] if f["name"] != "vmafx_feature_score"]
        doc["compat"] = [c for c in doc["compat"] if c["target"] != "vmafx_feature_score"]
        findings = self.findings(doc)
        self.assertTrue(any("vmafx_feature_score removed" in f for f in findings))
        self.assertTrue(any("vmaf_feature_score_at_index removed" in f for f in findings))

    def test_renumbered_constant_is_breaking(self) -> None:
        doc = bumped(copy.deepcopy(self.base), "0.2.0")
        entry(doc["status"], "VMAFX_E_RANGE")["value"] = -80
        self.assertTrue(any("VMAFX_E_RANGE renumbered" in f for f in self.findings(doc)))

    def test_major_bump_allows_a_break(self) -> None:
        doc = bumped(copy.deepcopy(self.base), "1.0.0")
        entry(doc["status"], "VMAFX_E_RANGE")["value"] = -80
        self.assertEqual(self.findings(doc), [])

    def test_appended_field_needs_a_version_bump(self) -> None:
        doc = copy.deepcopy(self.base)
        entry(doc["structs"], "VmafxScore")["fields"].append({"name": "flags", "type": "u32"})
        self.assertTrue(any("without an ABI version bump" in f for f in self.findings(doc)))
        self.assertEqual(self.findings(bumped(doc, "0.2.0")), [])

    def test_unsized_struct_cannot_grow(self) -> None:
        base = copy.deepcopy(self.base)
        base["structs"].append({"name": "VmafxPair", "fields": [{"name": "a", "type": "u32"}]})
        old = parse(base)
        doc = bumped(copy.deepcopy(base), "0.2.0")
        entry(doc["structs"], "VmafxPair")["fields"].append({"name": "b", "type": "u32"})
        self.assertTrue(
            any("grew without struct_size" in f for f in abi_check.compare(old, parse(doc)))
        )


class ValidationTest(unittest.TestCase):
    def assert_refused(self, doc: dict[str, Any], text: str) -> None:
        with self.assertRaisesRegex(DefinitionError, text):
            parse(doc)

    def test_incomplete_compat_field_map(self) -> None:
        doc = document()
        del entry(doc["compat"], "vmaf_init")["build"]["from"]["gpumask"]
        self.assert_refused(doc, "missing \\['gpumask'\\]")

    def test_unknown_type(self) -> None:
        doc = document()
        entry(doc["functions"], "vmafx_feature_score")["params"][2]["type"] = "u128"
        self.assert_refused(doc, "unknown type")

    def test_explicit_struct_size(self) -> None:
        doc = document()
        entry(doc["structs"], "VmafxScore")["fields"].insert(
            0, {"name": "struct_size", "type": "u32"}
        )
        self.assert_refused(doc, "implied")

    def test_error_parameter_must_be_last(self) -> None:
        doc = document()
        params = entry(doc["functions"], "vmafx_context_destroy")["params"]
        params.reverse()
        self.assert_refused(doc, "error out-parameter is last")


class LayoutTest(unittest.TestCase):
    def test_score_layout(self) -> None:
        lay = struct_layout(parse(document()).struct("VmafxScore"))
        self.assertEqual(lay.size, 40)
        self.assertEqual([f.offset for f in lay.fields], [0, 4, 8, 16, 24, 32])


if __name__ == "__main__":
    unittest.main()

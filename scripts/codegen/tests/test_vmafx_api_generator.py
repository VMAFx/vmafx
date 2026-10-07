#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The VMAFx API generator and its gates on the real definition (ADR-1852).

Every gate is shown refusing a planted defect: a hand-edited or missing
generated file fails the drift check; a reordered field, a removed function, a
renumbered constant, a field added to an unsized struct, a changed `since`, an
addition into a frozen or older version node and an addition without a version bump
fail the append-only check; an incomplete compat field map, an unknown type,
an explicit `struct_size`, a misplaced error parameter, a missing `since` or
header and a deprecation without a usable replacement fail validation.
"""

from __future__ import annotations

import copy
import io
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any

from support import (
    DEFINITION,
    FIXTURES,
    ROOT,
    bumped,
    document,
    entry,
    next_patch,
    quiet,
    render_into,
)
from vmafx_api import abi_check, cli
from vmafx_api.layout import struct_layout
from vmafx_api.loader import parse
from vmafx_api.model import DefinitionError


class DriftCheckTest(unittest.TestCase):
    def test_committed_outputs_match(self) -> None:
        self.assertEqual(quiet(cli.main, ["--check", "--root", str(ROOT)]), 0)

    def test_hand_edit_and_missing_files_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = render_into(root, parse(document()))
            self.assertEqual(quiet(cli.check, root, files), 0)
            header = root / "core/include/vmafx/types.h"
            header.write_text(
                header.read_text().replace("VMAFX_E_RANGE = -8", "VMAFX_E_RANGE = -80")
            )
            self.assertEqual(quiet(cli.check, root, files), 1)
            quiet(cli.write, root, files)
            for path in ("bindings/python/vmafx/_api.py", "docs/api/vmafx/context.md"):
                (root / path).unlink()
                self.assertEqual(quiet(cli.check, root, files), 1)
                quiet(cli.write, root, files)
            (root / "core/src/vmafx.map").write_text("VMAFX_0.1 { global: *; };\n")
            self.assertEqual(quiet(cli.check, root, files), 1)

    def test_write_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for path in cli.render(parse(document())):
                target = Path(tmp) / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / path, target)
            argv = ["--root", tmp, "--definition", str(DEFINITION)]
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(cli.main(["--write", *argv]), 0)
            self.assertEqual(output.getvalue(), "")  # nothing to rewrite
            self.assertEqual(quiet(cli.main, ["--check", *argv]), 0)


class AbiCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.base = document()
        self.old = parse(self.base)

    def findings(self, doc: dict[str, Any]) -> list[str]:
        found: list[str] = abi_check.compare(self.old, parse(doc))
        return found

    def test_unchanged_definition_passes(self) -> None:
        self.assertEqual(self.findings(copy.deepcopy(self.base)), [])

    def test_reordered_field_needs_a_minor_bump_in_0x(self) -> None:
        doc = bumped(copy.deepcopy(self.base), next_patch(self.base))
        fields = entry(doc["structs"], "VmafxScore")["fields"]
        fields[0], fields[1] = fields[1], fields[0]
        self.assertTrue(any("VmafxScore" in f for f in self.findings(doc)))
        result = abi_check.report(self.old, parse(bumped(doc, "0.2.0")))
        self.assertEqual(result.findings, [])
        self.assertTrue(any("VmafxScore" in a for a in result.accepted))

    def test_removed_function_is_breaking(self) -> None:
        doc = copy.deepcopy(self.base)
        doc["functions"] = [f for f in doc["functions"] if f["name"] != "vmafx_feature_score"]
        doc["compat"] = [c for c in doc["compat"] if c["target"] != "vmafx_feature_score"]
        findings = self.findings(doc)
        self.assertTrue(any("vmafx_feature_score removed" in f for f in findings))
        self.assertTrue(any("vmaf_feature_score_at_index removed" in f for f in findings))

    def test_renumbered_constant_is_breaking(self) -> None:
        doc = copy.deepcopy(self.base)
        entry(doc["status"], "VMAFX_E_RANGE")["value"] = -80
        self.assertTrue(any("VMAFX_E_RANGE renumbered" in f for f in self.findings(doc)))

    def test_changed_since_is_breaking(self) -> None:
        doc = bumped(copy.deepcopy(self.base), next_patch(self.base))
        entry(doc["functions"], "vmafx_feature_score")["since"] = "0.0"
        self.assertTrue(any("since changed" in f for f in self.findings(doc)))

    def test_after_1_0_a_break_needs_a_major_bump(self) -> None:
        base = bumped(copy.deepcopy(self.base), "1.0.0")
        doc = bumped(copy.deepcopy(base), "1.1.0")
        entry(doc["status"], "VMAFX_E_RANGE")["value"] = -80
        old = parse(base)
        self.assertTrue(any("higher ABI major" in f for f in abi_check.compare(old, parse(doc))))
        self.assertEqual(abi_check.compare(old, parse(bumped(doc, "2.0.0"))), [])

    def test_appended_field_needs_a_version_bump(self) -> None:
        doc = copy.deepcopy(self.base)
        entry(doc["structs"], "VmafxScore")["fields"].append({"name": "flags", "type": "u32"})
        self.assertTrue(any("without an ABI version bump" in f for f in self.findings(doc)))
        self.assertEqual(self.findings(bumped(doc, next_patch(self.base))), [])

    def test_from_1_0_a_shipped_node_is_frozen(self) -> None:
        base = bumped(copy.deepcopy(self.base), "1.0.0")
        doc = bumped(copy.deepcopy(base), "1.0.1")
        added = copy.deepcopy(entry(doc["functions"], "vmafx_version_string"))
        doc["functions"].append({**added, "name": "vmafx_build_id", "since": "1.0"})
        found = abi_check.compare(parse(base), parse(doc))
        self.assertTrue(any("vmafx_build_id" in f and "frozen" in f for f in found), found)
        entry(doc["functions"], "vmafx_build_id")["since"] = "1.1"
        self.assertEqual(abi_check.compare(parse(base), parse(bumped(doc, "1.1.0"))), [])

    def test_0x_addition_joins_the_current_node_not_an_older_one(self) -> None:
        doc = bumped(copy.deepcopy(self.base), next_patch(self.base))
        added = copy.deepcopy(entry(doc["functions"], "vmafx_version_string"))
        doc["functions"].append({**added, "name": "vmafx_build_id", "since": "0.0"})
        self.assertTrue(any("older than the current minor" in f for f in self.findings(doc)))
        entry(doc["functions"], "vmafx_build_id")["since"] = "0.1"
        self.assertEqual(self.findings(doc), [])

    def test_unsized_struct_cannot_grow(self) -> None:
        base = copy.deepcopy(self.base)
        pair = {"name": "VmafxPair", "header": "vmafx/types.h", "since": "0.1"}
        base["structs"].append({**pair, "fields": [{"name": "a", "type": "u32"}]})
        doc = bumped(copy.deepcopy(base), next_patch(base))
        entry(doc["structs"], "VmafxPair")["fields"].append({"name": "b", "type": "u32"})
        found = abi_check.compare(parse(base), parse(doc))
        self.assertTrue(any("grew without struct_size" in f for f in found))

    def test_prototype_definition_upgrades_and_is_compatible(self) -> None:
        prototype = parse(document(FIXTURES / "schema1.toml"))
        self.assertEqual(abi_check.compare(prototype, self.old), [])
        # Later work packages only add functions: the prototype's are a subset.
        self.assertLessEqual(
            {f.name for f in prototype.functions}, {f.name for f in self.old.functions}
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
        entry(doc["functions"], "vmafx_context_destroy")["params"].reverse()
        self.assert_refused(doc, "error out-parameter is last")

    def test_missing_since_and_header(self) -> None:
        for table, name, key in (
            ("functions", "vmafx_feature_score", "since"),
            ("structs", "VmafxScore", "since"),
            ("status", "VMAFX_E_RANGE", "since"),
            ("handles", "VmafxError", "header"),
        ):
            doc = document()
            del entry(doc[table], name)[key]
            self.assert_refused(doc, f"missing `{key}`")

    def test_since_newer_than_the_abi(self) -> None:
        doc = document()
        entry(doc["functions"], "vmafx_feature_score")["since"] = "0.2"
        self.assert_refused(doc, "newer than ABI")

    def test_declaration_in_the_umbrella_or_an_unknown_header(self) -> None:
        doc = document()
        entry(doc["functions"], "vmafx_feature_score")["header"] = "vmafx/vmafx.h"
        self.assert_refused(doc, "umbrella")
        entry(doc["functions"], "vmafx_feature_score")["header"] = "vmafx/nowhere.h"
        self.assert_refused(doc, "not declared")

    def test_deprecation_needs_a_declared_replacement(self) -> None:
        doc = bumped(document(), "0.2.0")
        target = entry(doc["functions"], "vmafx_feature_score")
        target["deprecated"] = {"since": "0.2", "removal": "1.0"}
        self.assert_refused(doc, "missing `replacement`")
        target["deprecated"]["replacement"] = "vmafx_feature_score_v2"
        self.assert_refused(doc, "not a declared name")
        target["deprecated"] = {
            "since": "0.2",
            "removal": "0.2",
            "replacement": "vmafx_status_name",
        }
        self.assert_refused(doc, "removal 0.2 must follow since")


class LayoutTest(unittest.TestCase):
    def test_score_layout(self) -> None:
        api = parse(document())
        lay = struct_layout(api, api.struct("VmafxScore"))
        self.assertEqual(lay.size, 40)
        self.assertEqual([f.offset for f in lay.fields], [0, 4, 8, 16, 24, 32])


if __name__ == "__main__":
    unittest.main()

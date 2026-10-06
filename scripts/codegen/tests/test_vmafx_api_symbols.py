#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exported-symbol gates (ADR-1852): the generated version script, `.def` and
symbol list, and core/test/check_exported_symbols.py reading them.

Planted defects: a `vmafx_` export missing from the list, a listed symbol the
library does not export, an export in the wrong version node and an export
with none all fail the check; a version script naming a function no source
defines fails the link. The end-to-end cases build a small shared library with
the host compiler and skip, with the reason, when a tool is missing.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

from support import ROOT, fixture, run, tool
from vmafx_api import emit_symbols
from vmafx_api.loader import parse

CHECKER = ROOT / "core" / "test" / "check_exported_symbols.py"


def checker() -> ModuleType:
    spec = importlib.util.spec_from_file_location("check_exported_symbols", CHECKER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_exported_symbols"] = module
    spec.loader.exec_module(module)
    return module


NM_SAMPLE = """\
                 U malloc@GLIBC_2.2.5
0000000000000000 A VMAFX_0.1
0000000000000000 A VMAFX_0.2
0000000000001109 T vmafx_a@@VMAFX_0.1
000000000000110f T vmafx_b@@VMAFX_0.2
0000000000001114 T vmaf_c
"""


class EmittedSymbolsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(fixture())

    def test_one_node_per_minor_each_inheriting_the_previous(self) -> None:
        script = emit_symbols.version_script(self.api)
        self.assertIn("VMAFX_0.1 {\n    global:\n        vmafx_error_errno;", script)
        self.assertIn(
            "        vmafx_window_submit;\n};",
            script.split("VMAFX_0.2 {")[1].replace("} VMAFX_0.1;", "};"),
        )
        self.assertTrue(script.rstrip().endswith("} VMAFX_0.1;"))
        self.assertNotIn("local:", script)

    def test_split_library_hides_everything_unlisted(self) -> None:
        doc = fixture()
        doc["api"]["hide_unlisted"] = True
        script = emit_symbols.version_script(parse(doc))
        self.assertIn("    local:\n        *;\n}", script.split("VMAFX_0.2")[0])

    def test_def_and_symbol_list_cover_every_function(self) -> None:
        names = {f.name for f in self.api.functions}
        exports = emit_symbols.def_file(self.api).split("EXPORTS\n")[1].split()
        self.assertEqual(set(exports), names)
        rows = checker().listed_symbols_from_text(emit_symbols.symbol_list(self.api))
        self.assertEqual(rows["vmafx_window_submit"], "VMAFX_0.2")
        self.assertEqual(rows["vmafx_error_free"], "VMAFX_0.1")
        self.assertEqual(set(rows), names)


class CheckerLogicTest(unittest.TestCase):
    def setUp(self) -> None:
        self.module = checker()
        self.symbols = self.module.parse_nm(NM_SAMPLE)
        self.listed = {"vmafx_a": "VMAFX_0.1", "vmafx_b": "VMAFX_0.2"}

    def test_nm_reading(self) -> None:
        self.assertTrue(self.symbols.versions_visible)
        self.assertEqual(set(self.symbols.exports), {"vmafx_a", "vmafx_b", "vmaf_c"})
        self.assertEqual(self.symbols.exports["vmafx_b"], "VMAFX_0.2")

    def test_matching_list_passes(self) -> None:
        self.assertEqual(self.module.vmafx_findings(self.symbols, self.listed), [])

    def test_planted_defects_fail(self) -> None:
        cases = (
            ({"vmafx_a": "VMAFX_0.1"}, "vmafx_b: exported, missing from the VMAFx symbol list"),
            ({**self.listed, "vmafx_z": "VMAFX_0.2"}, "vmafx_z: listed (VMAFX_0.2), not exported"),
            (
                {**self.listed, "vmafx_b": "VMAFX_0.1"},
                "vmafx_b: exported in VMAFX_0.2, the list names VMAFX_0.1",
            ),
        )
        for listed, finding in cases:
            with self.subTest(finding=finding):
                self.assertIn(finding, self.module.vmafx_findings(self.symbols, listed))

    def test_versions_seen_on_exports_when_imports_are_plain(self) -> None:
        # A toolchain that links its imports unversioned (`w __cxa_finalize`,
        # T-VMAFX-SYMBOL-VERSIONS-UNSEEN-UBUNTU-2026-10-06): nm still prints
        # the exports' nodes, so a wrong node must be caught.
        symbols = self.module.parse_nm(NM_SAMPLE.replace("malloc@GLIBC_2.2.5", "malloc"))
        self.assertTrue(symbols.versions_visible)
        found = self.module.vmafx_findings(symbols, {**self.listed, "vmafx_b": "VMAFX_0.1"})
        self.assertEqual(found, ["vmafx_b: exported in VMAFX_0.2, the list names VMAFX_0.1"])

    def test_unversioned_export_fails_when_nm_shows_versions(self) -> None:
        symbols = self.module.parse_nm(NM_SAMPLE.replace("vmafx_b@@VMAFX_0.2", "vmafx_b"))
        found = self.module.vmafx_findings(symbols, self.listed)
        self.assertEqual(found, ["vmafx_b: exported in no version node, the list names VMAFX_0.2"])
        blind = self.module.parse_nm(
            NM_SAMPLE.replace("malloc@GLIBC_2.2.5", "malloc")
            .replace("@@VMAFX_0.1", "")
            .replace("@@VMAFX_0.2", "")
        )
        self.assertFalse(blind.versions_visible)
        self.assertEqual(self.module.vmafx_findings(blind, self.listed), [])


STUB = '__attribute__((visibility("default"))) void {name}(void) {{}}\n'


@unittest.skipIf(
    None in (tool("cc"), tool("nm"), tool("c++filt")), "needs cc, nm and c++filt on PATH"
)
class EndToEndTest(unittest.TestCase):
    def build(self, tmp: Path, names: list[str], script: str) -> tuple[int, str]:
        (tmp / "lib.c").write_text("".join(STUB.format(name=n) for n in names))
        (tmp / "vmafx.map").write_text(script)
        done = run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-fvisibility=hidden",
                f"-Wl,--version-script={tmp / 'vmafx.map'}",
                "-Wl,--no-undefined-version",
                str(tmp / "lib.c"),
                "-o",
                str(tmp / "libtest.so"),
            ]
        )
        return done.returncode, done.stderr

    def check(self, tmp: Path, listed: str, compat: str = "") -> tuple[int, str]:
        (tmp / "symbols.txt").write_text(listed)
        (tmp / "compat.txt").write_text(compat)
        done = run(
            [
                sys.executable,
                str(CHECKER),
                "--library",
                "vmafx",
                "--vmafx-symbols",
                str(tmp / "symbols.txt"),
                "--compat-symbols",
                str(tmp / "compat.txt"),
                str(tmp / "libtest.so"),
            ]
        )
        return done.returncode, done.stdout

    def test_library_matches_its_list_and_refuses_planted_defects(self) -> None:
        api = parse(fixture())
        names = sorted(f.name for f in api.functions)
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            self.assertEqual(self.build(tmp, names, emit_symbols.version_script(api))[0], 0)
            listed = emit_symbols.symbol_list(api)
            self.assertEqual(self.check(tmp, listed)[0], 0)
            status, output = self.check(tmp, listed.replace("vmafx_window_submit VMAFX_0.2\n", ""))
            self.assertEqual(status, 1)
            self.assertIn(
                "vmafx_window_submit: exported, missing from the VMAFx symbol list", output
            )
            status, output = self.check(
                tmp,
                listed.replace("vmafx_window_submit VMAFX_0.2", "vmafx_window_submit VMAFX_0.1"),
            )
            self.assertIn("exported in VMAFX_0.2, the list names VMAFX_0.1", output)

    def test_script_naming_an_undefined_function_fails_the_link(self) -> None:
        api = parse(fixture())
        names = sorted(f.name for f in api.functions if f.name != "vmafx_window_release")
        with tempfile.TemporaryDirectory() as raw:
            status, errors = self.build(Path(raw), names, emit_symbols.version_script(api))
        self.assertNotEqual(status, 0)
        self.assertIn("vmafx_window_release", errors)


if __name__ == "__main__":
    unittest.main()

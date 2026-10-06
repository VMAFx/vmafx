#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The libvmaf compat layer of the definition (ADR-1852 decision D3, RC4 WP6).

The `[[compat]]` rules (kinds, engine exceptions with their end, manual
sources and the vmafx_ functions they call), the engine-name header, the
per-backend version scripts, the compat symbol list and the conformance
tables, each shown refusing or reporting its planted defect; and
core/test/check_exported_symbols.py judging both libraries from those lists.
"""

from __future__ import annotations

import copy
import importlib.util
import re
import sys
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any, ClassVar

from support import DEFINITION, ROOT, document
from vmafx_api import emit_compat, emit_conformance, emit_symbols
from vmafx_api.loader import parse
from vmafx_api.model import DefinitionError

CHECKER = ROOT / "core" / "test" / "check_exported_symbols.py"


def checker() -> ModuleType:
    spec = importlib.util.spec_from_file_location("check_exported_symbols", CHECKER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_exported_symbols"] = module
    spec.loader.exec_module(module)
    return module


def compat(doc: dict[str, Any], name: str) -> dict[str, Any]:
    return next(item for item in doc["compat"] if item["name"] == name)


class DefinitionRulesTest(unittest.TestCase):
    """Every rule refuses a planted defect in a copy of the live definition."""

    def setUp(self) -> None:
        self.doc = document(DEFINITION)

    def refused(self, mutate: Any, message: str) -> None:
        doc = copy.deepcopy(self.doc)
        mutate(doc)
        with self.assertRaises(DefinitionError) as caught:
            parse(doc)
        self.assertIn(message, str(caught.exception))

    def test_live_definition_lists_every_libvmaf_function_once(self) -> None:
        api = parse(self.doc)
        names = [c.name for c in api.compats]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(names), 107)

    def test_unknown_kind(self) -> None:
        self.refused(lambda d: compat(d, "vmaf_close").update(kind="forward"), "kind is one of")

    def test_engine_without_backend(self) -> None:
        self.refused(lambda d: compat(d, "vmaf_flush_sycl").pop("engine_with"), "names its backend")

    def test_exception_without_end(self) -> None:
        self.refused(lambda d: compat(d, "vmaf_cuda_state_free").pop("until"), "what ends it")

    def test_unknown_backend(self) -> None:
        self.refused(
            lambda d: compat(d, "vmaf_cuda_state_free").update(engine_with="vulkan"),
            "engine_with is one of",
        )

    def test_manual_source_outside_the_compat_directory(self) -> None:
        self.refused(
            lambda d: compat(d, "vmaf_picture_alloc").update(file="core/src/picture.c"),
            "names its source under",
        )

    def test_manual_call_of_an_undeclared_function(self) -> None:
        self.refused(
            lambda d: compat(d, "vmaf_picture_alloc")["calls"].append("vmafx_no_such"),
            "unknown function vmafx_no_such",
        )


class EmittedCompatTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(document(DEFINITION))

    def test_engine_names_rename_every_compat_definition_but_the_exceptions(self) -> None:
        text = emit_compat.engine_names(self.api)
        self.assertIn("#define vmaf_picture_alloc vmaf_engine_picture_alloc\n", text)
        self.assertIn(
            "#if !VMAFX_ENGINE_EXPORTS_HIP\n#define vmaf_hip_available vmaf_engine_hip_available",
            text,
        )
        self.assertNotIn("vmaf_cuda_state_init", text)
        self.assertIn("#ifndef VMAF_PUBLIC_NAMES", text)

    def test_generated_source_holds_shims_and_glue_only(self) -> None:
        text = emit_compat.compat_source(self.api)
        self.assertIn("int vmaf_init(VmafContext **vmaf, VmafConfiguration cfg)", text)
        self.assertIn("    assert(record.index == (uint64_t)index);\n", text)  # `post`
        self.assertNotIn("int vmaf_read_pictures(", text)  # manual
        self.assertNotIn("vmaf_sycl_state_init", text)  # engine exception
        self.assertIn("#if VMAFX_BUILD_MCP\n", text)

    def test_legacy_maps_and_symbol_list(self) -> None:
        self.assertEqual(emit_symbols.legacy_backends(self.api), ["cuda", "hip", "metal", "sycl"])
        hip = emit_symbols.legacy_version_script(self.api, "hip")
        self.assertIn("VMAF_LEGACY_HIP {\n    global:\n        vmaf_hip_available;", hip)
        rows = checker().compat_rows_from_text(emit_symbols.compat_symbol_list(self.api))
        self.assertEqual(rows["vmaf_init"], "compat")
        self.assertEqual(rows["vmaf_hip_available"], "compat:!hip")
        self.assertEqual(rows["vmaf_mcp_init"], "compat:mcp")
        self.assertEqual(rows["vmaf_cuda_state_init"], "engine:cuda")
        self.assertEqual(len(rows), 107)

    def test_conformance_tables_cover_every_compat_definition(self) -> None:
        header = emit_conformance.header_text(self.api)
        table = emit_conformance.table_text(self.api)
        tabled = emit_conformance.tabled(self.api)
        self.assertEqual(len(tabled), 107 - 25)
        for item in tabled:
            self.assertIn(f'VMAF_COMPAT_ENTRY("{item.name}"', header)
            self.assertIn(f"    .{item.stem} = VMAF_COMPAT_THUNK(", table)
        self.assertIn('{"vmaf_cuda_state_init", "cuda"}', header)


# Error plumbing every compat source uses through compat_errno.h.
ERROR_HELPERS = {"vmafx_error_free", "vmafx_error_errno"}


def called_in(path: Path) -> set[str]:
    """vmafx_ functions a C source calls (comments removed)."""
    text = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
    return set(re.findall(r"\b(vmafx_\w+)\s*\(", text)) - ERROR_HELPERS


def call_findings(sources: dict[str, set[str]], declared: dict[str, set[str]]) -> list[str]:
    """A manual compat file calls exactly what its entries declare in `calls`."""
    out = []
    for path in sorted(declared):
        called = sources[path]
        out += [f"{path}: calls {n}, no entry declares it" for n in sorted(called - declared[path])]
        out += [f"{path}: declares {n}, never calls it" for n in sorted(declared[path] - called)]
    return out


class ManualCallsTest(unittest.TestCase):
    def test_manual_sources_call_what_the_definition_declares(self) -> None:
        declared: dict[str, set[str]] = {}
        for item in parse(document(DEFINITION)).compats:
            if item.kind == "manual":
                declared.setdefault(item.file, set()).update(item.calls)
        sources = {path: called_in(ROOT / path) for path in declared}
        self.assertEqual(call_findings(sources, declared), [])

    def test_planted_mismatch_is_found(self) -> None:
        sources = {"a.c": {"vmafx_x", "vmafx_y"}}
        self.assertEqual(
            call_findings(sources, {"a.c": {"vmafx_x", "vmafx_z"}}),
            ["a.c: calls vmafx_y, no entry declares it", "a.c: declares vmafx_z, never calls it"],
        )


class CheckerTest(unittest.TestCase):
    """check_exported_symbols.py with the compat list, planted defects included."""

    ROWS: ClassVar[dict[str, str]] = {
        "vmaf_init": "compat",
        "vmaf_hip_available": "compat:!hip",
        "vmaf_mcp_init": "compat:mcp",
        "vmaf_cuda_state_init": "engine:cuda",
    }

    def setUp(self) -> None:
        self.module = checker()

    def symbols(self, text: str) -> Any:
        return self.module.parse_nm("                 U malloc@GLIBC_2.2.5\n" + text)

    def test_expected_exports_per_build(self) -> None:
        expected = self.module.expected_exports
        self.assertEqual(
            expected(self.ROWS, "vmaf", set(), set()),
            {"vmaf_init": None, "vmaf_hip_available": None},
        )
        self.assertEqual(
            expected(self.ROWS, "vmaf", {"hip"}, {"mcp"}),
            {"vmaf_init": None, "vmaf_mcp_init": None},
        )
        self.assertEqual(
            expected(self.ROWS, "vmafx", {"hip", "cuda"}, set()),
            {"vmaf_hip_available": "VMAF_LEGACY_HIP", "vmaf_cuda_state_init": "VMAF_LEGACY_CUDA"},
        )
        self.assertEqual(expected(self.ROWS, "vmafx", set(), set()), {})

    def test_matching_library_passes_and_planted_defects_fail(self) -> None:
        exported = expected = self.module.expected_exports(self.ROWS, "vmaf", set(), set())
        good = self.symbols("0000000000001000 T vmaf_init\n0000000000001010 T vmaf_hip_available\n")
        findings = self.module.libvmaf_findings
        self.assertEqual(findings(good, exported, "vmaf"), [])
        missing = self.symbols("0000000000001000 T vmaf_init\n")
        self.assertIn(
            "vmaf_hip_available: the compat list gives it to libvmaf.so.3, not exported",
            findings(missing, expected, "vmaf"),
        )
        extra = self.symbols(
            "0000000000001000 T vmaf_init\n0000000000001010 T vmaf_hip_available\n"
            "0000000000001020 T vmaf_cuda_state_init\n"
        )
        self.assertIn(
            "vmaf_cuda_state_init: exported, but the compat list gives it to no libvmaf.so.3 "
            "of this build",
            findings(extra, expected, "vmaf"),
        )
        versioned = self.symbols(
            "0000000000001000 T vmaf_init@@VMAFX_0.1\n0000000000001010 T vmaf_hip_available\n"
        )
        self.assertIn(
            "vmaf_init: exported in VMAFX_0.1, expected no version node",
            findings(versioned, expected, "vmaf"),
        )


if __name__ == "__main__":
    unittest.main()

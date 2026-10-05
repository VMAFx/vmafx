#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Layout gates on the every-feature fixture: the computed layout, the C test
compiled by the host compiler, the Python binding's import check, and the
clang-format form of every generated C file (ADR-1852).

Planted defects: an array one element longer and a field one size wider in a
copy of a generated header must fail the C layout test; a wrong size in the
Python layout table must fail the binding's import. Compiler and formatter
tests skip, with the reason, when the tool is not installed.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

from support import ROOT, fixture, render_into, run, tool
from vmafx_api import emit_layout_test
from vmafx_api.layout import DATA_MODELS, layouts, models_agree
from vmafx_api.loader import parse

C_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"]


def compile_layout(compiler: str, root: Path) -> tuple[int, str]:
    source = root / "core/test/test_vmafx_abi_layout.c"
    binary = root / "layout_test"
    done = run([compiler, *C_FLAGS, f"-I{root / 'core/include'}", str(source), "-o", str(binary)])
    if done.returncode != 0:
        return done.returncode, done.stderr
    ran = run([str(binary)])
    return ran.returncode, ran.stdout


def import_binding(path: Path, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        del sys.modules[name]
    return module


class ComputedLayoutTest(unittest.TestCase):
    def test_nested_structs_and_arrays(self) -> None:
        lay = layouts(parse(fixture()))
        frame = {f.name: (f.offset, f.size) for f in lay["VmafxFrameImport"].fields}
        self.assertEqual(frame["plane"], (16, 96))
        self.assertEqual(frame["acquire"], (112, 32))
        self.assertEqual(frame["fences"], (152, 64))
        self.assertEqual(lay["VmafxFrameImport"].size, 216)
        self.assertEqual(lay["VmafxWindowRequest"].size, 80)

    def test_data_models_agree_and_a_differing_one_gets_its_branch(self) -> None:
        api = parse(fixture())
        self.assertTrue(models_agree(api))
        ilp32 = {
            k: ((4, 4) if k in ("ptr", "cstr", "uptr", "size") else v)
            for k, v in DATA_MODELS["LP64"].items()
        }
        with mock.patch.dict(DATA_MODELS, {"LLP64": ilp32}):
            self.assertFalse(models_agree(api))
            self.assertEqual(layouts(api, "LLP64")["VmafxWindowRequest"].size, 56)
            text = emit_layout_test.layout_test_source(api)
        self.assertIn("#if defined(_WIN64) /* LLP64 */", text)


@unittest.skipIf(tool("cc") is None, "no C compiler (cc) on PATH")
class CompiledLayoutTest(unittest.TestCase):
    def test_fixture_compiles_and_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            render_into(Path(tmp), parse(fixture()))
            status, output = compile_layout("cc", Path(tmp))
        self.assertEqual(status, 0, output)
        self.assertIn("5 structs, 33 fields, 8 constants", output)

    def test_planted_layout_defects_fail(self) -> None:
        for old, new, message in (
            ("VmafxFramePlane plane[3];", "VmafxFramePlane plane[4];", "holds 3 elements"),
            ("    int32_t fd;\n    /** Plane in", "    int64_t fd;\n    /** Plane in", "offset"),
        ):
            with self.subTest(defect=new), tempfile.TemporaryDirectory() as tmp:
                render_into(Path(tmp), parse(fixture()))
                header = Path(tmp) / "core/include/vmafx/frame.h"
                text = header.read_text()
                self.assertIn(old, text)
                header.write_text(text.replace(old, new))
                status, output = compile_layout("cc", Path(tmp))
                self.assertNotEqual(status, 0)
                self.assertIn(message, output)


@unittest.skipIf(tool("c++") is None, "no C++ compiler (c++) on PATH")
class CxxHeaderTest(unittest.TestCase):
    def test_headers_compile_as_cxx(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            render_into(Path(tmp), parse(fixture()))
            source = Path(tmp) / "use.cpp"
            source.write_text("#include <vmafx/vmafx.h>\n#include <vmafx/device_test.h>\n")
            done = run(
                [
                    "c++",
                    "-std=c++20",
                    "-Wall",
                    "-Werror",
                    "-fsyntax-only",
                    f"-I{tmp}/core/include",
                    str(source),
                ]
            )
        self.assertEqual(done.returncode, 0, done.stderr)


@unittest.skipIf(tool("clang-format") is None, "no clang-format on PATH")
class FormatTest(unittest.TestCase):
    def test_generated_c_is_clang_format_clean(self) -> None:
        style = f"--style=file:{ROOT / '.clang-format'}"
        with tempfile.TemporaryDirectory() as tmp:
            files = render_into(Path(tmp), parse(fixture()))
            for path in files:
                if not path.endswith((".c", ".h")):
                    continue
                done = run(["clang-format", style, str(Path(tmp) / path)])
                self.assertEqual(done.stdout, files[path], path)


class PythonBindingLayoutTest(unittest.TestCase):
    def test_fixture_binding_imports_and_refuses_a_planted_size(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            render_into(Path(tmp), parse(fixture()))
            path = Path(tmp) / "bindings/python/vmafx/_api.py"
            module = import_binding(path, "vmafx_fixture_api")
            self.assertEqual(module.ImportFlags.ALLOW_PLANAR_ONLY, 8)
            self.assertEqual(len(module.VmafxFrameImport().plane), 3)
            planted = path.with_name("_api_planted.py")  # a new file: no stale bytecode
            planted.write_text(
                path.read_text().replace(
                    "VmafxFrameImport: (\n        216,", "VmafxFrameImport: (\n        224,"
                )
            )
            with self.assertRaisesRegex(ImportError, "VmafxFrameImport: size 216 != 224"):
                import_binding(planted, "vmafx_fixture_api_planted")


if __name__ == "__main__":
    unittest.main()

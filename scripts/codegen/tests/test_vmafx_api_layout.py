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
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

import support
from support import ROOT, fixture, pinned_clang_format, render_into, run, tool
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


CLANG_FORMAT, NO_CLANG_FORMAT = pinned_clang_format()


@unittest.skipIf(CLANG_FORMAT is None, NO_CLANG_FORMAT)
class FormatTest(unittest.TestCase):
    def test_generated_c_is_clang_format_clean(self) -> None:
        assert CLANG_FORMAT is not None
        style = f"--style=file:{ROOT / '.clang-format'}"
        with tempfile.TemporaryDirectory() as tmp:
            files = render_into(Path(tmp), parse(fixture()))
            for path in files:
                if not path.endswith((".c", ".h")):
                    continue
                done = run([CLANG_FORMAT, style, str(Path(tmp) / path)])
                self.assertEqual(done.stdout, files[path], path)


class ClangFormatPinTest(unittest.TestCase):
    def test_pin_is_read_from_the_hook_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / "c.yaml"
            config.write_text(
                "  - repo: https://github.com/pre-commit/mirrors-clang-format\n    rev: v19.1.0\n"
            )
            self.assertEqual(support.clang_format_pin(config), 19)
            config.write_text("repos: []\n")
            with self.assertRaises(AssertionError):
                support.clang_format_pin(config)

    def test_the_tooling_lock_installs_the_hook_pin(self) -> None:
        """The Tooling Tests job runs this test with the lock's clang-format."""
        lock_input = (ROOT / "requirements" / "locks" / "tooling-tests.in").read_text("utf-8")
        hook = (ROOT / ".pre-commit-config.yaml").read_text("utf-8")
        rev = re.search(r"mirrors-clang-format\s*\n\s*rev:\s*v([\d.]+)", hook)
        assert rev is not None
        self.assertIn(f"clang-format=={rev.group(1)}\n", lock_input)

    def test_another_major_on_path_is_refused_with_its_version(self) -> None:
        major = support.clang_format_pin()
        with (
            mock.patch.dict("os.environ", {"VMAFX_CLANG_FORMAT": ""}),
            mock.patch.object(
                support, "tool", lambda name: "/opt/cf" if name == "clang-format" else None
            ),
            mock.patch.object(support, "clang_format_major", lambda _: major - 5),
        ):
            found, why = support.pinned_clang_format()
        self.assertIsNone(found)
        self.assertIn(f"clang-format {major}", why)
        self.assertIn(f"/opt/cf is {major - 5}", why)

    def test_the_pinned_major_is_used(self) -> None:
        major = support.clang_format_pin()
        with (
            mock.patch.dict("os.environ", {"VMAFX_CLANG_FORMAT": ""}),
            mock.patch.object(support, "tool", lambda name: f"/opt/{name}"),
            mock.patch.object(
                support,
                "clang_format_major",
                lambda path: major if path.endswith(f"-{major}") else 3,
            ),
        ):
            self.assertEqual(support.pinned_clang_format(), (f"/opt/clang-format-{major}", ""))

    def test_an_explicit_binary_of_another_major_fails(self) -> None:
        major = support.clang_format_pin()
        with (
            mock.patch.dict("os.environ", {"VMAFX_CLANG_FORMAT": "/opt/old"}),
            mock.patch.object(support, "clang_format_major", lambda _: major - 1),
            self.assertRaises(AssertionError),
        ):
            support.pinned_clang_format()


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

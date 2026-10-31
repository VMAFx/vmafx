#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Definition features on the every-feature fixture (fixtures/full.toml, ADR-1852).

Header split and computed includes, callbacks, flag sets, fixed arrays,
nested sized structs, handle / pointer / size fields, deprecation and option
groups: each is rendered, and each validation rule refuses its planted defect.
"""

from __future__ import annotations

import contextlib
import copy
import io
import os
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Any

import setuptools.build_meta as backend  # type: ignore[import-untyped]  # no stubs in the hook env
from support import ROOT, document, entry, fixture
from vmafx_api import emit_c, emit_python
from vmafx_api.headers import plan
from vmafx_api.loader import parse
from vmafx_api.model import Api, DefinitionError


def rendered(api: Api, path: str) -> str:
    item = next(p for p in plan(api) if p.header.path == path)
    text: str = emit_c.header_text(api, item)
    return text


class HeaderSplitTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(fixture())
        self.includes = {p.header.path: p.includes for p in plan(self.api)}

    def test_each_header_includes_what_its_declarations_use(self) -> None:
        self.assertEqual(self.includes["vmafx/frame.h"], ("vmafx/types.h", "vmafx/error.h"))
        self.assertIn("vmafx/score.h", self.includes["vmafx/device_test.h"])
        self.assertEqual(self.includes["vmafx/version.h"], ("vmafx/types.h",))

    def test_umbrella_includes_core_headers_only(self) -> None:
        umbrella = self.includes["vmafx/vmafx.h"]
        self.assertNotIn("vmafx/device_test.h", umbrella)
        self.assertEqual(
            set(umbrella),
            {"vmafx/version.h", "vmafx/types.h", "vmafx/error.h", "vmafx/frame.h", "vmafx/score.h"},
        )
        text = rendered(self.api, "vmafx/vmafx.h")
        self.assertNotIn("extern", text)

    def test_include_cycle_is_refused(self) -> None:
        doc = fixture()
        fence = entry(doc["structs"], "VmafxFence")
        fence["fields"].append({"name": "plane", "type": "VmafxFramePlane"})
        with self.assertRaisesRegex(DefinitionError, "header include cycle"):
            plan(parse(doc))


class StructFeatureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.api = parse(fixture())

    def test_arrays_nested_structs_and_pointer_fields(self) -> None:
        frame = rendered(self.api, "vmafx/frame.h")
        self.assertIn("    VmafxFramePlane plane[3];\n", frame)
        self.assertIn("    VmafxFence acquire;\n", frame)
        score = rendered(self.api, "vmafx/score.h")
        for decl in (
            "const VmafxModel *model;",
            "VmafxWindowCallback on_complete;",
            "void *user;",
            "size_t budget;",
            "double value[8];",
        ):
            self.assertIn(decl, score)

    def test_init_macro_sets_every_nested_struct_size(self) -> None:
        macro = emit_c.init_macro(self.api, self.api.struct("VmafxFrameImport"))
        self.assertIn(".acquire = VMAFX_FENCE_INIT", macro)
        self.assertIn(".fences = {VMAFX_FENCE_INIT, VMAFX_FENCE_INIT}", macro)
        self.assertIn("/* clang-format off */", macro)

    def assert_refused(self, doc: dict[str, Any], text: str) -> None:
        with self.assertRaisesRegex(DefinitionError, text):
            parse(doc)

    def test_by_value_cycle_is_refused(self) -> None:
        doc = fixture()
        entry(doc["structs"], "VmafxFence")["fields"].append(
            {"name": "back", "type": "VmafxFrameImport"}
        )
        self.assert_refused(doc, "by-value struct cycle")

    def test_sized_struct_inside_an_unsized_one_is_refused(self) -> None:
        doc = fixture()
        entry(doc["structs"], "VmafxFramePlane")["fields"].append(
            {"name": "fence", "type": "VmafxFence"}
        )
        self.assert_refused(doc, "unsized struct cannot embed sized")

    def test_field_rules(self) -> None:
        cases = (
            ({"name": "x", "type": "u32", "count": 65}, "count is 1..64"),
            ({"name": "x", "type": "u32", "const": True}, "`const` applies to handle"),
            ({"name": "x", "type": "VmafxFenceKind"}, "fields are fixed-width"),
            ({"name": "x", "type": "u64", "flags": "VmafxImportFlags"}, "flag set of type u64"),
            ({"name": "x", "type": "u64", "enum": "VmafxFenceKind"}, "`enum` needs type u32"),
        )
        for field, text in cases:
            doc = fixture()
            entry(doc["structs"], "VmafxFramePlane")["fields"].append(field)
            self.assert_refused(doc, text)


class CallbackFlagTest(unittest.TestCase):
    def assert_refused(self, doc: dict[str, Any], text: str) -> None:
        with self.assertRaisesRegex(DefinitionError, text):
            parse(doc)

    def test_callback_typedef(self) -> None:
        text = rendered(parse(fixture()), "vmafx/score.h")
        self.assertIn("typedef void (*VmafxWindowCallback)(VmafxWindow *window,", text)
        self.assertIn("void *user);", text)

    def test_callback_needs_a_trailing_user_pointer(self) -> None:
        doc = fixture()
        entry(doc["callbacks"], "VmafxWindowCallback")["params"].pop()
        self.assert_refused(doc, "last parameter is `user`")
        doc = fixture()
        entry(doc["callbacks"], "VmafxWindowCallback")["params"][0].update({"pass": "out_handle"})
        self.assert_refused(doc, "callback parameters are inputs")

    def test_flag_bits(self) -> None:
        text = rendered(parse(fixture()), "vmafx/frame.h")
        self.assertIn("#define VMAFX_IMPORT_ALLOW_PLANAR_ONLY (UINT32_C(1) << 3U)", text)
        doc = fixture()
        entry(doc["flags"], "VmafxImportFlags")["bits"][1]["bit"] = 32
        self.assert_refused(doc, "bit outside 0..31")
        doc = fixture()
        entry(doc["flags"], "VmafxImportFlags")["bits"][1]["bit"] = 0
        self.assert_refused(doc, "duplicate bit")

    def test_deprecated_function_carries_the_attribute(self) -> None:
        text = rendered(parse(fixture()), "vmafx/frame.h")
        expected = 'VMAFX_DEPRECATED("since 0.2; use vmafx_frame_import_check2; removal in 1.0")\nVMAFX_EXPORT'
        self.assertIn(expected, text)
        self.assertIn("@deprecated since 0.2", text)
        self.assertIn(
            "#define VMAFX_DEPRECATED(message)", rendered(parse(fixture()), "vmafx/types.h")
        )


class OptionGroupTest(unittest.TestCase):
    def assert_refused(self, change: tuple[str, int, str, object], text: str) -> None:
        group, index, key, value = change
        doc = copy.deepcopy(fixture())
        option = entry(doc["option_groups"], group)["options"][index]
        if value is None:
            del option[key]
        else:
            option[key] = value
        with self.assertRaisesRegex(DefinitionError, text):
            parse(doc)

    def test_fixture_groups_parse(self) -> None:
        groups = parse(fixture()).option_groups
        threads = groups[0].options[0]
        self.assertEqual(threads.spellings["ffmpeg"], ("threads", "n_threads"))
        self.assertEqual(threads.proto_field, 7)

    def test_option_rules(self) -> None:
        cases = (
            (("threads", 0, "mcp", None), "no `mcp` spelling"),
            (("threads", 0, "proto", None), "no `proto = { field = N }`"),
            (("threads", 0, "proto", {"field": 19500}), "not a usable field number"),
            (("threads", 0, "default", 300), "outside"),
            (("threads", 0, "cli", ["-t"]), "start with --"),
            (("pool", 0, "default", "median"), "not one of"),
            (("pool", 1, "ffmpeg", "pool"), "also used by"),
            (("pool", 1, "type", "bool"), "`range` is \\[min\\] or \\[min, max\\]"),
        )
        for change, text in cases:
            with self.subTest(change=change):
                self.assert_refused(change, text)


def python_record(text: str, name: str) -> str:
    start = text.index(f"class {name}:")
    return text[start : text.index("\n@dataclass", start)]


class PythonRecordToCTest(unittest.TestCase):
    """A record converts back to C when its fields are numbers or one struct of
    numbers: VmafxFrameDesc kept to_c() when it gained `color` (ADR-2094)."""

    def test_a_struct_of_numbers_converts_back(self) -> None:
        record = python_record(emit_python.module_text(parse(document())), "FrameDesc")
        self.assertIn("    def to_c(self) -> VmafxFrameDesc:\n", record)
        self.assertIn("        raw.color = self.color.to_c()\n", record)

    def test_a_struct_with_a_pointer_does_not(self) -> None:
        # VmafxFrameImport embeds VmafxFence, whose handle has no record value.
        record = python_record(emit_python.module_text(parse(document())), "FrameImport")
        self.assertNotIn("def to_c", record)


class PythonMethodNameTest(unittest.TestCase):
    """Two functions on one Python class with one method name: the later
    definition hid the earlier one (vmafx_context_feature_provenance behind
    vmafx_feature_provenance, T-VMAFX-PYTHON-METHOD-SHADOWED-2026-10-07)."""

    NAME = "vmafx_context_feature_provenance"

    def test_the_definition_binds_both_provenance_lookups(self) -> None:
        text = emit_python.module_text(parse(document()))
        self.assertIn("    def feature_provenance_at(self, index: int)", text)
        self.assertIn("    def feature_provenance(self, feature: str)", text)

    def test_a_collision_is_refused(self) -> None:
        doc = document()
        del entry(doc["functions"], self.NAME)["python"]
        with self.assertRaisesRegex(DefinitionError, "Context.feature_provenance would bind both"):
            emit_python.module_text(parse(doc))

    def test_python_name_must_be_an_identifier(self) -> None:
        for bad in ("feature-provenance", "class", 7):
            doc = document()
            entry(doc["functions"], self.NAME)["python"] = bad
            with self.assertRaises(DefinitionError):
                parse(doc)


def _refuse_compiled_wheel_entries(entries: list[str]) -> None:
    """Refuse a pure-Python wheel that contains any compiled native library."""
    compiled = [e for e in entries if e.endswith((".so", ".pyd", ".dylib"))]
    if compiled:
        raise ValueError(f"wheel contains compiled code: {', '.join(compiled)}")


class PythonWrapCoverageTest(unittest.TestCase):
    """Every function in core/api/vmafx.toml is wrapped or in NOT_WRAPPED (#2434)."""

    # Go and Rust currently wrap only the 23 prototype scalar/context functions
    # (`context_methods(api)` + `library_functions(api)` + create/destroy/libvmaf_handle
    # + 5 error functions). Python additionally wraps all 100 remaining host-data
    # and handle functions across Options, Device, Frame, Fence, FramePool,
    # FrameConverter, Model, ModelSet, ReportFile, Window, WindowClock, DnnSession,
    # McpServer, and Context (`HANDWRITTEN_WRAPPED`).
    PYTHON_EXTRA_WRAPPED = emit_python.HANDWRITTEN_WRAPPED - {
        "vmafx_error_message",
        "vmafx_error_subject",
        "vmafx_error_errno",
        "vmafx_error_free",
        "vmafx_context_create",
        "vmafx_context_destroy",
        "vmafx_context_libvmaf_handle",
    }

    def test_every_definition_function_is_wrapped_or_in_not_wrapped(self) -> None:
        api = parse(document())
        emit_python.check_coverage(api)
        wrapped = set(emit_python.wrapped_functions(api))
        skipped = set(emit_python.not_wrapped(api))
        all_names = [fn.name for fn in api.functions]
        self.assertEqual(wrapped & skipped, set())
        self.assertEqual(wrapped | skipped, set(all_names))
        self.assertEqual(
            list(emit_python.not_wrapped(api)),
            [n for n in all_names if n in skipped],
        )
        text = emit_python.module_text(api)
        for name in wrapped:
            self.assertIn(f".{name}(", text, f"{name} declared wrapped but not called in _api.py")

    def test_not_wrapped_matches_go_and_rust_plus_documented_python_extras(self) -> None:
        api = parse(document())
        go_rust_wrapped = (
            {fn.name for fn in emit_python.context_methods(api)}
            | {fn.name for fn in emit_python.library_functions(api)}
            | {
                "vmafx_error_message",
                "vmafx_error_subject",
                "vmafx_error_errno",
                "vmafx_error_free",
                "vmafx_context_create",
                "vmafx_context_destroy",
                "vmafx_context_libvmaf_handle",
            }
        )
        go_rust_not_wrapped = {fn.name for fn in api.functions} - go_rust_wrapped
        py_not_wrapped = set(emit_python.not_wrapped(api))
        self.assertTrue(py_not_wrapped.issubset(go_rust_not_wrapped))
        self.assertEqual(go_rust_not_wrapped - py_not_wrapped, self.PYTHON_EXTRA_WRAPPED)

    def test_planted_unaccounted_function_is_refused(self) -> None:
        doc = document()
        planted = copy.deepcopy(entry(doc["functions"], "vmafx_frame_import"))
        planted["name"] = "vmafx_frame_import_planted"
        doc["functions"].append(planted)
        with self.assertRaisesRegex(DefinitionError, "vmafx_frame_import_planted"):
            emit_python.check_coverage(parse(doc))


class PythonPackagingTest(unittest.TestCase):
    """bindings/python builds a pure py3-none-any wheel with no compiled artifacts (#2434)."""

    def test_wheel_is_py3_none_any_without_compiled_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_dir = Path(tmp) / "pkg"
            out_dir = Path(tmp) / "dist"
            out_dir.mkdir()
            shutil.copytree(ROOT / "bindings" / "python", src_dir)
            old_cwd = Path.cwd()
            os.chdir(src_dir)
            try:
                with (
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                ):
                    wheel_name = backend.build_wheel(str(out_dir))
            finally:
                os.chdir(old_cwd)
            self.assertTrue(wheel_name.endswith("-py3-none-any.whl"), wheel_name)
            with zipfile.ZipFile(out_dir / wheel_name) as archive:
                entries = archive.namelist()
            self.assertIn("vmafx/_api.py", entries)
            self.assertIn("vmafx/__init__.py", entries)
            self.assertIn("vmafx/py.typed", entries)
            _refuse_compiled_wheel_entries(entries)

    def test_planted_shared_object_in_wheel_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "vmafx/_planted.so"):
            _refuse_compiled_wheel_entries(["vmafx/__init__.py", "vmafx/_planted.so"])


if __name__ == "__main__":
    unittest.main()

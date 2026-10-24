#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Objective-C++ Metal host file closes what it opens.

The Metal host code (core/src/metal/*.mm, core/src/feature/metal/*.mm) is
compiled only on macOS, and the macOS legs are not required checks, so a
structural slip there lands on master unseen: #2222 opened a second anonymous
namespace in float_motion_metal.mm and closed one, and every macOS Metal build
stopped at `float_motion_metal.mm:869:19: error: expected '}'`
(T-METAL-FLOAT-MOTION-NAMESPACE-2026-10-06).

The test reads each file with comments, string and character literals removed
and requires the braces to balance, never to close below zero, and every
`namespace {` to have its `} // namespace`. It does not compile anything.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SOURCES = ("src/metal", "src/feature/metal")
TOKEN = re.compile(
    r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|@"(?:\\.|[^"\\\n])*\"""",
    re.S,
)


def code_of(text: str) -> str:
    """The text with comments and literals blanked out."""
    return TOKEN.sub(" ", text)


def balance_errors(name: str, text: str) -> list[str]:
    code = code_of(text)
    errors = []
    depth = 0
    for line_no, line in enumerate(code.splitlines(), start=1):
        for ch in line:
            depth += {"{": 1, "}": -1}.get(ch, 0)
            if depth < 0:
                errors.append(f"{name}:{line_no}: '}}' closes more than was opened")
                depth = 0
    if depth:
        errors.append(f"{name}: {depth} '{{' never closed")
    opened = len(re.findall(r"\bnamespace\s*\{", code))
    closed = len(re.findall(r"\}\s*//\s*namespace\b", text))
    if opened != closed:
        errors.append(f"{name}: {opened} 'namespace {{' but {closed} '}} // namespace'")
    return errors


def metal_host_files(root: Path) -> list[Path]:
    return sorted(p for sub in SOURCES for p in (root / sub).glob("*.mm"))


SHARED_BUFFER_STEPS = (
    "if (ctx == nullptr)",
    "*rc = -EINVAL;",
    "if (bytes == 0)",
    "vmaf_metal_context_device_handle(ctx)",
    "if (device_handle == nullptr)",
    "*rc = -ENODEV;",
    "(__bridge id<MTLDevice>)device_handle",
    "newBufferWithLength:bytes",
    "MTLResourceStorageModeShared",
    "if (buf == nil)",
    "*rc = -ENOMEM;",
    "*rc = 0;",
    "return buf;",
)


def extract_function_body(code: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^)]*\)\s*\{{", code)
    if not match:
        return ""
    pos = match.end()
    depth = 1
    while pos < len(code) and depth > 0:
        depth += {"{": 1, "}": -1}.get(code[pos], 0)
        pos += 1
    return code[match.start() : pos]


def shared_buffer_helper_errors(
    objc_handle: str, picture_mm: str, kernel_template_mm: str
) -> list[str]:
    helper = extract_function_body(code_of(objc_handle), "vmaf_metal_shared_buffer_alloc")
    if not helper:
        return ["objc_handle.h: missing vmaf_metal_shared_buffer_alloc"]
    errors = []
    cursor = 0
    for step in SHARED_BUFFER_STEPS:
        idx = helper.find(step, cursor)
        if idx < 0:
            errors.append(f"objc_handle.h: missing or out-of-order '{step}'")
        else:
            cursor = idx + len(step)
    for filename, src, fn_name in (
        ("picture_metal.mm", picture_mm, "vmaf_metal_picture_alloc"),
        ("kernel_template.mm", kernel_template_mm, "vmaf_metal_kernel_buffer_alloc"),
    ):
        body = extract_function_body(code_of(src), fn_name)
        if "vmaf_metal_shared_buffer_alloc(" not in body:
            errors.append(f"{filename}: {fn_name} does not call vmaf_metal_shared_buffer_alloc")
        if "newBufferWithLength:" in body or "vmaf_metal_context_device_handle(" in body:
            errors.append(f"{filename}: {fn_name} duplicates shared buffer allocation")
    return errors


class MetalHostSourceBalance(unittest.TestCase):
    def test_every_metal_host_file_balances(self) -> None:
        files = metal_host_files(CORE)
        self.assertTrue(files, "no Metal host file found")
        errors = []
        for path in files:
            errors += balance_errors(path.relative_to(CORE).as_posix(), path.read_text("utf-8"))
        self.assertEqual(errors, [])

    def test_an_unclosed_namespace_is_reported(self) -> None:
        # master's float_motion_metal.mm in miniature.
        text = "namespace {\nstruct S { int a; };\n\nnamespace {\nint f() { return 0; }\n} // namespace\n"
        self.assertEqual(
            balance_errors("x.mm", text),
            ["x.mm: 1 '{' never closed", "x.mm: 2 'namespace {' but 1 '} // namespace'"],
        )

    def test_braces_in_comments_and_literals_do_not_count(self) -> None:
        text = "int f() { /* { */ const char *s = \"}\"; char c = '{'; return 0; } // }\n"
        self.assertEqual(balance_errors("y.mm", text), [])

    def test_shared_buffer_helper_consolidates_picture_and_kernel_alloc(self) -> None:
        metal_dir = CORE / "src" / "metal"
        self.assertEqual(
            shared_buffer_helper_errors(
                (metal_dir / "objc_handle.h").read_text("utf-8"),
                (metal_dir / "picture_metal.mm").read_text("utf-8"),
                (metal_dir / "kernel_template.mm").read_text("utf-8"),
            ),
            [],
        )

    def test_shared_buffer_helper_contract_rejects_drift_or_duplication(self) -> None:
        metal_dir = CORE / "src" / "metal"
        objc_handle = (metal_dir / "objc_handle.h").read_text("utf-8")
        picture_mm = (metal_dir / "picture_metal.mm").read_text("utf-8")
        kernel_mm = (metal_dir / "kernel_template.mm").read_text("utf-8")
        broken_helper = objc_handle.replace("*rc = -ENODEV;", "*rc = -EINVAL;", 1)
        self.assertIn(
            "objc_handle.h: missing or out-of-order '*rc = -ENODEV;'",
            shared_buffer_helper_errors(broken_helper, picture_mm, kernel_mm),
        )
        duplicated_caller = picture_mm.replace(
            "vmaf_metal_shared_buffer_alloc(ctx, size, &rc)",
            "[device newBufferWithLength:size options:MTLResourceStorageModeShared]",
        )
        self.assertEqual(
            shared_buffer_helper_errors(objc_handle, duplicated_caller, kernel_mm),
            [
                "picture_metal.mm: vmaf_metal_picture_alloc does not call vmaf_metal_shared_buffer_alloc",
                "picture_metal.mm: vmaf_metal_picture_alloc duplicates shared buffer allocation",
            ],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

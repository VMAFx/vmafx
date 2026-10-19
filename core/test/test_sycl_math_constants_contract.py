#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every icpx compile line carries the Windows math-constant define.

Since Netflix/vmaf 4e150067b no translation unit defines M_PI or M_E: on
Windows they come from `-D_USE_MATH_DEFINES`, which core/meson.build passes as
a project argument. icpx compiles the SYCL translation units in custom
targets, and project arguments never reach a custom target's command line, so
the Windows SYCL build failed on `M_PI` in barten_csf_tools.h and
integer_adm_sycl.cpp (T-SYCL-WINDOWS-M-PI-UNDECLARED-2026-10-09). The define
is one list, `vmaf_math_constant_args`, set in the Windows branch of
core/meson.build and added to the two SYCL argument lists of
core/src/meson.build. This test keeps that wiring:

- core/meson.build spells `-D_USE_MATH_DEFINES` once, in that list, and passes
  the list as a project argument (C and C++) and to the header checks;
- `sycl_common_args` and `sycl_feature_tail_args` add the list;
- every icpx command line in core/src/meson.build and core/test/meson.build
  takes one of the two lists, except the device links
  (`sycl_device_link_args`), which compile nothing.

Device-free: reads the build files only. The planted cases below are the
defect and its near misses.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOP = ROOT / "core" / "meson.build"
SRC = ROOT / "core" / "src" / "meson.build"
TEST = ROOT / "core" / "test" / "meson.build"

LIST = "vmaf_math_constant_args"
DEFINE = "'-D_USE_MATH_DEFINES'"
SYCL_LISTS = ("sycl_common_args", "sycl_feature_tail_args")
COMMENT = re.compile(r"#[^\n]*")


def code(text: str) -> str:
    """The build file without comments: prose may name the define."""
    return COMMENT.sub("", text)


def assignment(text: str, var: str) -> str:
    """`var = ...` with its backslash continuation lines, or ''."""
    match = re.search(rf"^[ \t]*{var} = (?:[^\n]*\\[ \t]*\n)*[^\n]*", code(text), re.M)
    return match.group(0) if match else ""


def icpx_compile_commands(text: str) -> list[str]:
    """Each `command : ... [icpx] ...` argument other than a device link, up to
    the next keyword argument or the end of the call."""
    commands = []
    for match in re.finditer(r"command\s*:(.*?)(?=\n\s*\w+\s*:|\n\s*\))", code(text), re.S):
        command = match.group(1)
        if "[icpx]" in command and "sycl_device_link_args" not in command:
            commands.append(command)
    return commands


def top_failures(text: str) -> list[str]:
    body = code(text)
    failures = []
    if body.count(DEFINE) != 1:
        failures.append(f"core/meson.build spells {DEFINE} {body.count(DEFINE)} times, not once")
    if not re.search(rf"^[ \t]*{LIST} = \[\]", body, re.M):
        failures.append(f"core/meson.build: {LIST} has no empty default for other hosts")
    if not re.search(rf"^[ \t]*{LIST} = \[{re.escape(DEFINE)}\]", body, re.M):
        failures.append(f"core/meson.build: {LIST} does not hold {DEFINE}")
    if not re.search(rf"add_project_arguments\({LIST},\s*language:\s*\['c',\s*'cpp'\]\)", body):
        failures.append(f"core/meson.build: {LIST} is not a C and C++ project argument")
    if not re.search(rf"test_args \+= {LIST}\b", body):
        failures.append(f"core/meson.build: the header checks do not see {LIST}")
    return failures


def sycl_failures(src: str, test: str) -> list[str]:
    failures = []
    for var in SYCL_LISTS:
        if LIST not in assignment(src, var):
            failures.append(f"core/src/meson.build: {var} does not add {LIST}")
    for name, text in (("core/src/meson.build", src), ("core/test/meson.build", test)):
        commands = icpx_compile_commands(text)
        if not commands:
            failures.append(f"{name}: no icpx compile command found: parser stale?")
        for command in commands:
            if not any(var in command for var in SYCL_LISTS):
                line = " ".join(command.split())[:80]
                failures.append(f"{name}: icpx compiles without a SYCL argument list: {line}")
    return failures


def contract_failures(top: str, src: str, test: str) -> list[str]:
    return top_failures(top) + sycl_failures(src, test)


class SyclMathConstantsContract(unittest.TestCase):
    def sources(self) -> tuple[str, str, str]:
        return tuple(p.read_text(encoding="utf-8") for p in (TOP, SRC, TEST))

    def assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_build_files_satisfy_the_contract(self) -> None:
        self.assertEqual(contract_failures(*self.sources()), [])

    def test_compile_commands_are_found(self) -> None:
        _top, src, test = self.sources()
        self.assertGreaterEqual(len(icpx_compile_commands(src)), 2)
        self.assertGreaterEqual(len(icpx_compile_commands(test)), 1)

    def test_feature_list_without_the_define_is_detected(self) -> None:
        # The defect: the feature TUs' list without the define.
        top, src, test = self.sources()
        planted = src.replace(
            "+ ['-fpermissive'] \\\n        + vmaf_math_constant_args", "+ ['-fpermissive']", 1
        )
        self.assertNotEqual(planted, src)
        self.assert_detected(contract_failures(top, planted, test), "sycl_feature_tail_args")

    def test_common_list_without_the_define_is_detected(self) -> None:
        top, src, test = self.sources()
        planted = src.replace(
            "+ sycl_pic_arg \\\n        + vmaf_math_constant_args", "+ sycl_pic_arg", 1
        )
        self.assertNotEqual(planted, src)
        self.assert_detected(contract_failures(top, src=planted, test=test), "sycl_common_args")

    def test_compile_line_without_a_list_is_detected(self) -> None:
        top, src, test = self.sources()
        planted = test.replace("+ sycl_feature_tail_args + ['@INPUT@'", "+ ['@INPUT@'", 1)
        self.assertNotEqual(planted, test)
        self.assert_detected(contract_failures(top, src, planted), "without a SYCL argument list")

    def test_literal_define_beside_the_list_is_detected(self) -> None:
        top, src, test = self.sources()
        planted = top.replace(
            f"add_project_arguments({LIST}, language: ['c', 'cpp'])",
            f"add_project_arguments({DEFINE}, language: ['c', 'cpp'])",
            1,
        )
        self.assertNotEqual(planted, top)
        failures = contract_failures(planted, src, test)
        self.assert_detected(failures, "times, not once")
        self.assert_detected(failures, "not a C and C++ project argument")


if __name__ == "__main__":
    unittest.main()

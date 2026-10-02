#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""ADR-1467: ciede.c's powf() calls are calls of the C library.

clang replaces ``powf(x, 2)`` by ``x * x``; GCC emits the call, and glibc's
``powf`` is not correctly rounded, so the two forms return different floats
on some arguments. ``core/src/meson.build`` therefore builds ciede.c in a
library of its own with ``vmaf_libm_call_args``, which keeps the call under
clang and leaves GCC's command as it was. icx folds the call as well and is
left alone: it links Intel's math library, so no flag makes its build agree
with a GCC build.

This test pins that arrangement without a compiler: the policy per compiler
id, the one library that holds ciede.c, and the tests that are built with the
same list. With ``VMAF_LIBM_CALL_BUILD_ROOT`` set (Meson sets it) it also
reads the compile command this build really uses for ciede.c.
``test_ciede_powf_call`` and ``test_ciede_device_math`` check the built code.
"""

from __future__ import annotations

import json
import os
import textwrap
import unittest
from pathlib import Path

from test_strict_fp_compiler_args import (
    MESON_COMMAND,
    SOURCE_MESON,
    TEST_MESON,
    _marked_block,
    _meson_code,
    _meson_list,
    _meson_setup,
    _target_block,
)

POLICY_BEGIN = "# BEGIN VMAF libm call policy"
POLICY_END = "# END VMAF libm call policy"
KEEP_CALL = ["-fno-builtin-powf"]

# Compiler id -> vmaf_libm_call_args. clang and Apple's clang fold the call
# and share their C library with a GCC build; icx has its own math library.
COMPILER_MATRIX: dict[str, list[str]] = {
    "gcc": [],
    "clang": KEEP_CALL,
    "apple-clang": KEEP_CALL,
    "intel-llvm": [],
    "msvc": [],
    "clang-cl": [],
    "intel-llvm-cl": [],
}
FOLDING_COMPILERS = tuple(cid for cid, args in COMPILER_MATRIX.items() if args)

# Test executables that must be built with the library's list: one includes
# ciede.c, the other replays its arithmetic against the library.
TESTS_WITH_THE_LIST = ("test_ciede_powf_call", "test_ciede_device_math")


def _executable_block(source: str, target: str) -> str:
    """The `target = executable(...)` call, up to its closing parenthesis."""
    start = source.index(f"{target} = executable(")
    depth = 0
    for position in range(source.index("(", start), len(source)):
        depth += {"(": 1, ")": -1}.get(source[position], 0)
        if depth == 0:
            return source[start : position + 1]
    raise AssertionError(f"{target}: unbalanced executable() call")


def _ciede_command(build_root: Path) -> list[str]:
    """The arguments this build compiles the library's ciede.c with."""
    commands = json.loads((build_root / "compile_commands.json").read_text(encoding="utf-8"))
    found = [
        entry
        for entry in commands
        if entry["file"].replace("\\", "/").endswith("src/feature/ciede.c")
    ]
    if len(found) != 1:
        raise AssertionError(f"ciede.c is compiled {len(found)} times, expected once")
    entry = found[0]
    if "arguments" in entry:
        return list(entry["arguments"])
    return str(entry["command"]).split()


def _c_compiler_id(build_root: Path) -> str:
    info = json.loads(
        (build_root / "meson-info" / "intro-compilers.json").read_text(encoding="utf-8")
    )
    return str(info["host"]["c"]["id"])


class CiedeLibmCallArgsTest(unittest.TestCase):
    @unittest.skipUnless(MESON_COMMAND, "Meson is not installed")
    def test_policy_per_compiler(self) -> None:
        policy = _marked_block(POLICY_BEGIN, POLICY_END, "libm call").replace(
            "_libm_call_compiler_id = cc.get_id()",
            "_libm_call_compiler_id = libm_call_fixture_compiler_id",
        )
        self.assertNotIn("cc.", _meson_code(policy))
        for compiler_id, args in COMPILER_MATRIX.items():
            with self.subTest(compiler_id=compiler_id):
                fixture = textwrap.dedent(
                    f"""\
                    project('libm-call-args-{compiler_id}', 'c')
                    libm_call_fixture_compiler_id = '{compiler_id}'

                    {policy}

                    assert(vmaf_libm_call_args == {_meson_list(args)},
                           'wrong libm call arguments for {compiler_id}')
                    """
                )
                _meson_setup(self, compiler_id, fixture)

    def test_one_library_holds_ciede_and_names_the_list(self) -> None:
        code = _meson_code(SOURCE_MESON.read_text(encoding="utf-8"))
        self.assertEqual(code.count("feature_src_dir + 'ciede.c'"), 1)
        block = _target_block(code, "libvmaf_ciede_static_lib")
        self.assertIn("feature_src_dir + 'ciede.c'", block)
        self.assertIn("vmaf_strict_fp_args", block)
        self.assertIn("vmaf_libm_call_args", block)
        self.assertNotIn("-fno-builtin", block)
        feature = _target_block(code, "libvmaf_feature_static_lib")
        self.assertIn("libvmaf_ciede_static_lib.extract_all_objects(", feature)
        # The flag is spelled once, in the policy.
        self.assertEqual(code.count("'-fno-builtin-powf'"), 1)

    def test_ciede_tests_are_built_with_the_list(self) -> None:
        code = _meson_code(TEST_MESON.read_text(encoding="utf-8"))
        for target in TESTS_WITH_THE_LIST:
            with self.subTest(target=target):
                block = _executable_block(code, target)
                self.assertIn("vmaf_strict_fp_args + vmaf_libm_call_args", block)
        self.assertNotIn("-fno-builtin", code)

    def test_this_build_compiles_ciede_with_the_policy(self) -> None:
        root = os.environ.get("VMAF_LIBM_CALL_BUILD_ROOT", "")
        if not root:
            self.skipTest("VMAF_LIBM_CALL_BUILD_ROOT is not set (run through meson test)")
        build_root = Path(root)
        compiler_id = _c_compiler_id(build_root)
        if compiler_id not in COMPILER_MATRIX:
            self.skipTest(f"no libm call policy is stated for the compiler '{compiler_id}'")
        command = _ciede_command(build_root)
        folds = compiler_id in FOLDING_COMPILERS
        self.assertEqual(
            "-fno-builtin-powf" in command,
            folds,
            msg=f"{compiler_id}: ciede.c is compiled with {command}",
        )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the RGB conversion's shared arithmetic (ADR-2146).

`core/src/vmafx/rgb_math.h` is compiled by the CPU reference, nvcc, hipcc and the
SYCL kernels. A SYCL kernel must be free of scratch memory (ADR-1395) and of fp64
(ADR-0220), and every twin must return the reference's integers.

Positive: the header compiles alone as C and as C++ with every warning an error,
and contains only the integer expression (no floating-point type, no division or
modulo, no array indexed by a run-time value). Negative: the same scan refuses a
planted `double`, a planted division and a planted run-time index. Boundary: the
device kernels pass the plan by value and include the header with the function
qualifier; nothing else defines the expression.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "core/src/vmafx/rgb_math.h"
KERNELS = ROOT / "core/src/vmafx/import_convert_kernels.h"
FRAME_IMPORT = ROOT / "core/src/vmafx/frame_import.c"
TIMEOUT = 120
C_STD_CANDIDATES = ("-std=c23", "-std=c2x", "-std=c17")
BANNED = (
    (r"\b(float|double|long\s+double)\b", "floating-point type"),
    (r"(?<![/*])/(?![/*])", "division"),
    (r"%", "modulo"),
    (r"\[\s*(plane|p|i|k|x|y)\s*\]", "array indexed by a run-time value"),
)


def code(text: str) -> str:
    """The text without comments and string literals."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"[^"\n]*"', '""', text)


def findings(text: str) -> list[str]:
    return [what for pattern, what in BANNED if re.search(pattern, code(text))]


def _slice_between(text: str, start_marker: str, end_marker: str) -> str:
    start = text.find(start_marker)
    end = text.find(end_marker, start) if start >= 0 else -1
    return text[start:end] if start >= 0 and end > start else ""


def frame_import_findings(text: str) -> list[str]:
    stripped = code(text)
    fill_body = _slice_between(stripped, "static int fill_plane", "static int convert_planes")
    convert_body = _slice_between(
        stripped, "static int convert_planes", "static VmafxStatus bind_frame"
    )
    bind_body = _slice_between(
        stripped, "static VmafxStatus bind_frame", "static VmafxStatus import_on_cpu"
    )
    rc_mapping = re.search(
        r"rc\s*==\s*-1\s*\?\s*VMAFX_FAIL\s*\(\s*report\s*,\s*VMAFX_E_NOMEM\b[^:;]*:"
        r"\s*VMAFX_FAIL\s*\(\s*report\s*,\s*VMAFX_E_INVALID\b",
        bind_body,
    )
    checks = (
        ("VmafxRgbPlan rgb = {0};" in fill_body, "rgb plan not zero-initialized"),
        ("(void)planned" not in fill_body, "vmafx_rgb_plan_init return cast to void"),
        (
            not re.search(r"\bassert\s*\([^;]*\bplanned\b", fill_body),
            "fill_plane asserts on planned before returning error",
        ),
        ("if (!planned || !dst)" in fill_body, "fill_plane does not return on failed plan"),
        (
            "aligned_free(buffer);" in convert_body and "return -2;" in convert_body,
            "convert_planes leaks aligned buffer on fill_plane failure",
        ),
        (
            bool(rc_mapping),
            "bind_frame does not map rc == -1 to VMAFX_E_NOMEM and rc == -2 to VMAFX_E_INVALID",
        ),
    )
    return [msg for ok, msg in checks if not ok]


def resolve_compiler_path(candidate: str | None) -> str | None:
    if not candidate or not Path(candidate).is_absolute():
        return None
    resolved = Path(candidate).resolve()
    if not (resolved.is_absolute() and resolved.is_file() and os.access(resolved, os.X_OK)):
        return None
    return str(resolved)


def is_gnu_gcc(exe: str) -> bool:
    version = subprocess.run(  # noqa: S603 # nosec B603 -- resolved compiler, fixed argv
        [exe, "--version"],
        capture_output=True,
        text=True,
        check=False,
        shell=False,
        timeout=TIMEOUT,
    )
    if (
        version.returncode != 0
        or not re.search(r"\bgcc\b", version.stdout, re.IGNORECASE)
        or "clang" in version.stdout.lower()
    ):
        return False
    probe = subprocess.run(  # noqa: S603 # nosec B603 -- resolved compiler, fixed argv
        [exe, "-dM", "-E", "-x", "c", "-"],
        input="",
        capture_output=True,
        text=True,
        check=False,
        shell=False,
        timeout=TIMEOUT,
    )
    return (
        probe.returncode == 0
        and "#define __GNUC__" in probe.stdout
        and "#define __clang__" not in probe.stdout
    )


def find_gnu_gcc() -> str | None:
    for name in ("gcc", "cc"):
        exe = resolve_compiler_path(shutil.which(name))
        if exe is not None and is_gnu_gcc(exe):
            return exe
    return None


def probe_c_std(gcc: str) -> str:
    exe = resolve_compiler_path(gcc)
    if exe is None:
        raise ValueError(f"compiler path must be an absolute executable file: {gcc!r}")
    for flag in C_STD_CANDIDATES:
        probe = subprocess.run(  # noqa: S603 # nosec B603 -- resolved compiler, fixed argv
            [exe, flag, "-x", "c", "-E", "-"],
            input="",
            capture_output=True,
            text=True,
            check=False,
            shell=False,
            timeout=TIMEOUT,
        )
        if probe.returncode == 0:
            return flag
    return "-std=c2x"


def compile_frame_import(
    gcc: str, source_text: str, c_std: str | None = None
) -> subprocess.CompletedProcess[str]:
    exe = resolve_compiler_path(gcc)
    if exe is None or not is_gnu_gcc(exe):
        raise ValueError(f"compiler must be a resolved GNU GCC executable: {gcc!r}")
    std_flag = c_std if c_std in C_STD_CANDIDATES else probe_c_std(exe)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "config.h").write_text("", encoding="utf-8")
        (t / "libvmaf").mkdir()
        (t / "libvmaf" / "version.h").write_text("", encoding="utf-8")
        src = t / "frame_import.c"
        src.write_text(source_text, encoding="utf-8")
        return subprocess.run(  # noqa: S603 # nosec B603 -- resolved compiler, fixed argv
            [
                exe,
                std_flag,
                "-O2",
                "-DNDEBUG",
                "-fno-lto",
                "-Wall",
                "-Wextra",
                "-Werror=maybe-uninitialized",
                "-DVMAF_BUILDING_LIBVMAF",
                f"-I{t}",
                f"-I{ROOT}/core/include",
                f"-I{ROOT}/core/src",
                f"-I{ROOT}/core/src/vmafx",
                "-c",
                str(src),
                "-o",
                str(t / "frame_import.o"),
            ],
            capture_output=True,
            text=True,
            check=False,
            shell=False,
            timeout=TIMEOUT,
            env={**os.environ, "LC_ALL": "C"},
        )


class RgbMathContract(unittest.TestCase):
    def test_header_is_the_integer_expression_only(self) -> None:
        self.assertEqual(findings(HEADER.read_text(encoding="utf-8")), [])

    def test_scan_refuses_planted_defects(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        planted = {
            "floating-point type": text.replace("int64_t t =", "double t =", 1),
            "division": text.replace(">> VMAFX_RGB_SHIFT", "/ 1073741824", 1),
            "array indexed by a run-time value": text.replace(
                "plan->coef[1][0]", "plan->coef[plane][0]", 1
            ),
        }
        for what, mutated in planted.items():
            self.assertNotEqual(mutated, text, what)
            self.assertIn(what, findings(mutated), what)

    def test_header_compiles_alone(self) -> None:
        compilers = [("cc", "c", "-std=c11"), ("c++", "c++", "-std=c++17")]
        for tool, lang, std in compilers:
            exe = resolve_compiler_path(shutil.which(tool))
            if exe is None:
                continue
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / f"t.{'c' if lang == 'c' else 'cpp'}"
                src.write_text('#include "vmafx/rgb_math.h"\nint main(void) { return 0; }\n')
                done = subprocess.run(  # noqa: S603 # nosec B603 -- resolved compiler, fixed argv
                    [
                        exe,
                        std,
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pedantic",
                        f"-I{ROOT}/core/src",
                        "-x",
                        lang,
                        str(src),
                        "-o",
                        str(Path(tmp) / "t"),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                    shell=False,
                    timeout=TIMEOUT,
                )
                self.assertEqual(done.returncode, 0, done.stderr)

    def test_kernels_use_the_shared_expression(self) -> None:
        text = KERNELS.read_text(encoding="utf-8")
        self.assertIn("#define VMAFX_RGB_FN __device__ __forceinline__", text)
        self.assertIn('#include "vmafx/rgb_math.h"', text)
        self.assertIn("VmafxRgbPlan rgb)", text)  # the plan by value
        self.assertIn("vmafx_rgb_value(&rgb, plane, r, g, b)", text)
        self.assertNotIn("coef", code(text))  # no second copy of the expression

    def test_frame_import_handles_rgb_plan_failure(self) -> None:
        text = FRAME_IMPORT.read_text(encoding="utf-8")
        self.assertEqual(frame_import_findings(text), [])
        mutations = (
            ("VmafxRgbPlan rgb = {0};", "VmafxRgbPlan rgb;", "rgb plan not zero-initialized"),
            (
                "if (!planned || !dst) {\n            return -1;\n        }",
                "(void)planned;",
                "vmafx_rgb_plan_init return cast to void",
            ),
            (
                "if (!planned || !dst) {",
                "assert(planned && dst != NULL);\n        if (!planned || !dst) {",
                "fill_plane asserts on planned before returning error",
            ),
            (
                "aligned_free(buffer);",
                "",
                "convert_planes leaks aligned buffer on fill_plane failure",
            ),
            (
                "return rc == -1 ?\n"
                '                   VMAFX_FAIL(report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FRAME, "frame",\n'
                '                              "cannot allocate a %ux%u frame", (unsigned)d->w, (unsigned)d->h) :\n'
                "                   VMAFX_FAIL(report, VMAFX_E_INVALID,",
                "return rc == -1 ?\n"
                '                   VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FRAME, "frame",\n'
                '                              "cannot allocate a %ux%u frame", (unsigned)d->w, (unsigned)d->h) :\n'
                "                   VMAFX_FAIL(report, VMAFX_E_NOMEM,",
                "bind_frame does not map rc == -1 to VMAFX_E_NOMEM and rc == -2 to VMAFX_E_INVALID",
            ),
        )
        for old, new, expected in mutations:
            mutated = text.replace(old, new, 1)
            self.assertNotEqual(mutated, text, expected)
            self.assertIn(expected, frame_import_findings(mutated))

    def test_compiler_resolution_rejects_non_absolute_and_non_gcc(self) -> None:
        self.assertIsNone(resolve_compiler_path("gcc"))
        self.assertIsNone(resolve_compiler_path("./gcc"))
        with (
            mock.patch.object(shutil, "which", return_value="relative/gcc"),
            mock.patch.object(subprocess, "run") as run_mock,
        ):
            self.assertIsNone(find_gnu_gcc())
            with self.assertRaises(unittest.SkipTest):
                self.test_frame_import_compiles_under_ndebug_no_lto_maybe_uninitialized()
            run_mock.assert_not_called()
        non_gcc = subprocess.CompletedProcess(["/bin/true", "--version"], 0, "true 9.0\n", "")
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(subprocess, "run", return_value=non_gcc) as run_mock,
        ):
            fake = Path(tmp) / "fake-cc"
            fake.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake.chmod(0o500)
            with mock.patch.object(shutil, "which", return_value=str(fake)):
                self.assertIsNone(find_gnu_gcc())
                with self.assertRaises(unittest.SkipTest):
                    self.test_frame_import_compiles_under_ndebug_no_lto_maybe_uninitialized()
            self.assertEqual(run_mock.call_count, 4)
        with self.assertRaises(ValueError):
            compile_frame_import("gcc", "int x;\n")

    def test_c_std_probe_falls_back_to_c2x(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "gcc"
            fake.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake.chmod(0o500)
            exe = str(fake.resolve())

            def accept_c23(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(argv, 0 if "-std=c23" in argv else 1, "", "")

            def gcc12_stub(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                if "--version" in argv:
                    return subprocess.CompletedProcess(argv, 0, "gcc (GCC) 12.3.0\n", "")
                if "-dM" in argv:
                    return subprocess.CompletedProcess(argv, 0, "#define __GNUC__ 12\n", "")
                return subprocess.CompletedProcess(argv, 1 if "-std=c23" in argv else 0, "", "")

            with mock.patch.object(subprocess, "run", side_effect=accept_c23):
                self.assertEqual(probe_c_std(exe), "-std=c23")
            with mock.patch.object(subprocess, "run", side_effect=gcc12_stub):
                self.assertEqual(probe_c_std(exe), "-std=c2x")
                done = compile_frame_import(exe, "int x;\n")
                self.assertEqual(done.returncode, 0)

    def test_frame_import_compiles_under_ndebug_no_lto_maybe_uninitialized(self) -> None:
        gcc = find_gnu_gcc()
        if gcc is None:
            self.skipTest("GNU gcc not on PATH; source contract checked above")
        text = FRAME_IMPORT.read_text(encoding="utf-8")
        done = compile_frame_import(gcc, text)
        self.assertEqual(done.returncode, 0, done.stderr)
        planted = text.replace("VmafxRgbPlan rgb = {0};", "VmafxRgbPlan rgb;", 1).replace(
            "if (!planned || !dst) {\n            return -1;\n        }",
            "(void)planned;",
            1,
        )
        self.assertNotEqual(planted, text)
        bad = compile_frame_import(gcc, planted)
        self.assertNotEqual(
            bad.returncode, 0, "unfixed fill_plane must fail -Werror=maybe-uninitialized"
        )
        self.assertIn("maybe-uninitialized", bad.stderr)


if __name__ == "__main__":
    sys.exit(unittest.main())

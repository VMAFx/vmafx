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

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "core/src/vmafx/rgb_math.h"
KERNELS = ROOT / "core/src/vmafx/import_convert_kernels.h"
FRAME_IMPORT = ROOT / "core/src/vmafx/frame_import.c"
TIMEOUT = 120
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


def frame_import_findings(text: str) -> list[str]:
    stripped = code(text)
    bind_at = stripped.find("static VmafxStatus bind_frame")
    bind_end = stripped.find("static VmafxStatus import_on_cpu", bind_at)
    bind_body = stripped[bind_at:bind_end] if bind_at >= 0 and bind_end > bind_at else ""
    checks = (
        ("VmafxRgbPlan rgb = {0};" in stripped, "rgb plan not zero-initialized"),
        ("(void)planned" not in stripped, "vmafx_rgb_plan_init return cast to void"),
        ("if (!planned || !dst)" in stripped, "fill_plane does not return on failed plan"),
        (
            "aligned_free(buffer);" in stripped,
            "convert_planes leaks aligned buffer on fill_plane failure",
        ),
        ("VMAFX_E_INVALID" in bind_body, "bind_frame does not map plan failure to VMAFX_E_INVALID"),
    )
    return [msg for ok, msg in checks if not ok]


def find_gnu_gcc() -> str | None:
    for name in ("gcc", "cc"):
        exe = shutil.which(name)
        if exe is None:
            continue
        probe = subprocess.run(  # noqa: S603 -- resolved compiler, fixed argv
            [exe, "-dM", "-E", "-x", "c", "-"],
            input="",
            capture_output=True,
            text=True,
            check=False,
            timeout=TIMEOUT,
        )
        if "#define __GNUC__" in probe.stdout and "#define __clang__" not in probe.stdout:
            return exe
    return None


def compile_frame_import(gcc: str, source_text: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        (t / "config.h").write_text("", encoding="utf-8")
        (t / "libvmaf").mkdir()
        (t / "libvmaf" / "version.h").write_text("", encoding="utf-8")
        src = t / "frame_import.c"
        src.write_text(source_text, encoding="utf-8")
        return subprocess.run(  # noqa: S603 -- resolved compiler, fixed argv
            [
                gcc,
                "-std=c23",
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
            exe = shutil.which(tool)
            if exe is None:
                continue
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / f"t.{'c' if lang == 'c' else 'cpp'}"
                src.write_text('#include "vmafx/rgb_math.h"\nint main(void) { return 0; }\n')
                done = subprocess.run(  # noqa: S603 -- resolved compiler, fixed argv
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
                "aligned_free(buffer);",
                "",
                "convert_planes leaks aligned buffer on fill_plane failure",
            ),
        )
        for old, new, expected in mutations:
            mutated = text.replace(old, new, 1)
            self.assertNotEqual(mutated, text, expected)
            self.assertIn(expected, frame_import_findings(mutated))

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

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


def _is_transparent_scope(source: str, brace: int) -> bool:
    """Match only a scope opener; code() replaces linkage string literals with \"\"."""
    prefix = source[max(0, brace - 200) : brace]
    return (
        re.search(
            r'\b(?:namespace(?:\s+[A-Za-z_]\w*)?|extern\s*"(?:C(?:\+\+)?)?")\s*$',
            prefix,
        )
        is not None
    )


def _extract_function_bodies(source: str) -> list[str]:
    bodies: list[str] = []
    depth = 0
    start = 0
    max_len = min(len(source), 1_000_000)
    for idx in range(max_len):
        ch = source[idx]
        if ch == "{":
            if depth == 0 and _is_transparent_scope(source, idx):
                start = idx + 1
            else:
                depth += 1
        elif ch == "}":
            if depth == 1:
                bodies.append(source[start : idx + 1])
                start = idx + 1
            if depth > 0:
                depth -= 1
    if not bodies and "vmafx_rgb_plan_init(" in source:
        return [source]
    return bodies


def _strip_outer_parens(s: str) -> str:
    res = s.strip()
    max_passes = min(len(res), 50)
    for _ in range(max_passes):
        if not (res.startswith("(") and res.endswith(")")):
            break
        depth = 0
        wraps_all = True
        limit = min(len(res), 500)
        for i in range(limit):
            if res[i] == "(":
                depth += 1
            elif res[i] == ")":
                depth -= 1
                if depth == 0 and i < len(res) - 1:
                    wraps_all = False
                    break
        if wraps_all and depth == 0:
            res = res[1:-1].strip()
        else:
            break
    return res


def _has_top_level_and(cond: str) -> bool:
    depth = 0
    max_len = min(len(cond), 500)
    for i in range(max_len):
        ch = cond[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth > 0:
                depth -= 1
        elif depth == 0 and cond[i : i + 2] == "&&":
            return True
    return False


def _split_top_level_or(cond: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    start = 0
    max_len = min(len(cond), 500)
    for i in range(max_len):
        ch = cond[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth > 0:
                depth -= 1
        elif depth == 0 and cond[i : i + 2] == "||":
            parts.append(cond[start:i])
            start = i + 2
    parts.append(cond[start:])
    return parts


def _check_cond_narrowing(cond: str, guard_re: re.Pattern[str]) -> list[str]:
    cleaned = _strip_outer_parens(cond)
    if _has_top_level_and(cleaned):
        return ["failure branch condition narrowed by &&"]
    for term in _split_top_level_or(cleaned):
        if guard_re.search(term) and _has_top_level_and(_strip_outer_parens(term)):
            return ["failure branch condition narrowed by &&"]
    return []


def _strip_nested_brace_blocks(block: str) -> str:
    top_level: list[str] = []
    depth = 0
    max_len = min(len(block), 2000)
    for i in range(max_len):
        ch = block[i]
        if ch == "{":
            depth += 1
            top_level.append(" ")
        elif ch == "}":
            if depth > 0:
                depth -= 1
            top_level.append(" ")
        elif depth == 0:
            top_level.append(ch)
    return "".join(top_level)


def _find_outer_brace_span(after_cond: str) -> tuple[int, int]:
    brace_open = -1
    for i in range(min(len(after_cond), 200)):
        if after_cond[i].isspace():
            continue
        if after_cond[i] == "{":
            brace_open = i
        break
    if brace_open < 0:
        return (-1, -1)
    depth = 1
    for i in range(brace_open + 1, min(len(after_cond), brace_open + 2000)):
        if after_cond[i] == "{":
            depth += 1
        elif after_cond[i] == "}":
            depth -= 1
            if depth == 0:
                return (brace_open, i)
    return (-1, -1)


def _check_guarded_block_return(after_cond: str, res_var: str) -> list[str]:
    brace_open, brace_close = _find_outer_brace_span(after_cond)
    if brace_open < 0 or brace_close < 0:
        return [f"missing return in !{res_var} branch"]
    block = after_cond[brace_open + 1 : brace_close]
    top_level = _strip_nested_brace_blocks(block)
    ret_match = re.search(r"\breturn\b", top_level)
    if not ret_match:
        return [f"missing return in !{res_var} branch"]
    preceding = top_level[: ret_match.start()]
    if re.search(r"\b(if|else|for|while|switch)\b", preceding):
        return [f"missing return in !{res_var} branch"]
    return []


def _find_closing_paren(text: str, paren_open: int) -> int:
    depth = 1
    max_idx = min(len(text), paren_open + 500)
    for i in range(paren_open + 1, max_idx):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _check_branch_returns(body_after: str, res_var: str) -> list[str]:
    guard_re = re.compile(rf"!\s*\(?\s*{re.escape(res_var)}\b")
    for m in list(re.finditer(r"\bif\s*\(", body_after))[:20]:
        paren_open = m.end() - 1
        paren_close = _find_closing_paren(body_after, paren_open)
        if paren_close < 0:
            continue
        cond = body_after[paren_open + 1 : paren_close]
        if not guard_re.search(cond):
            continue
        cond_errors = _check_cond_narrowing(cond, guard_re)
        if cond_errors:
            return cond_errors
        return _check_guarded_block_return(body_after[paren_close + 1 :], res_var)
    return [f"missing branch on !{res_var}"]


def _check_call_findings(body: str, match: re.Match[str]) -> list[str]:
    reasons: list[str] = []
    plan_name = match.group(1)
    call_pos = match.start()
    stmt_start = max(body.rfind(";", 0, call_pos), body.rfind("{", 0, call_pos))
    prefix = body[stmt_start + 1 : call_pos].strip()
    stmt_end = body.find(";", match.end())
    body_after = body[stmt_end + 1 :] if stmt_end >= 0 else body[match.end() :]
    init_pattern = rf"\bVmafxRgbPlan\s+{re.escape(plan_name)}\s*=\s*\{{\s*0\s*\}}\s*;"
    if not re.search(init_pattern, body):
        reasons.append("rgb plan not zero-initialized")
    if prefix.endswith("(void)") or "(void)" in prefix:
        reasons.append("vmafx_rgb_plan_init return cast to void")
        return reasons
    res_var = prefix.split("=")[0].strip().split()[-1] if "=" in prefix else None
    if not res_var:
        reasons.append("missing branch on !planned")
        return reasons
    if f"(void){res_var}" in body_after or re.search(
        rf"\(\s*void\s*\)\s*{re.escape(res_var)}\b", body_after
    ):
        reasons.append("vmafx_rgb_plan_init return cast to void")
    if re.search(rf"\bassert\s*\([^;]*\b{re.escape(res_var)}\b", body):
        reasons.append("assert on the recoverable plan result")
    reasons.extend(_check_branch_returns(body_after, res_var))
    return reasons


def rgb_plan_file_findings(text: str) -> list[str]:
    c = code(text)
    if "vmafx_rgb_plan_init(" not in c:
        return []
    call_pattern = re.compile(r"\bvmafx_rgb_plan_init\s*\(\s*&?([A-Za-z0-9_]+)")
    bodies = _extract_function_bodies(c)
    reasons: list[str] = []
    for body in bodies[:100]:
        for match in list(call_pattern.finditer(body))[:50]:
            reasons.extend(_check_call_findings(body, match))
    body_call_count = sum(len(list(call_pattern.finditer(b))) for b in bodies[:100])
    total_call_count = len(list(call_pattern.finditer(c)))
    if total_call_count > body_call_count:
        reasons.append("vmafx_rgb_plan_init called outside function")
    return reasons


def unhandled_rgb_plan_callers(root: Path = ROOT) -> list[str]:
    src = root / "core/src"
    unhandled = []
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".c", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8")
        if rgb_plan_file_findings(text):
            unhandled.append(str(path.relative_to(root)))
    return unhandled


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

    def test_rgb_plan_callers_handle_failure(self) -> None:
        unhandled = unhandled_rgb_plan_callers()
        self.assertEqual(unhandled, [])

    def test_scan_refuses_planted_unhandled_rgb_plan(self) -> None:
        valid_sample = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "        return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rgb_plan_file_findings(valid_sample), [])
        planted = {
            "vmafx_rgb_plan_init return cast to void": valid_sample.replace(
                "if (!planned) {\n        return -1;\n    }", "(void)planned;", 1
            ),
            "missing branch on !planned": valid_sample.replace(
                "if (!planned) {\n        return -1;\n    }", "", 1
            ),
            "rgb plan not zero-initialized": valid_sample.replace(
                "VmafxRgbPlan rgb = {0};", "VmafxRgbPlan rgb;", 1
            ),
            "assert on the recoverable plan result": valid_sample.replace(
                "    if (!planned) {", "    assert(planned);\n    if (!planned) {", 1
            ),
            "failure branch condition narrowed by &&": valid_sample.replace(
                "if (!planned) {", "if (!planned && !dst) {", 1
            ),
            "missing return in !planned branch": valid_sample.replace(
                "        return -1;\n", "        if (!dst) return -1;\n", 1
            ),
        }
        for what, mutated in planted.items():
            self.assertNotEqual(mutated, valid_sample, what)
            self.assertIn(what, rgb_plan_file_findings(mutated), what)

    def test_scan_flags_noncompliant_caller_when_file_has_compliant_one(self) -> None:
        sample = (
            "static int safe_fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "        return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "static int unsafe_fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb2 = {0};\n"
            "    const bool planned2 = vmafx_rgb_plan_init(&rgb2, layout, d->bpc, d->rgb_matrix,\n"
            "                                              d->rgb_range, d->rgb_out_range);\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rgb_plan_file_findings(sample), ["missing branch on !planned2"])

    def test_scan_flags_unsafe_caller_in_transparent_scopes(self) -> None:
        unsafe = (
            "int unsafe(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc,\n"
            "        d->rgb_matrix, d->rgb_range, d->rgb_out_range);\n"
            "    return 0;\n"
            "}\n"
        )
        safe = unsafe.replace("unsafe(", "safe(").replace(
            "    return 0;", "    if (!planned) { return -1; }\n    return 0;"
        )
        wrappers = (
            ("namespace {", "}"),
            ("namespace n {", "}"),
            ('extern "C" {', "}"),
            ('extern "C++" {', "}"),
            ("namespace a { namespace b {", "} }"),
        )
        for opening, closing in wrappers:
            with self.subTest(scope=opening):
                sample = f"{opening}\n{unsafe}{safe}{closing}\n"
                self.assertEqual(rgb_plan_file_findings(sample), ["missing branch on !planned"])

    def test_scan_flags_unsafe_caller_with_single_linkage_declarations(self) -> None:
        unsafe = (
            'extern "C" int unsafe(const VmafxFrameImport *d, const ImportLayout *layout) {\n'
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc,\n"
            "        d->rgb_matrix, d->rgb_range, d->rgb_out_range);\n"
            "    return 0;\n"
            "}\n"
        )
        safe = unsafe.replace("unsafe(", "safe(").replace(
            "    return 0;", "    if (!planned) { return -1; }\n    return 0;"
        )
        self.assertEqual(rgb_plan_file_findings(unsafe + safe), ["missing branch on !planned"])

    def test_scan_accepts_safe_callers_in_namespace(self) -> None:
        safe = (
            "int safe(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc,\n"
            "        d->rgb_matrix, d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) { return -1; }\n"
            "    return 0;\n"
            "}\n"
        )
        sample = "namespace {\n" + safe + safe.replace("safe(", "also_safe(") + "}\n"
        self.assertEqual(rgb_plan_file_findings(sample), [])

    def test_scan_refuses_empty_failure_branch(self) -> None:
        empty_branch = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn("missing return in !planned branch", rgb_plan_file_findings(empty_branch))

    def test_scan_refuses_log_only_failure_branch(self) -> None:
        log_only_branch = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            '        log_error("cannot init rgb plan");\n'
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn("missing return in !planned branch", rgb_plan_file_findings(log_only_branch))

    def test_scan_accepts_return_in_failure_branch(self) -> None:
        return_branch = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "        return -EINVAL;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rgb_plan_file_findings(return_branch), [])

    def test_scan_refuses_narrowed_and_failure_branch(self) -> None:
        narrowed_branch = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned && !dst) {\n"
            "        return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn(
            "failure branch condition narrowed by &&",
            rgb_plan_file_findings(narrowed_branch),
        )

    def test_scan_refuses_nested_if_in_failure_branch(self) -> None:
        nested_if_braceless = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned) {\n"
            "        if (!dst) return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn(
            "missing return in !planned branch",
            rgb_plan_file_findings(nested_if_braceless),
        )
        nested_if_braced = nested_if_braceless.replace(
            "        if (!dst) return -1;\n",
            "        if (!dst) {\n            return -1;\n        }\n",
            1,
        )
        self.assertIn(
            "missing return in !planned branch",
            rgb_plan_file_findings(nested_if_braced),
        )

    def test_scan_accepts_or_condition_failure_branch(self) -> None:
        or_branch = (
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    if (!planned || !dst) {\n"
            "        return -1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(rgb_plan_file_findings(or_branch), [])

    def test_known_gap_macro_checked_result_reported_as_unhandled(self) -> None:
        """Known gap (HISS-20): textual scanner does not expand helper guard macros.

        When a caller delegates result handling to a helper macro (or helper function)
        instead of an explicit `if (!planned) { return ...; }` block, the textual
        scanner flags the caller as missing an explicit branch.
        """
        macro_sample = (
            "#define CHECK_PLAN(p) do { if (!(p)) return -1; } while (0)\n"
            "static int fill(const VmafxFrameImport *d, const ImportLayout *layout) {\n"
            "    VmafxRgbPlan rgb = {0};\n"
            "    const bool planned = vmafx_rgb_plan_init(&rgb, layout, d->bpc, d->rgb_matrix,\n"
            "                                             d->rgb_range, d->rgb_out_range);\n"
            "    CHECK_PLAN(planned);\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(
            rgb_plan_file_findings(macro_sample),
            ["missing branch on !planned"],
        )

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

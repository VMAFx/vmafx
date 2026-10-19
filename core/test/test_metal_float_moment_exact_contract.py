#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_moment_metal's 16-bit second moments to the CPU's float squares (ADR-1498).

``moment.c::compute_2nd_moment()`` forms each square in ``float``: at 16 bits
that is the integer square rounded to 24 bits. ``float_moment_metal`` added
exact integer squares (T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02). The
10/12/16-bit kernel now adds ``vmaf_mtl_moment_float_square()``
(``metal_float_moment_math.h``): one fp32 product of the sample with itself,
converted to an integer, as ``moment_float_square()`` of the CUDA, SYCL and HIP
twins (ADR-1453, ADR-1449, ADR-1447). The host adds the workgroup sums in
``uint64`` and converts each total once.

Past 2^53 units of 1 / scaler^2 (a 16-bit frame of more than 2^21 pixels) the
CPU's running double rounds as it adds, and the exact uint64 sum rounded once
is another number. The twin forms the CPU's rounded sum there with the
arithmetic of ``feature/float_moment_sum.h`` (ADR-1497), as the CUDA, SYCL and
HIP twins do: ``metal/metal_float_moment_sum.h`` is its Metal copy (a Metal
address space on each pointer parameter, which the shared text cannot carry
without changing the other twins' preprocessed output), and
``float_moment.metal`` lays it out over five kernels the host enqueues after the
frame kernel on a frame that can pass 2^53 units. This test holds, without a
device:

- every function of the copy to the shared text through the rename rule at the
  top of the copy (address-space macros removed, comments and spacing ignored),
  and its constants to the shared values;
- the copy to integers and to the Metal language subset (no ``double`` or
  ``float``, no ``long long`` or ``ULL`` literal);
- the five kernels to the sum's design: an early return while the plane's exact
  sum is at most 2^53, the walk with its run and term fallback storing the CPU's
  sum, no device reduction in place of the walk;
- the host to sizing the passes by ``vmaf_mtl_msum_may_round()``, refusing a
  pipeline that cannot run 256 lanes, enqueuing the passes on the frame's
  encoder and taking the second-moment sums from the walk.

The reduction itself (uint64 threadgroup sums, lo/hi pairs) is pinned by
``test_metal_float_moment_contract.py``. Device-free: reads the sources only.
``test_metal_float_moment_math`` holds the term against ``moment.c`` on the
host; ``test_metal_float_moment_sum`` runs the copy's steps, laid out as the
kernels lay them out, against ``compute_2nd_moment()`` on frames past 2^53;
``test_metal_float_moment_parity`` compares the moments on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_float_moment_math.h"
KERNEL = "metal/float_moment.metal"
HOST = "metal/float_moment_metal.mm"
REFERENCE = "moment.c"
SUM_COPY = "metal/metal_float_moment_sum.h"
SUM_SHARED = "float_moment_sum.h"
# The shared header defines 21 functions; fewer found = the scan is stale.
MIN_SHARED_FUNCTIONS = 20
ORDERED_SHARED = "ordered_sum.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

TERM_SIGNATURE = "VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_moment_float_square(vmaf_mtl_u32 v)"
TERM = (
    "const float sample = (float)v;",
    "return (vmaf_mtl_u32)(sample * sample);",
)
KERNEL_16_SIGNATURE = "kernel void float_moment_kernel_16bpc("
KERNEL_16 = (
    "const uint rv = (uint)ref_row[(int)gid.x];",
    "const uint dv = (uint)dis_row[(int)gid.x];",
    "my_r2 = (ulong)vmaf_mtl_moment_float_square(rv);",
    "my_d2 = (ulong)vmaf_mtl_moment_float_square(dv);",
)
HOST_SUM_SIGNATURE = (
    "static void accumulate_partials(const FloatMomentStateMetal *s, uint64_t sum[4])"
)
HOST_SUM = tuple(
    f"sum[{k}] += reconstruct_partial(parts[{2 * k}], parts[{2 * k + 1}], i);" for k in range(4)
)
HOST_COLLECT = (
    "uint64_t sum[4] = {0u, 0u, 0u, 0u};",
    "const double ref1 = denom1 > 0.0 ? (double)sum[0] / denom1 : 0.0;",
    "const double ref2 = denom2 > 0.0 ? (double)sum[2] / denom2 : 0.0;",
    "const double dis2 = denom2 > 0.0 ? (double)sum[3] / denom2 : 0.0;",
)
REFERENCE_LINES = (
    "const float term = pic_ * pic_;",
    "cum += (double)term;",
    "cum /= ((double)w * h);",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


# The rename rule at the top of metal_float_moment_sum.h, shared -> copy, in the
# order it applies (a longer prefix before the one it contains).
RENAMES = (
    ("vmaf_moment_sum_", "vmaf_mtl_msum_"),
    ("VMAF_MOMENT_SUM_", "VMAF_MTL_MSUM_"),
    ("VMAF_MOMENT_PLAN_", "VMAF_MTL_MSUM_PLAN_"),
    ("VMAF_MOMENT_WALK_", "VMAF_MTL_MSUM_WALK_"),
    ("VMAF_MOMENT_SQUARE", "VMAF_MTL_MSUM_SQUARE"),
    ("vmaf_ordsum_", "vmaf_mtl_os_"),
    ("VmafOrdsumUnits", "VmafMtlOrdsumUnits"),
    ("VMAF_ORDSUM_FUNC", "VMAF_MTL_FUNC"),
    ("VMAF_ORDSUM_", "VMAF_MTL_OS_"),
)
ADDRESS_SPACE = re.compile(r"\bVMAF_MTL_(?:DEV|TG|THR)\b")
# What the copy takes from ordered_sum.h: the five functions and two constants.
ORDERED_FUNCTIONS = (
    "vmaf_ordsum_units",
    "vmaf_ordsum_round_shifted",
    "vmaf_ordsum_is_odd",
    "vmaf_ordsum_cap",
    "vmaf_ordsum_then",
)
ORDERED_CONSTANTS = ("VMAF_ORDSUM_PLAN_TERMS", "VMAF_ORDSUM_UNFIT")
SHARED_FUNCTION = re.compile(r"VMAF_ORDSUM_FUNC\s[^;{(]*?\b(vmaf_moment_sum_\w+)\(")
SHARED_CONSTANT = re.compile(
    r"^#define\s+(VMAF_MOMENT_(?:SUM|PLAN|WALK)_\w+)\s+(.+?)\s*(?:/\*.*)?$", re.M
)
KERNELS = (
    "float_moment_plane_sums",
    "float_moment_row_totals",
    "float_moment_row_plans",
    "float_moment_row_units",
    "float_moment_ordered_totals",
)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8")
        for name in (MATH, KERNEL, HOST, REFERENCE, SUM_COPY, SUM_SHARED, ORDERED_SHARED)
    }


def _function_body(code: str, signature: str) -> str:
    """Flattened text of the definition that starts with `signature` (brace-matched), or empty."""
    start = code.find(signature)
    if start < 0:
        return ""
    depth = 0
    for index in range(code.index("{", start), len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[start : index + 1]
    return ""


def _math_failures(math: str) -> list[str]:
    term = _function_body(_flat(math), TERM_SIGNATURE)
    failures: list[str] = []
    if any(piece not in term for piece in TERM):
        failures.append(f"{MATH}: the term is not one fp32 product of the sample with itself")
    if re.search(r"scaler|double|FMA|fma|\bv \* v\b", term):
        failures.append(f"{MATH}: the term is scaled, fused or squared in another type")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    body = _function_body(code, KERNEL_16_SIGNATURE)
    failures: list[str] = []
    if '#include "metal_float_moment_math.h"' not in code:
        failures.append(f"{KERNEL}: the kernel does not include the term's header")
    if any(piece not in body for piece in KERNEL_16):
        failures.append(f"{KERNEL}: the 16-bit kernel does not add the CPU's float squares")
    if re.search(r"\b(?:rv \* rv|dv \* dv)\b", body):
        failures.append(f"{KERNEL}: the 16-bit kernel adds exact integer squares")
    return failures


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    accumulate = _function_body(code, HOST_SUM_SIGNATURE)
    failures: list[str] = []
    if any(piece not in accumulate for piece in HOST_SUM):
        failures.append(f"{HOST}: the host does not add the workgroup sums in uint64")
    if "(double)reconstruct_partial" in code or "double sum[4]" in code:
        failures.append(f"{HOST}: the host adds the workgroup sums in floating point")
    if any(piece not in code for piece in HOST_COLLECT):
        failures.append(f"{HOST}: the host does not convert each exact total once")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]


def _renamed(text: str) -> str:
    for old, new in RENAMES:
        text = text.replace(old, new)
    return re.sub(r"\bhalf\b", "half_way", text)  # `half` is a Metal type name


def _compact(text: str) -> str:
    """Code without comments and without any whitespace (formatting-proof)."""
    return "".join(COMMENT.sub(" ", text).split())


def _definition(code: str, name: str) -> str:
    """Text from `name(` to the end of the brace-matched body of the function
    that `name` defines (not a call: the parameter list is followed by `{`),
    or empty. `code` has no whitespace, so the name is not word-bounded."""
    start = code.find(f"{name}(")
    while start >= 0:
        depth = 0
        index = start + len(name)
        while index < len(code):
            depth += (code[index] == "(") - (code[index] == ")")
            index += 1
            if depth == 0:
                break
        if index < len(code) and code[index] == "{":
            body_depth = 0
            for end in range(index, len(code)):
                body_depth += (code[end] == "{") - (code[end] == "}")
                if body_depth == 0:
                    return code[start : end + 1]
        start = code.find(f"{name}(", start + 1)
    return ""


def _constant(text: str, name: str) -> str:
    match = re.search(rf"^#define\s+{name}\s+(.+?)\s*(?:/\*.*)?$", text, re.M)
    return "".join(match.group(1).split()) if match else ""


def _mirror_failures(sources: dict[str, str]) -> list[str]:
    """The Metal copy equals the shared header function for function."""
    copy = _compact(ADDRESS_SPACE.sub("", sources[SUM_COPY]))
    shared_sum = sources[SUM_SHARED]
    failures: list[str] = []
    functions = sorted(set(SHARED_FUNCTION.findall(COMMENT.sub(" ", shared_sum))))
    if len(functions) < MIN_SHARED_FUNCTIONS:
        failures.append(f"{SUM_SHARED}: only {len(functions)} functions found; the scan is stale")
    pairs = [(name, _compact(_renamed(shared_sum))) for name in functions]
    pairs += [(name, _compact(_renamed(sources[ORDERED_SHARED]))) for name in ORDERED_FUNCTIONS]
    for name, shared_code in pairs:
        expected = _definition(shared_code, _renamed(name))
        found = _definition(copy, _renamed(name))
        if not expected:
            failures.append(f"{name}: not found in the shared header")
        elif found != expected:
            failures.append(f"{SUM_COPY}: {_renamed(name)} differs from the shared {name}")
    names = [(m.group(1), shared_sum) for m in SHARED_CONSTANT.finditer(shared_sum)]
    names += [(n, sources[ORDERED_SHARED]) for n in ORDERED_CONSTANTS]
    for name, text in names:
        if _renamed(_constant(text, name)) != _constant(sources[SUM_COPY], _renamed(name)):
            failures.append(f"{SUM_COPY}: constant {_renamed(name)} differs from {name}")
    return failures


def _copy_failures(copy: str) -> list[str]:
    code = COMMENT.sub(" ", copy)
    failures = []
    if re.search(r"\b(?:double|float)\b", code):
        failures.append(f"{SUM_COPY}: the sum uses a floating-point type")
    if re.search(r"\blong\s+long\b|\b0[xX][0-9a-fA-F]+[uU]?[lL]{2}\b|\b\d+[uU]?[lL]{2}\b", code):
        failures.append(f"{SUM_COPY}: a `long long` or `ULL` literal (Metal has neither)")
    for macro, space in (("DEV", "device"), ("TG", "threadgroup"), ("THR", "thread")):
        if f"#define VMAF_MTL_{macro} {space}\n" not in code:
            failures.append(f"{SUM_COPY}: VMAF_MTL_{macro} is not `{space}` under Metal")
    if '#include "metal_float_moment_math.h"' not in code:
        failures.append(f"{SUM_COPY}: the term is not metal_float_moment_math.h's")
    return failures


def _sum_kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures = []
    if '#include "metal_float_moment_sum.h"' not in code:
        failures.append(f"{KERNEL}: the kernels do not include the sum's header")
    bodies = {}
    for name in KERNELS:
        body = _function_body(code, f"kernel void {name}(")
        bodies[name] = body
        if not body:
            failures.append(f"{KERNEL}: no kernel {name}")
        if re.search(r"\b(?:double|atomic\w*|simd_sum|simd_\w+|long long)\b", body):
            failures.append(f"{KERNEL}: {name} uses a device reduction or fp64")
    for name in KERNELS[1:]:
        if "sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END" not in bodies[name]:
            failures.append(f"{KERNEL}: {name} does not return while the exact sum is the CPU's")
    walk = bodies["float_moment_ordered_totals"]
    for call in (
        "vmaf_mtl_msum_walk_stage(",
        "vmaf_mtl_msum_walk_run(",
        "vmaf_mtl_msum_walk_row_runs(",
        "vmaf_mtl_msum_walk_next(",
    ):
        if call not in walk:
            failures.append(f"{KERNEL}: the walk does not call {call[:-1]}")
    if "sums[2u + plane] = sum;" not in walk:
        failures.append(f"{KERNEL}: the walk does not store the CPU's sum")
    units = bodies["float_moment_row_units"]
    if "vmaf_mtl_msum_tree_step(units, lane, step);" not in units:
        failures.append(f"{KERNEL}: the increments are not composed by the ordered tree")
    plans = bodies["float_moment_row_plans"]
    if "vmaf_mtl_msum_plan_batch(&prefix," not in plans:
        failures.append(f"{KERNEL}: the plans are not the prefix of the exact row sums")
    totals = bodies["float_moment_row_totals"]
    if "vmaf_mtl_msum_lane_total(" not in totals:
        failures.append(f"{KERNEL}: the row sums are not the header's lane totals")
    return failures


def _sum_host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures = []
    if "s->rounds = vmaf_mtl_msum_may_round(w, h, bpc) != 0;" not in _function_body(
        code, "static int init_fex_metal("
    ):
        failures.append(f"{HOST}: the passes are not sized by vmaf_mtl_msum_may_round()")
    failures += [
        f"{HOST}: kernel {name} is not loaded" for name in KERNELS if f'"{name}"' not in code
    ]
    if "[pso maxTotalThreadsPerThreadgroup] < VMAF_MTL_MSUM_LANES" not in code:
        failures.append(f"{HOST}: a pipeline that cannot run 256 lanes is not refused")
    submit = _function_body(code, "static int submit_fex_metal(")
    if "if (s->rounds) { encode_sum_passes(s, enc, ref_buf, dis_buf, row_bytes); }" not in submit:
        failures.append(f"{HOST}: submit() does not enqueue the rounded-sum passes")
    if (
        submit.index("[enc dispatchThreadgroups:grid") > submit.index("encode_sum_passes(")
        if ("encode_sum_passes(" in submit and "[enc dispatchThreadgroups:grid" in submit)
        else False
    ):
        failures.append(f"{HOST}: the passes are enqueued before the frame kernel")
    apply = _function_body(code, "static int apply_rounded_sums(")
    if "sum[2] = frame[2]; sum[3] = frame[3];" not in apply:
        failures.append(f"{HOST}: the second-moment sums are not the walk's")
    collect = _function_body(code, "static int collect_fex_metal(")
    if "const int sum_err = apply_rounded_sums(s, sum);" not in collect:
        failures.append(f"{HOST}: collect() does not take the walk's sums")
    if collect.count("waitUntilCompleted") or collect.count("[cmd "):
        failures.append(f"{HOST}: collect() submits or waits for device work")
    for pass_index, name in enumerate(KERNELS):
        if f"select_sum_pass(s, enc, {pass_index}u);" not in _function_body(
            code, "static void encode_sum_passes("
        ):
            failures.append(f"{HOST}: encode_sum_passes() does not run {name}")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _math_failures(sources[MATH])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _reference_failures(sources[REFERENCE])
        + _mirror_failures(sources)
        + _copy_failures(sources[SUM_COPY])
        + _sum_kernel_failures(sources[KERNEL])
        + _sum_host_failures(sources[HOST])
    )


class FloatMomentMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_exact_integer_square_is_detected(self) -> None:
        # The pre-port 16-bit term.
        failures = self._edited(
            KERNEL,
            "        my_r2 = (ulong)vmaf_mtl_moment_float_square(rv);",
            "        my_r2 = (ulong)rv * (ulong)rv;",
        )
        self._assert_detected(failures, "does not add the CPU's float squares")

    def test_integer_square_in_the_header_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return (vmaf_mtl_u32)(sample * sample);",
            "    return v * v;",
        )
        self._assert_detected(failures, "not one fp32 product")
        self._assert_detected(failures, "squared in another type")

    def test_scaled_sample_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float sample = (float)v;",
            "    const float sample = (float)v / scaler;",
        )
        self._assert_detected(failures, "not one fp32 product")

    def test_double_host_sum_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "        sum[2] += reconstruct_partial(parts[4], parts[5], i);",
            "        sum[2] += (double)reconstruct_partial(parts[4], parts[5], i);",
        )
        self._assert_detected(failures, "in floating point")

    def test_double_host_totals_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    uint64_t sum[4] = {0u, 0u, 0u, 0u};",
            "    double sum[4] = {0.0, 0.0, 0.0, 0.0};",
        )
        self._assert_detected(failures, "in floating point")
        self._assert_detected(failures, "convert each exact total once")

    def test_changed_reference_square_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "            const float term = pic_ * pic_;",
            "            const double term = (double)pic_ * pic_;",
        )
        self._assert_detected(failures, "the twin mirrors it")

    # ADR-1497: the rounded sum past 2^53 units.

    def test_dropped_term_fallback_is_detected(self) -> None:
        failures = self._edited(
            SUM_COPY,
            "            sum = vmaf_mtl_msum_add_term(sum, (uint32_t)VMAF_MTL_MSUM_SQUARE(line[x]));",
            "            (void)x;",
        )
        self._assert_detected(failures, "vmaf_mtl_msum_walk_row_runs differs")

    def test_reordered_tree_is_detected(self) -> None:
        failures = self._edited(
            SUM_COPY, "vmaf_mtl_os_then(left, right)", "vmaf_mtl_os_then(right, left)"
        )
        self._assert_detected(failures, "vmaf_mtl_msum_tree_step differs")

    def test_ignored_parity_is_detected(self) -> None:
        failures = self._edited(SUM_COPY, "(m & 1u) ? units.odd : units.even", "units.even")
        self._assert_detected(failures, "vmaf_mtl_msum_add_run differs")

    def test_changed_shared_header_is_detected(self) -> None:
        # A change to the shared header that the copy did not follow.
        failures = self._edited(
            SUM_SHARED,
            "return (uint64_t)vmaf_ordsum_round_shifted(exact, (int)shift).even << shift;",
            "return (uint64_t)vmaf_ordsum_round_shifted(exact, (int)shift).odd << shift;",
        )
        self._assert_detected(failures, "vmaf_mtl_msum_add_term differs")

    def test_changed_constant_is_detected(self) -> None:
        failures = self._edited(
            SUM_COPY, "#define VMAF_MTL_MSUM_LANES 256u", "#define VMAF_MTL_MSUM_LANES 128u"
        )
        self._assert_detected(failures, "constant VMAF_MTL_MSUM_LANES differs")

    def test_fp64_in_the_copy_is_detected(self) -> None:
        failures = self._edited(
            SUM_COPY,
            "    const uint64_t exact = sum + (uint64_t)term;",
            "    const uint64_t exact = (uint64_t)((double)sum + (double)term);",
        )
        self._assert_detected(failures, "floating-point type")

    def test_ull_literal_is_detected(self) -> None:
        failures = self._edited(
            SUM_COPY,
            "#define VMAF_MTL_OS_UNFIT ((int64_t)((uint64_t)1 << 54u))",
            "#define VMAF_MTL_OS_UNFIT (1ULL << 54)",
        )
        self._assert_detected(failures, "ULL")

    def test_missing_address_space_is_detected(self) -> None:
        failures = self._edited(SUM_COPY, "#define VMAF_MTL_TG threadgroup", "#define VMAF_MTL_TG")
        self._assert_detected(failures, "VMAF_MTL_TG is not")

    def test_missing_walk_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "        command = vmaf_mtl_msum_walk_next(",
            "        command = VMAF_MTL_MSUM_WALK_DONE + 0u * vmaf_mtl_msum_unused(",
        )
        self._assert_detected(failures, "the walk does not call vmaf_mtl_msum_walk_next")

    def test_unstored_walk_is_detected(self) -> None:
        failures = self._edited(KERNEL, "        sums[2u + plane] = sum;", "        (void)sum;")
        self._assert_detected(failures, "does not store the CPU's sum")

    def test_exact_sum_returned_by_a_kernel_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    if (sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END)\n        return;\n    const uint height = dim.y;\n    const size_t base = (size_t)plane * height;\n    ulong prefix",
            "    const uint height = dim.y;\n    const size_t base = (size_t)plane * height;\n    ulong prefix",
        )
        self._assert_detected(failures, "does not return while the exact sum")

    def test_device_reduction_in_place_of_the_walk_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    if (lane == 0u)\n        sums[2u + plane] = sum;",
            "    if (lane == 0u)\n        atomic_fetch_add_explicit((device atomic_uint *)sums, 1u, memory_order_relaxed);\n    if (lane == 0u)\n        sums[2u + plane] = sum;",
        )
        self._assert_detected(failures, "device reduction or fp64")

    def test_host_using_the_exact_sums_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    const int sum_err = apply_rounded_sums(s, sum);",
            "    const int sum_err = 0;",
        )
        self._assert_detected(failures, "collect() does not take the walk's sums")

    def test_host_without_the_passes_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    if (s->rounds) { encode_sum_passes(s, enc, ref_buf, dis_buf, row_bytes); }",
            "",
        )
        self._assert_detected(failures, "does not enqueue the rounded-sum passes")

    def test_host_unsized_passes_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "s->rounds = vmaf_mtl_msum_may_round(w, h, bpc) != 0;",
            "s->rounds = false;",
        )
        self._assert_detected(failures, "vmaf_mtl_msum_may_round()")

    def test_walk_taking_the_exact_value_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    sum[2] = frame[2];\n    sum[3] = frame[3];",
            "    sum[2] = sum[2];\n    sum[3] = sum[3];",
        )
        self._assert_detected(failures, "the second-moment sums are not the walk's")


if __name__ == "__main__":
    unittest.main()

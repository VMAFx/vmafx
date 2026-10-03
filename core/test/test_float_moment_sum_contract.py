#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the float_moment twins' form of the CPU's second-moment sum (ADR-1497).

``moment.c::compute_2nd_moment()`` adds one float square per pixel into a
double, in raster order. Past 2^53 units of 1 / scaler^2 that sum rounds as it
adds, and the twins' exact integer sum, rounded once, is another number. The
twins form the CPU's sum instead, with ``feature/float_moment_sum.h``: four
kernels (each row's exact sum, a plan per row, each planned row's increments
composed in pixel order, one walk per plane) that replace the two
second-moment accumulators on a frame that can pass 2^53 units. The CUDA and
HIP twins compile the kernels of ``feature/float_moment_sum_gpu.h``; the SYCL
twin lays the same lane steps out in its own kernels; the Metal twin compiles
``metal/metal_float_moment_sum.h``, a copy of the header with a Metal address
space on each pointer (its mirror, kernels and host are held by
``test_metal_float_moment_exact_contract.py``, whose checks this test runs on
the header it edits).

This test holds, without a device:

- the shared header to integers (a SYCL kernel has no fp64, ADR-0220), its
  one-term step to the double add's rounding (ties to even) and its run step
  to the increment of the sum's parity;
- every twin's kernel source to the header, with the twin's own float square
  as the term;
- every twin's host to allocating the per-row buffers for the frames that can
  pass 2^53 units and launching the four kernels after the frame kernel;
- the walk to storing the CPU's sums in the accumulators the host reads;
- the SYCL kernels to choosing a plane by value (an argument array indexed at
  run time is private memory, which is scratch memory, ADR-1395).

test_float_moment_sum runs the header's steps against the CPU on the host;
test_{cuda,sycl,hip}_float_moment_parity compare the twins with the CPU.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_metal_float_moment_exact_contract as metal_contract

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

SUM = "float_moment_sum.h"
GPU = "float_moment_sum_gpu.h"
CUDA_KERNEL = "cuda/integer_moment/moment_score.cu"
CUDA_HOST = "cuda/integer_moment_cuda.c"
HIP_KERNEL = "hip/float_moment/moment_score.hip"
HIP_HOST = "hip/float_moment_hip.c"
SYCL = "sycl/integer_moment_sycl.cpp"
FILES = (SUM, GPU, CUDA_KERNEL, CUDA_HOST, HIP_KERNEL, HIP_HOST, SYCL)

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
KERNELS = ("moment_row_totals", "moment_row_plans", "moment_row_units", "moment_ordered_totals")
TERM = "#define VMAF_MOMENT_SQUARE(v) moment_float_square(v)"


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in FILES}


def _body(code: str, signature: str) -> str:
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


def _header_failures(header: str) -> list[str]:
    code = _flat(header)
    failures = []
    if re.search(r"\b(double|float)\b", code):
        failures.append(f"{SUM}: the shared arithmetic uses a floating-point type")
    add_term = _body(code, "uint64_t vmaf_moment_sum_add_term(")
    if "vmaf_ordsum_round_shifted(exact, (int)shift).even << shift" not in add_term:
        failures.append(f"{SUM}: vmaf_moment_sum_add_term() does not round to the even value")
    add_run = _body(code, "int vmaf_moment_sum_add_run(")
    if "m + (uint64_t)((m & 1u) ? units.odd : units.even)" not in add_run:
        failures.append(f"{SUM}: vmaf_moment_sum_add_run() ignores the parity of the sum")
    return failures


def _gpu_failures(gpu: str) -> list[str]:
    code = _flat(gpu)
    failures = [
        f"{GPU}: no kernel body {name}_body"
        for name in KERNELS
        if f"void {name}_body(const VmafMomentSumArgs &a)" not in code
    ]
    if "a.sums[2u + plane] = sum;" not in _body(code, "moment_ordered_totals_body("):
        failures.append(f"{GPU}: the walk does not store the CPU's sum")
    return failures


def _kernel_failures(name: str, kernel: str, gpu_header: bool) -> list[str]:
    code = _flat(kernel)
    failures = []
    if TERM not in code or "#define VMAF_ORDSUM_NO_FP64" not in code:
        failures.append(f"{name}: the sum's term is not the twin's moment_float_square()")
    if not re.search(rf'#include "(\.\./|feature/)?{SUM}"', code):
        failures.append(f"{name}: does not include {SUM}")
    if gpu_header and not re.search(rf'#include "(feature/)?{GPU}"', code):
        failures.append(f"{name}: does not compile the kernels of {GPU}")
    for entry in KERNELS if gpu_header else ():
        wrapper = _body(code, f"{entry}(const VmafMomentSumArgs a)")
        if f"{entry}_body(a);" not in wrapper:
            failures.append(f"{name}: no __global__ {entry} running {entry}_body()")
    return failures


def _cuda_host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures = [
        f"{CUDA_HOST}: kernel {name} is not resolved" for name in KERNELS if f'"{name}"' not in code
    ]
    if "vmaf_moment_sum_may_round(w, h, bpc)" not in _body(
        code, "static int moment_cuda_sum_alloc("
    ):
        failures.append(
            f"{CUDA_HOST}: the row buffers are not sized by vmaf_moment_sum_may_round()"
        )
    submit = _body(code, "static int submit_fex_cuda(")
    if "if (!err && s->row_totals) { err = moment_cuda_dispatch_sum(" not in submit:
        failures.append(f"{CUDA_HOST}: submit() does not launch the sum's kernels")
    return failures


def _hip_host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures = [
        f"{HIP_HOST}: kernel {name} is not resolved" for name in KERNELS if f'"{name}"' not in code
    ]
    if "vmaf_moment_sum_may_round(w, h, bpc)" not in _body(
        code, "static int moment_hip_sum_alloc("
    ):
        failures.append(f"{HIP_HOST}: the row buffers are not sized by vmaf_moment_sum_may_round()")
    launch = _body(code, "static int moment_hip_launch(")
    if "if (err == 0 && s->row_totals != NULL) err = moment_hip_launch_sum(" not in launch:
        failures.append(f"{HIP_HOST}: the frame launch does not launch the sum's kernels")
    return failures


def _sycl_failures(source: str) -> list[str]:
    code = _flat(source)
    failures = _kernel_failures(SYCL, source, gpu_header=False)
    enqueue = _body(code, "static void enqueue_moment_work(")
    if "if (s->d_row_totals) launch_moment_sum(q, s, shared_ref, shared_dis);" not in enqueue:
        failures.append(f"{SYCL}: the frame's work does not launch the sum's kernels")
    sum_launch = _body(code, "static void launch_moment_sum(")
    for call in (
        "launch_row_totals(",
        "launch_row_plans(",
        "launch_row_units(",
        "launch_ordered_totals(",
    ):
        if call not in sum_launch:
            failures.append(f"{SYCL}: launch_moment_sum() does not run {call[:-1]}")
    if "vmaf_moment_sum_may_round(s->width, s->height, s->bpc)" not in _body(
        code, "static bool moment_sum_alloc("
    ):
        failures.append(f"{SYCL}: the row arrays are not sized by vmaf_moment_sum_may_round()")
    if "a.sums[2u + plane] = sum;" not in _body(code, "static void launch_ordered_totals("):
        failures.append(f"{SYCL}: the walk does not store the CPU's sum")
    if re.search(r"\ba\.(luma|stride)\[plane\]", code):
        failures.append(f"{SYCL}: a plane is chosen by a run-time index into the argument block")
    return failures


def _metal_failures(sources: dict[str, str]) -> list[str]:
    """The Metal twin's copy of the header, its kernels and its host, against
    the shared header in `sources` (an edit of it that the copy did not follow
    fails here)."""
    metal = metal_contract._sources()
    metal[metal_contract.SUM_SHARED] = sources[SUM]
    return metal_contract._contract_failures(metal)


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _header_failures(sources[SUM])
        + _gpu_failures(sources[GPU])
        + _kernel_failures(CUDA_KERNEL, sources[CUDA_KERNEL], gpu_header=True)
        + _kernel_failures(HIP_KERNEL, sources[HIP_KERNEL], gpu_header=True)
        + _cuda_host_failures(sources[CUDA_HOST])
        + _hip_host_failures(sources[HIP_HOST])
        + _sycl_failures(sources[SYCL])
        + _metal_failures(sources)
    )


class FloatMomentSumContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_tie_away_from_even_is_detected(self) -> None:
        failures = self._edited(
            SUM,
            "vmaf_ordsum_round_shifted(exact, (int)shift).even << shift",
            "vmaf_ordsum_round_shifted(exact, (int)shift).odd << shift",
        )
        self.assertTrue(any("even value" in item for item in failures), failures)

    def test_ignored_parity_is_detected(self) -> None:
        failures = self._edited(
            SUM,
            "m + (uint64_t)((m & 1u) ? units.odd : units.even)",
            "m + (uint64_t)units.even",
        )
        self.assertTrue(any("parity" in item for item in failures), failures)

    def test_double_in_the_header_is_detected(self) -> None:
        failures = self._edited(
            SUM,
            "    const uint64_t exact = sum + (uint64_t)term;",
            "    const uint64_t exact = (uint64_t)((double)sum + (double)term);",
        )
        self.assertTrue(any("floating-point" in item for item in failures), failures)

    def test_integer_square_as_the_term_is_detected(self) -> None:
        failures = self._edited(
            HIP_KERNEL,
            TERM,
            "#define VMAF_MOMENT_SQUARE(v) ((uint64_t)(v) * (uint64_t)(v))",
        )
        self.assertTrue(any("moment_float_square" in item for item in failures), failures)

    def test_cuda_without_the_sum_is_detected(self) -> None:
        failures = self._edited(
            CUDA_HOST,
            "    if (!err && s->row_totals) {\n",
            "    if (0) {\n",
        )
        self.assertTrue(any("does not launch" in item for item in failures), failures)

    def test_hip_without_the_sum_is_detected(self) -> None:
        failures = self._edited(
            HIP_HOST,
            "    if (err == 0 && s->row_totals != NULL)\n",
            "    if (0)\n",
        )
        self.assertTrue(any("does not launch" in item for item in failures), failures)

    def test_sycl_without_the_sum_is_detected(self) -> None:
        failures = self._edited(
            SYCL,
            "    if (s->d_row_totals)\n        launch_moment_sum(q, s, shared_ref, shared_dis);",
            "",
        )
        self.assertTrue(any("does not launch" in item for item in failures), failures)

    def test_walk_without_the_store_is_detected(self) -> None:
        failures = self._edited(GPU, "        a.sums[2u + plane] = sum;", "        (void)sum;")
        self.assertTrue(any("does not store" in item for item in failures), failures)

    def test_metal_copy_not_following_the_header_is_detected(self) -> None:
        failures = self._edited(
            SUM,
            "    return (uint64_t)vmaf_ordsum_round_shifted(exact, (int)shift).even << shift;",
            "    return (uint64_t)vmaf_ordsum_round_shifted(exact, (int)shift).odd << shift;",
        )
        self.assertTrue(
            any("vmaf_mtl_msum_add_term differs" in item for item in failures), failures
        )

    def test_sycl_plane_by_index_is_detected(self) -> None:
        failures = self._edited(
            SYCL,
            "    const uint8_t *luma = plane == 0u ? a.luma[0] : a.luma[1];",
            "    const uint8_t *luma = a.luma[plane];",
        )
        self.assertTrue(any("run-time index" in item for item in failures), failures)


if __name__ == "__main__":
    unittest.main()

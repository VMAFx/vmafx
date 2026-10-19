#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer_psnr_metal to the CPU `psnr`'s integer SSE, options and flags.

T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26 (psnr part) and item (2) of
T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30, ADR-1498: integer_psnr.c sums
the squared differences of a plane as integers and derives every score from
that SSE through psnr_score.h. The Metal twin declared two of the CPU's six
options, had no flush and no TEMPORAL flag (so no `apsnr_*`, and
`--subsample` would have summed a subset of the frames), added its block sums
in double, halved every chroma plane as 4:2:0, and reduced each block with
uint32 simd_sum() calls that drop carries once 16-bit squares are summed.

The port follows psnr_sycl / psnr_cuda / psnr_hip (ADR-1365, ADR-1373,
ADR-1382): the kernel stores one exact uint64 SSE per threadgroup (thread 0
adds the group's 256 squares in threadgroup memory), the host adds those in
uint64 and calls vmaf_psnr_peak(), vmaf_psnr_max(), vmaf_psnr_from_mse() and,
from a flush(), vmaf_psnr_aggregate(); `enable_mse` adds `mse_*`; the option
table is the CPU's; the twin is TEMPORAL.

Device-free: reads the sources only. test_metal_integer_psnr_parity and the
psnr cases of test_metal_twin_option_parity compare the values on a device.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metal_option_tables as tables

FEATURE = tables.FEATURE
KERNEL = "metal/integer_psnr.metal"
HOST = "metal/integer_psnr_metal.mm"
REFERENCE = "integer_psnr.c"

# Every score passes through the CPU's helpers; the host forms none itself.
HOST_HELPERS = (
    '#include "psnr_score.h"',
    "s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);",
    "vmaf_psnr_max(bpc, s->peak, min_sse, s->width[p], s->height[p]);",
    "vmaf_psnr_from_mse(mse, (double)s->peak * s->peak, s->psnr_max[p], s->uncapped);",
    "vmaf_psnr_aggregate(s->peak, s->apsnr_sse[p], s->apsnr_n_pixels[p], s->psnr_max[p]);",
)
HOST_SUM = (
    "const uint64_t *parts = (const uint64_t *)s->rb[p].host_view;",
    "uint64_t sum = 0U;",
    "sum += parts[i];",
)
# The CPU's own expression for the MSE (integer_psnr.c::psnr / psnr_hbd).
HOST_MSE = "const double mse = ((double)sse) / (s->width[p] * s->height[p]);"
HOST_OUTPUTS = (
    'mse_name[PSNR_NUM_PLANES] = {"mse_y", "mse_cb", "mse_cr"};',
    'apsnr_name[PSNR_NUM_PLANES] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};',
    "if (err == 0 && s->enable_mse) {",
    "if (s->enable_apsnr) {",
    "vmaf_psnr_clip_sse_add(&s->apsnr_sse[p], sse);",
    "vmaf_feature_collector_set_aggregate(feature_collector, apsnr_name[p], apsnr);",
)
HOST_GEOMETRY = (
    "const unsigned ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1U : 0U;",
    "const unsigned ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1U : 0U;",
    "s->width[p] = (s->n_planes > 1U) ? (w + ss_hor) >> ss_hor : 0U;",
    "s->height[p] = (s->n_planes > 1U) ? (h + ss_ver) >> ss_ver : 0U;",
)
REGISTRATION = (
    ".flush = flush_fex_metal,",
    ".flags = VMAF_FEATURE_EXTRACTOR_METAL | VMAF_FEATURE_EXTRACTOR_TEMPORAL,",
)
# The group reduction (one helper) and what each of the two kernels (8 and 16
# bpc) does with it.
KERNEL_SUM = (
    "tg_se[lid] = my_se;",
    "threadgroup_barrier(mem_flags::mem_threadgroup);",
    "ulong group_se = 0uL;",
    "for (uint i = 0u; i < PSNR_TG_THREADS; ++i) {",
    "group_se += tg_se[i];",
    "sse_parts[slot] = group_se;",
)
KERNELS = 2
EACH_KERNEL = (
    "threadgroup ulong tg_se[PSNR_TG_THREADS];",
    "device ulong *sse_parts [[buffer(2)]],",
    "const long e = r - d;",
    "my_se = (ulong)(e * e);",
    "psnr_store_group_sse(my_se, lid, bid.y * grid_groups.x + bid.x, tg_se, sse_parts);",
)
# Constructs the kernel must not hold: a lossy reduction or a float value.
KERNEL_BANNED = re.compile(r"\bsimd_sum\b|\batomic\w*|\b(?:float|half|double)\b")
HOST_BANNED = re.compile(r"\blog10\s*\(|\bdouble\s+sse\w*\s*=|\+=\s*\(double\)")
# What the CPU extractor must keep doing for the twin to mirror it.
REFERENCE_LINES = (
    '#include "psnr_score.h"',
    "s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);",
    "const double mse = ((double)sse) / (ref_pic->w[p] * ref_pic->h[p]);",
    "vmaf_psnr_aggregate(s->peak, s->apsnr.sse[i], s->apsnr.n_pixels[i], s->psnr_max[i]);",
    "vmaf_psnr_clip_sse_add(&s->apsnr.sse[p], sse);",
    ".flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(tables.strip_comments(source).split())


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8") for name in (KERNEL, HOST, REFERENCE)
    }


def _missing(code: str, pieces: tuple[str, ...]) -> list[str]:
    return [piece for piece in pieces if " ".join(piece.split()) not in code]


def _table_failures(sources: dict[str, str]) -> list[str]:
    cpu = tables.table_of_text(sources[REFERENCE], FEATURE / REFERENCE)
    twin = tables.table_of_text(sources[HOST], FEATURE / HOST)
    return [f"{HOST}: option table: {item}" for item in tables.differences(cpu, twin)]


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures = [
        f"{HOST}: the score helper `{p}` is not called" for p in _missing(code, HOST_HELPERS)
    ]
    if _missing(code, HOST_SUM) or HOST_MSE not in code:
        failures.append(f"{HOST}: the SSE is not the uint64 sum of the group sums")
    failures += [f"{HOST}: `{p}` is missing" for p in _missing(code, HOST_OUTPUTS + REGISTRATION)]
    if _missing(code, HOST_GEOMETRY):
        failures.append(f"{HOST}: chroma planes do not follow the pixel format")
    if HOST_BANNED.search(code):
        failures.append(f"{HOST}: the host forms a score or sums the SSE in floating point")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    per_kernel = [p for p in EACH_KERNEL if code.count(" ".join(p.split())) != KERNELS]
    if _missing(code, KERNEL_SUM) or per_kernel:
        failures.append(f"{KERNEL}: a group SSE is not the exact uint64 sum of its squares")
    if KERNEL_BANNED.search(code):
        failures.append(f"{KERNEL}: a simd_sum, an atomic or a floating-point value is back")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [f"{REFERENCE} no longer holds `{p}`" for p in _missing(code, REFERENCE_LINES)]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _table_failures(sources)
        + _host_failures(sources[HOST])
        + _kernel_failures(sources[KERNEL])
        + _reference_failures(sources[REFERENCE])
    )


class MetalIntegerPsnrExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_missing_cpu_option_is_detected(self) -> None:
        failures = self._edited(HOST, '.name = "enable_apsnr",', '.name = "enable_apsnr_x",')
        self._assert_detected(failures, "the twin lacks enable_apsnr")

    def test_option_range_drift_is_detected(self) -> None:
        failures = self._edited(HOST, ".max = DBL_MAX,", ".max = 1000.0,")
        self._assert_detected(failures, "min_sse: cpu")

    def test_missing_temporal_flag_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            ".flags             = VMAF_FEATURE_EXTRACTOR_METAL | VMAF_FEATURE_EXTRACTOR_TEMPORAL,",
            ".flags             = VMAF_FEATURE_EXTRACTOR_METAL,",
        )
        self._assert_detected(failures, "VMAF_FEATURE_EXTRACTOR_TEMPORAL")

    def test_missing_flush_is_detected(self) -> None:
        failures = self._edited(HOST, ".flush             = flush_fex_metal,", ".flush = NULL,")
        self._assert_detected(failures, ".flush = flush_fex_metal,")

    def test_double_block_sum_is_detected(self) -> None:
        # The pre-port host: block sums added in double.
        failures = self._edited(HOST, "    uint64_t sum = 0U;", "    double sse_d = 0.0;")
        self._assert_detected(failures, "not the uint64 sum")
        self._assert_detected(failures, "in floating point")

    def test_own_score_expression_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "vmaf_psnr_from_mse(mse, (double)s->peak * s->peak, s->psnr_max[p], s->uncapped);",
            "10.0 * log10((double)s->peak * s->peak / mse);",
        )
        self._assert_detected(failures, "vmaf_psnr_from_mse")
        self._assert_detected(failures, "forms a score")

    def test_420_chroma_for_every_format_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "s->height[p] = (s->n_planes > 1U) ? (h + ss_ver) >> ss_ver : 0U;",
            "s->height[p] = (s->n_planes > 1U) ? (h + 1U) / 2U : 0U;",
        )
        self._assert_detected(failures, "do not follow the pixel format")

    def test_simd_sum_reduction_is_detected(self) -> None:
        # The pre-port kernel: two uint32 simd_sum() calls on the halves.
        failures = self._edited(
            KERNEL, "    tg_se[lid] = my_se;", "    tg_se[lid] = simd_sum((uint)my_se);"
        )
        self._assert_detected(failures, "simd_sum")
        self._assert_detected(failures, "not the exact uint64 sum")

    def test_float_term_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "        my_se = (ulong)(e * e);", "        my_se = (ulong)((float)e * e);"
        )
        self._assert_detected(failures, "floating-point value")
        self._assert_detected(failures, "not the exact uint64 sum")

    def test_cpu_reference_drift_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,",
            "    .flags = 0,",
        )
        self._assert_detected(failures, f"{REFERENCE} no longer holds")


if __name__ == "__main__":
    unittest.main()

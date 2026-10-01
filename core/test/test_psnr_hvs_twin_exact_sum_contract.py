#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact psnr_hvs twin design (ADR-1397).

``calc_psnrhvs()`` adds every masked coefficient error of a plane into one
running ``float``, so a twin returns the CPU's bits only if it produces the
same terms and they are added in the same order and type. Device-free: reads
the sources only. Every planted regression below is a construct the
pre-ADR-1397 twin had, so the contract fails on the old design and passes on
the new one. The device test (``test_cuda_psnr_hvs_parity``) checks the
resulting scores; this contract keeps the design from eroding on hosts
without the device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
MESON_BUILD = ROOT / "core" / "src" / "meson.build"

HELPER = "psnr_hvs_score.c"
CUDA_HOST = "cuda/integer_psnr_hvs_cuda.c"
CUDA_KERNEL = "cuda/integer_psnr_hvs/psnr_hvs_score.cu"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The scaling constant of the CPU's masking table. With an `f` suffix the
# product is taken in float; the CPU takes it in double.
MASK_SCALE = "0.3885746225901003"
FMAD_OFF = "'psnr_hvs_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
# A float accumulated over `+=` of an indexed load: a host or kernel sum of
# its own, next to the one in the shared helper.
OWN_FLOAT_SUM = re.compile(r"\b(?:sum|error_sum|partial|acc)\s*\+=")


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    sources = {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (HELPER, CUDA_HOST, CUDA_KERNEL)
    }
    sources["meson.build"] = MESON_BUILD.read_text(encoding="utf-8")
    return sources


def _helper_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[HELPER])
    if not re.search(r"\bfloat\s+ret\s*=\s*0\.0f\s*;", code):
        failures.append(f"{HELPER}: the running sum is no longer a single float")
    if not re.search(r"for\s*\([^)]*\)\s*ret\s*\+=\s*terms\[i\]\s*;", code):
        failures.append(f"{HELPER}: the terms are no longer added one by one in index order")
    if re.search(r"\b(?:long\s+)?double\s+(?:ret|sum|acc)\b", code):
        failures.append(f"{HELPER}: a double accumulator replaces the CPU's float sum")
    if not re.search(
        r"psnr_hvs\.c['\"],\s*feature_src_dir\s*\+\s*['\"]psnr_hvs_score\.c['\"]\]",
        sources["meson.build"],
    ):
        failures.append(
            "core/src/meson.build: psnr_hvs_score.c left the strict-FP psnr_hvs scalar library"
        )
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[CUDA_KERNEL])
    if f"{MASK_SCALE}f" in code:
        failures.append(f"{CUDA_KERNEL}: masking table scaled in float; the CPU scales in double")
    if f"(double)csf * {MASK_SCALE}" not in code:
        failures.append(f"{CUDA_KERNEL}: masking table no longer taken from a double product")
    if re.search(r"\bsqrtf\s*\(", code):
        failures.append(f"{CUDA_KERNEL}: float square root in the masking threshold")
    if "sqrt((double)energy * (double)ratio) / 32.0" not in code:
        failures.append(f"{CUDA_KERNEL}: masking threshold no longer a double product and root")
    if "(float)abs(block[ref_base + index] - block[dist_base + index])" not in code:
        failures.append(f"{CUDA_KERNEL}: coefficient error no longer an integer difference")
    if "terms[index] = (error * csf) * (error * csf);" not in code:
        failures.append(f"{CUDA_KERNEL}: the kernel no longer stores every term")
    if OWN_FLOAT_SUM.search(code):
        failures.append(f"{CUDA_KERNEL}: the kernel sums terms itself (a per-block partial)")
    if FMAD_OFF not in sources["meson.build"]:
        failures.append(
            "core/src/meson.build: psnr_hvs_score fatbin is not built with --fmad=false"
        )
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[CUDA_HOST])
    # A call, not a mention: the TU names the helper in a _Static_assert text.
    if not re.search(r"\bvmaf_psnr_hvs_plane_score\(\s*\w", code):
        failures.append(f"{CUDA_HOST}: plane scores no longer come from the shared CPU-order sum")
    for helper in ("vmaf_psnr_hvs_combined_score", "vmaf_psnr_hvs_score_db"):
        if not re.search(rf"\b{helper}\(\s*\w", code):
            failures.append(f"{CUDA_HOST}: {helper}() no longer forms the emitted score")
    if OWN_FLOAT_SUM.search(code):
        failures.append(f"{CUDA_HOST}: the host adds terms or partials in a sum of its own")
    if not re.search(r"total_blocks\s*\*\s*\(size_t\)PSNR_HVS_TERMS\s*\*\s*sizeof\(float\)", code):
        failures.append(f"{CUDA_HOST}: the readback no longer holds every term of every block")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return _helper_failures(sources) + _kernel_failures(sources) + _host_failures(sources)


class PsnrHvsTwinExactSumContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_contracting_build_is_detected(self) -> None:
        sources = _sources()
        sources["meson.build"] = sources["meson.build"].replace(FMAD_OFF + ",", "", 1)
        self.assertTrue(any("--fmad=false" in item for item in _contract_failures(sources)))

    def test_float_masking_table_is_detected(self) -> None:
        # The pre-ADR-1397 hvs_mask_at().
        sources = _sources()
        sources[CUDA_KERNEL] = sources[CUDA_KERNEL].replace(
            f"const double scaled = (double)csf * {MASK_SCALE};",
            f"const float scaled = csf * {MASK_SCALE}f;",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("scaled in float" in item for item in failures))
        self.assertTrue(any("double product" in item for item in failures))

    def test_float_threshold_is_detected(self) -> None:
        # The pre-ADR-1397 score_hvs_block().
        sources = _sources()
        sources[CUDA_KERNEL] = sources[CUDA_KERNEL].replace(
            "return (float)(sqrt((double)energy * (double)ratio) / 32.0);",
            "return sqrtf(energy * ratio) / 32.f;",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("float square root" in item for item in failures))
        self.assertTrue(any("double product and root" in item for item in failures))

    def test_float_coefficient_difference_is_detected(self) -> None:
        sources = _sources()
        sources[CUDA_KERNEL] = sources[CUDA_KERNEL].replace(
            "(float)abs(block[ref_base + index] - block[dist_base + index])",
            "fabsf((float)block[ref_base + index] - (float)block[dist_base + index])",
            1,
        )
        self.assertTrue(any("integer difference" in item for item in _contract_failures(sources)))

    def test_per_block_partial_is_detected(self) -> None:
        # The pre-ADR-1397 hvs_error(): one float sum per block on the device.
        sources = _sources()
        sources[CUDA_KERNEL] = sources[CUDA_KERNEL].replace(
            "terms[index] = (error * csf) * (error * csf);",
            "error_sum += (error * csf) * (error * csf);",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("no longer stores every term" in item for item in failures))
        self.assertTrue(any("per-block partial" in item for item in failures))

    def test_host_sum_of_partials_is_detected(self) -> None:
        # The pre-ADR-1397 reduce_hvs_planes().
        sources = _sources()
        sources[CUDA_HOST] = sources[CUDA_HOST].replace(
            "scores[p] = vmaf_psnr_hvs_plane_score(plane_terms, s->num_blocks[p], s->bpc);",
            "float sum = 0.0f;\n"
            "        for (unsigned i = 0; i < s->num_blocks[p]; i++) {\n"
            "            sum += plane_terms[i];\n"
            "        }\n"
            "        scores[p] = (double)sum;",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("shared CPU-order sum" in item for item in failures))
        self.assertTrue(any("sum of its own" in item for item in failures))

    def test_partials_sized_readback_is_detected(self) -> None:
        sources = _sources()
        sources[CUDA_HOST] = sources[CUDA_HOST].replace(
            "(size_t)s->total_blocks * (size_t)PSNR_HVS_TERMS * sizeof(float)",
            "(size_t)s->total_blocks * sizeof(float)",
            1,
        )
        self.assertTrue(
            any("every term of every block" in item for item in _contract_failures(sources))
        )

    def test_double_accumulator_is_detected(self) -> None:
        # Closer to the exact sum, and for that reason not the CPU's value.
        sources = _sources()
        sources[HELPER] = sources[HELPER].replace("float ret = 0.0f;", "double ret = 0.0;", 1)
        failures = _contract_failures(sources)
        self.assertTrue(any("no longer a single float" in item for item in failures))
        self.assertTrue(any("double accumulator" in item for item in failures))

    def test_reordered_sum_is_detected(self) -> None:
        sources = _sources()
        sources[HELPER] = sources[HELPER].replace(
            "ret += terms[i];", "ret += terms[n_terms - 1u - i];", 1
        )
        self.assertTrue(any("index order" in item for item in _contract_failures(sources)))

    def test_helper_outside_the_strict_library_is_detected(self) -> None:
        sources = _sources()
        sources["meson.build"] = sources["meson.build"].replace(
            "     feature_src_dir + 'psnr_hvs_score.c'],", "    ],", 1
        )
        self.assertTrue(
            any("strict-FP psnr_hvs scalar library" in item for item in _contract_failures(sources))
        )


if __name__ == "__main__":
    unittest.main()

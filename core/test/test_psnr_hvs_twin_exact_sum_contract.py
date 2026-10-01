#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact psnr_hvs twin design (ADR-1397, ADR-1401).

``calc_psnrhvs()`` adds every masked coefficient error of a plane into one
running ``float``, so a twin returns the CPU's bits only if it produces the
same terms and they are added in the same order and type. Device-free: reads
the sources only. Every planted regression below is a construct a twin had
before it was made exact, so the contract fails on the old design and passes
on the new one. The device tests (``test_cuda_psnr_hvs_parity``,
``test_sycl_psnr_hvs_parity``, ``test_hip_psnr_hvs_parity``) check the
resulting scores; this contract keeps the design from eroding on hosts
without the device.

The CUDA and HIP kernels take the masking threshold as a double product and
root, as the CPU does. The SYCL kernel has no fp64 (ADR-0220) and takes it
from ``sqrt_prod_rn()`` in ``sycl_exact_fp.h``, an integer square root of the
exact product, which the contract pins as well.
"""

from __future__ import annotations

import re
import unittest
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
MESON_BUILD = ROOT / "core" / "src" / "meson.build"

HELPER = "psnr_hvs_score.c"
EXACT_FP = "sycl/sycl_exact_fp.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# The scaling constant of the CPU's masking table. With an `f` suffix the
# product is taken in float; the CPU takes it in double.
MASK_SCALE = "0.3885746225901003"
# ADR-1403: every CUDA fatbin takes the one device FP list, psnr_hvs_score
# (ADR-1397) included; core/test/test_strict_fp_compiler_args.py pins the policy.
FMAD_OFF = "cuda_device_strict_fp_args = vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
FATBIN_FP_ARGS = "cuda_flags + cuda_device_strict_fp_args"
MASK_DOUBLE = f"const double scaled = (double)csf * {MASK_SCALE};"
MASK_FLOAT = f"const float scaled = csf * {MASK_SCALE}f;"
STORE_TERM = "terms[index] = (error * csf) * (error * csf);"
# A float accumulated over `+=` of an indexed load: a host or kernel sum of
# its own, next to the one in the shared helper.
OWN_FLOAT_SUM = re.compile(r"\b(?:sum|error_sum|partial|acc|ret)\s*\+=")
PLANE_SCORE_CALL = re.compile(r"=\s*vmaf_psnr_hvs_plane_score\(\s*plane_terms,[^;]*;")
PARTIAL_SUM = """= 0.0;
        float sum = 0.0f;
        for (unsigned i = 0; i < 1u; i++) {
            sum += plane_terms[i];
        }"""


@dataclass(frozen=True)
class Twin:
    """One GPU twin: where its code lives and how its exact arithmetic is spelled."""

    name: str
    host: str
    kernel: str
    # The threshold expression and what a float-only kernel would write.
    threshold: str
    float_threshold: str
    float_root: str
    # abs(ref - dist) on the integer coefficients, and its float form.
    error: str
    float_error: str
    # The readback size expression in the host code.
    readback: str
    partial_readback: str
    # The meson line that keeps the kernel uncontracted, and that line broken.
    build_flags: str
    broken_build_flags: str


CUDA = Twin(
    name="cuda",
    host="cuda/integer_psnr_hvs_cuda.c",
    kernel="cuda/integer_psnr_hvs/psnr_hvs_score.cu",
    threshold="return (float)(sqrt((double)energy * (double)ratio) / 32.0);",
    float_threshold="return sqrtf(energy * ratio) / 32.f;",
    float_root=r"\bsqrtf\s*\(",
    error="(float)abs(block[ref_base + index] - block[dist_base + index])",
    float_error="fabsf((float)block[ref_base + index] - (float)block[dist_base + index])",
    readback="(size_t)s->total_blocks * (size_t)PSNR_HVS_TERMS * sizeof(float)",
    partial_readback="(size_t)s->total_blocks * sizeof(float)",
    build_flags=FATBIN_FP_ARGS,
    broken_build_flags="cuda_flags",
)

HIP = Twin(
    name="hip",
    host="hip/integer_psnr_hvs_hip.c",
    kernel="hip/integer_psnr_hvs/psnr_hvs_score.hip",
    threshold="return (float)(sqrt((double)energy * (double)ratio) / 32.0);",
    float_threshold="return sqrtf(energy * ratio) / 32.f;",
    float_root=r"\bsqrtf\s*\(",
    error="(float)abs(ref[index] - dist[index])",
    float_error="fabsf((float)ref[index] - (float)dist[index])",
    readback="(size_t)s->total_blocks * (size_t)PSNR_HVS_HIP_TERMS * sizeof(float)",
    partial_readback="(size_t)s->total_blocks * sizeof(float)",
    build_flags=(
        "hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']"
    ),
    broken_build_flags="hip_strict_fp_args = ['-fhip-fp32-correctly-rounded-divide-sqrt']",
)

# Kernel and host of the SYCL twin share one translation unit.
SYCL = Twin(
    name="sycl",
    host="sycl/integer_psnr_hvs_sycl.cpp",
    kernel="sycl/integer_psnr_hvs_sycl.cpp",
    threshold="return vmaf_sycl_exact::sqrt_prod_rn(energy, ratio) / 32.f;",
    float_threshold="return sycl::sqrt(energy * ratio) / 32.f;",
    float_root=r"\bsycl::(?:native::)?sqrt\s*\(",
    error="(float)sycl::abs(block[ref_base + index] - block[dist_base + index])",
    float_error="sycl::fabs((float)block[ref_base + index] - (float)block[dist_base + index])",
    readback="(size_t)s->total_blocks * HVS_TERMS * sizeof(float)",
    partial_readback="(size_t)s->total_blocks * sizeof(float)",
    build_flags=(
        "sycl_feature_tail_args = ['-std=c++20'] + sycl_strict_fp_args + sycl_pic_arg"
        " + ['-fpermissive']"
    ),
    broken_build_flags="sycl_feature_tail_args = ['-std=c++20'] + sycl_pic_arg + ['-fpermissive']",
)

TWINS = (CUDA, HIP, SYCL)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    names = {HELPER, EXACT_FP}
    for twin in TWINS:
        names.update((twin.host, twin.kernel))
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
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


def _kernel_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[twin.kernel])
    if f"{MASK_SCALE}f" in code:
        failures.append(f"{twin.kernel}: masking table scaled in float; the CPU scales in double")
    if MASK_DOUBLE not in code:
        failures.append(f"{twin.kernel}: masking table no longer taken from a double product")
    if re.search(twin.float_root, code):
        failures.append(f"{twin.kernel}: float square root in the masking threshold")
    if twin.threshold not in code:
        failures.append(f"{twin.kernel}: masking threshold no longer the CPU's product and root")
    if twin.error not in code:
        failures.append(f"{twin.kernel}: coefficient error no longer an integer difference")
    if STORE_TERM not in code:
        failures.append(f"{twin.kernel}: the kernel no longer stores every term")
    if OWN_FLOAT_SUM.search(code):
        failures.append(f"{twin.kernel}: a float sum of terms next to the shared helper's")
    if (
        (
            twin.name == "cuda"
            and (
                FMAD_OFF not in sources["meson.build"]
                or FATBIN_FP_ARGS not in sources["meson.build"]
                or "'psnr_hvs_score'" not in sources["meson.build"]
            )
        )
        or (
            twin.name == "hip"
            and (
                "'psnr_hvs_score'" not in sources["meson.build"]
                or twin.build_flags not in sources["meson.build"]
            )
        )
        or (twin.name == "sycl" and twin.build_flags not in sources["meson.build"])
    ):
        failures.append(
            f"core/src/meson.build: the {twin.name} psnr_hvs kernel may contract a multiply and"
            " an add"
        )
    return failures


def _host_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    code = _code(sources[twin.host])
    # A call, not a mention: the TU names the helper in a static assertion text.
    if not re.search(r"\bvmaf_psnr_hvs_plane_score\(\s*\w", code):
        failures.append(f"{twin.host}: plane scores no longer come from the shared CPU-order sum")
    for helper in ("vmaf_psnr_hvs_combined_score", "vmaf_psnr_hvs_score_db"):
        if not re.search(rf"\b{helper}\(\s*\w", code):
            failures.append(f"{twin.host}: {helper}() no longer forms the emitted score")
    if OWN_FLOAT_SUM.search(code):
        failures.append(f"{twin.host}: a float sum of terms next to the shared helper's")
    if twin.readback not in code:
        failures.append(f"{twin.host}: the readback no longer holds every term of every block")
    return failures


def _exact_root_failures(sources: dict[str, str]) -> list[str]:
    """``sqrt_prod_rn()``: the fp64-free form of the CPU's double product and root."""
    failures: list[str] = []
    code = _code(sources[EXACT_FP])
    body = code[code.find("inline float sqrt_prod_rn(") :]
    body = body[: body.find("\n}\n") + 3]
    if "isqrt_floor50(product)" not in body:
        failures.append(f"{EXACT_FP}: sqrt_prod_rn() no longer takes the integer root")
    if re.search(r"\bsycl::(?:native::)?sqrt\s*\(", body):
        failures.append(f"{EXACT_FP}: sqrt_prod_rn() rounds the product before the root")
    if "floor_root + (floor_root & uint64_t{1})" not in body:
        failures.append(f"{EXACT_FP}: sqrt_prod_rn() no longer rounds the root to nearest")
    if re.search(r"\bdouble\b", sources[EXACT_FP]):
        failures.append(f"{EXACT_FP}: fp64 type in the device helpers")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = _helper_failures(sources) + _exact_root_failures(sources)
    for twin in TWINS:
        failures += _kernel_failures(twin, sources) + _host_failures(twin, sources)
    return failures


def _planted(path: str, old: str, new: str) -> dict[str, str]:
    """The sources with ``old`` replaced by ``new`` once in ``path``."""
    sources = _sources()
    if old not in sources[path]:
        raise AssertionError(f"{path}: planted regression has nothing to replace: {old!r}")
    sources[path] = sources[path].replace(old, new, 1)
    return sources


class PsnrHvsTwinExactSumContract(unittest.TestCase):
    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_contracting_build_is_detected(self) -> None:
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(
                    _planted("meson.build", twin.build_flags, twin.broken_build_flags)
                )
                self.assertTrue(
                    any(
                        f"the {twin.name} psnr_hvs kernel may contract" in item for item in failures
                    )
                )

    def test_float_masking_table_is_detected(self) -> None:
        # hvs_mask_at() of the twins that summed per block.
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(_planted(twin.kernel, MASK_DOUBLE, MASK_FLOAT))
                self.assertTrue(any(f"{twin.kernel}: masking table scaled" in i for i in failures))
                self.assertTrue(
                    any(f"{twin.kernel}: masking table no longer" in i for i in failures)
                )

    def test_float_threshold_is_detected(self) -> None:
        # The threshold of the twins that summed per block: a float product
        # rounded before a float root.
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(
                    _planted(twin.kernel, twin.threshold, twin.float_threshold)
                )
                self.assertTrue(any(f"{twin.kernel}: float square root" in i for i in failures))
                self.assertTrue(
                    any(f"{twin.kernel}: masking threshold no longer" in i for i in failures)
                )

    def test_float_coefficient_difference_is_detected(self) -> None:
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(_planted(twin.kernel, twin.error, twin.float_error))
                self.assertTrue(any(f"{twin.kernel}: coefficient error" in i for i in failures))

    def test_per_block_partial_is_detected(self) -> None:
        # hvs_error() of the twins that summed per block: one float per block
        # on the device.
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(
                    _planted(twin.kernel, STORE_TERM, "error_sum += (error * csf) * (error * csf);")
                )
                self.assertTrue(
                    any(f"{twin.kernel}: the kernel no longer stores" in i for i in failures)
                )
                self.assertTrue(any(f"{twin.kernel}: a float sum of terms" in i for i in failures))

    def test_host_sum_of_partials_is_detected(self) -> None:
        # The host reduction of the twins that summed per block.
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                sources = _sources()
                planted, count = PLANE_SCORE_CALL.subn(PARTIAL_SUM, sources[twin.host], count=1)
                self.assertEqual(count, 1)
                sources[twin.host] = planted
                failures = _contract_failures(sources)
                self.assertTrue(any(f"{twin.host}: plane scores no longer" in i for i in failures))
                self.assertTrue(any(f"{twin.host}: a float sum of terms" in i for i in failures))

    def test_partials_sized_readback_is_detected(self) -> None:
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(
                    _planted(twin.host, twin.readback, twin.partial_readback)
                )
                self.assertTrue(any(f"{twin.host}: the readback no longer" in i for i in failures))

    def test_double_accumulator_is_detected(self) -> None:
        # Closer to the exact sum, and for that reason not the CPU's value.
        failures = _contract_failures(_planted(HELPER, "float ret = 0.0f;", "double ret = 0.0;"))
        self.assertTrue(any("no longer a single float" in item for item in failures))
        self.assertTrue(any("double accumulator" in item for item in failures))

    def test_reordered_sum_is_detected(self) -> None:
        failures = _contract_failures(
            _planted(HELPER, "ret += terms[i];", "ret += terms[n_terms - 1u - i];")
        )
        self.assertTrue(any("index order" in item for item in failures))

    def test_helper_outside_the_strict_library_is_detected(self) -> None:
        failures = _contract_failures(
            _planted("meson.build", "     feature_src_dir + 'psnr_hvs_score.c'],", "    ],")
        )
        self.assertTrue(any("strict-FP psnr_hvs scalar library" in item for item in failures))

    def test_rounded_product_root_is_detected(self) -> None:
        # sqrt(fl(a * b)): the product rounded to fp32 before the root, which
        # differs from the CPU's value for a third of all operand pairs.
        failures = _contract_failures(
            _planted(
                EXACT_FP,
                "const uint64_t floor_root = isqrt_floor50(product);",
                "const uint64_t floor_root = (uint64_t)sycl::sqrt((float)product);",
            )
        )
        self.assertTrue(any("no longer takes the integer root" in item for item in failures))
        self.assertTrue(any("rounds the product before the root" in item for item in failures))

    def test_truncated_root_is_detected(self) -> None:
        failures = _contract_failures(
            _planted(
                EXACT_FP,
                "const uint64_t rounded = floor_root + (floor_root & uint64_t{1});",
                "const uint64_t rounded = floor_root & ~uint64_t{1};",
            )
        )
        self.assertTrue(any("no longer rounds the root to nearest" in item for item in failures))


if __name__ == "__main__":
    unittest.main()

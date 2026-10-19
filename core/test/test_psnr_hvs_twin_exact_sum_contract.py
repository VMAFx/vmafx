#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact psnr_hvs twin design (ADR-1397, ADR-1401, ADR-1498).

``calc_psnrhvs()`` adds every masked coefficient error of a plane into one
running ``float``, so a twin returns the CPU's bits only if it produces the
same terms and they are added in the same order and type. Device-free: reads
the sources only. Every planted regression below is a construct a twin had
before it was made exact, so the contract fails on the old design and passes
on the new one. The device tests (``test_cuda_psnr_hvs_parity``,
``test_sycl_psnr_hvs_parity``, ``test_hip_psnr_hvs_parity``,
``test_metal_integer_psnr_hvs_parity``) check the resulting scores; this
contract keeps the design from eroding on hosts without the device.

The masking threshold is upstream's statement, ``sqrt(s_mask * s_gvar) / 32.f``
(Netflix/vmaf ``libvmaf/src/feature/third_party/xiph/psnr_hvs.c:316-317``): a
float product, its root taken in double, the result stored as float
(ADR-1488). The scalar reference and its AVX2 and NEON forms write it; the
CUDA and HIP kernels form the float product and take the double root; the
SYCL kernel has no fp64 (ADR-0220) and takes ``sqrt_rn()`` of the float
product, the correctly rounded fp32 root, which is the same value (rounding a
square root to 53 bits and then to 24 equals rounding it to 24). The contract
pins all of them, and fails on the double product the fork carried between
PR #552 and ADR-1488.

The Metal twin (ADR-1498) has no double either: its arithmetic is
``metal/metal_psnr_hvs_math.h`` (the root is Metal's ``sqrt``, correctly
rounded under the kernels' ``-fno-fast-math``), ``integer_psnr_hvs.metal``
stores every term, and the host forms the masking table with the shared
``vmaf_psnr_hvs_mask_value()``. Before the port it took the masking table as
an fp32 product and summed each block into a float partial; the planted
regressions below include both.
"""

from __future__ import annotations

import re
import unittest
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
MESON_BUILD = ROOT / "core" / "src" / "meson.build"
# The Metal kernels' build file, under its own key in the sources.
METAL_MESON = "metal/meson.build"
METAL_MESON_BUILD = ROOT / "core" / "src" / "metal" / "meson.build"

HELPER = "psnr_hvs_score.c"
EXACT_FP = "sycl/sycl_exact_fp.h"
SCALAR = "third_party/xiph/psnr_hvs.c"
AVX2 = "x86/psnr_hvs_avx2.c"
NEON = "arm64/psnr_hvs_neon.c"
# Upstream's two statements, as the scalar reference carries them, and the
# same statements on the block structure of the SIMD forms. The `(double)`
# converts the product's result; one in front of the first operand is what
# PR #552 added.
CPU_THRESHOLDS = {
    SCALAR: (
        "s_mask = (float)(sqrt((double)(s_mask * s_gvar)) / 32.f);",
        "d_mask = (float)(sqrt((double)(d_mask * d_gvar)) / 32.f);",
    ),
    AVX2: (
        "b->s_mask = (float)(sqrt((double)(b->s_mask * b->s_gvar)) / 32.0);",
        "b->d_mask = (float)(sqrt((double)(b->d_mask * b->d_gvar)) / 32.0);",
    ),
    NEON: (
        "b->s_mask = (float)(sqrt((double)(b->s_mask * b->s_gvar)) / 32.0);",
        "b->d_mask = (float)(sqrt((double)(b->d_mask * b->d_gvar)) / 32.0);",
    ),
}
WIDE_PRODUCT = re.compile(r"sqrt\(\s*\(double\)\s*(?:b->)?[sd]_mask\s*\*")

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
    # The float product of the reference's statement, and the exact product
    # the twin formed before ADR-1488.
    product: str
    wide_product: str
    # The threshold expression and one with another root.
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
    # The statement in `kernel` that forms the stored term.
    term: str = STORE_TERM
    # The file that forms the masking table in double ("" = `kernel`).
    mask_file: str = ""
    # A host call that forms the masking table ("" = none: the kernel does).
    mask_call: str = ""
    # A kernel file besides `kernel` ("" = none), and its statement storing
    # every term.
    device: str = ""
    device_store: str = ""
    # The source key of the build file holding `build_flags`.
    meson: str = "meson.build"

    @property
    def mask_source(self) -> str:
        return self.mask_file or self.kernel


CUDA = Twin(
    name="cuda",
    host="cuda/integer_psnr_hvs_cuda.c",
    kernel="cuda/integer_psnr_hvs/psnr_hvs_score.cu",
    product="const float product = energy * ratio;",
    wide_product="const double product = (double)energy * (double)ratio;",
    threshold="return (float)(sqrt((double)product) / 32.0);",
    float_threshold="return sqrtf(product) / 32.f;",
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
    product="const float product = energy * ratio;",
    wide_product="const double product = (double)energy * (double)ratio;",
    threshold="return (float)(sqrt((double)product) / 32.0);",
    float_threshold="return sqrtf(product) / 32.f;",
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
    # One statement: the product is the argument of the root.
    product="vmaf_sycl_exact::sqrt_rn(energy * ratio)",
    wide_product="vmaf_sycl_exact::sqrt_prod_rn(energy, ratio)",
    threshold="return vmaf_sycl_exact::sqrt_rn(energy * ratio) / 32.f;",
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

# The arithmetic is a header on metal_portable.h, valid as MSL and as host C
# (ADR-1498); the .metal file composes it and stores every term; the host
# forms the masking table, which needs a double.
METAL = Twin(
    name="metal",
    host="metal/integer_psnr_hvs_metal.mm",
    kernel="metal/metal_psnr_hvs_math.h",
    product="const float product = energy * ratio;",
    wide_product="const VmafMtlPair product = vmaf_mtl_two_product(energy, ratio);",
    threshold="return VMAF_MTL_SQRT(product) / 32.f;",
    float_threshold="return metal::fast::sqrt(product) / 32.f;",
    float_root=r"\b(?:metal::)?(?:fast|native)::sqrt\s*\(|\brsqrt\s*\(",
    error="(float)(diff < 0 ? -diff : diff)",
    float_error="VMAF_MTL_FABS((float)ref - (float)dist)",
    readback="(size_t)s->num_blocks[p] * VMAF_PSNR_HVS_TERMS_PER_BLOCK * sizeof(float)",
    partial_readback="(size_t)s->num_blocks[p] * sizeof(float)",
    build_flags="metal_shader_strict_fp_args = ['-fno-fast-math', '-ffp-contract=off']",
    broken_build_flags="metal_shader_strict_fp_args = ['-fno-fast-math']",
    term="return (error * csf) * (error * csf);",
    mask_file=HELPER,
    mask_call="vmaf_psnr_hvs_mask_value(",
    device="metal/integer_psnr_hvs.metal",
    device_store="terms[(ulong)slot * VMAF_MTL_HVS_TERMS + lid] = valid_block ? term : 0.f;",
    meson=METAL_MESON,
)

TWINS = (CUDA, HIP, SYCL, METAL)


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    names = {HELPER, EXACT_FP, SCALAR, AVX2, NEON}
    for twin in TWINS:
        names.update(name for name in (twin.host, twin.kernel, twin.device) if name)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["meson.build"] = MESON_BUILD.read_text(encoding="utf-8")
    sources[METAL_MESON] = METAL_MESON_BUILD.read_text(encoding="utf-8")
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


def _reference_failures(sources: dict[str, str]) -> list[str]:
    """The scalar reference and its SIMD forms write upstream's float product."""
    failures: list[str] = []
    for name, statements in CPU_THRESHOLDS.items():
        code = " ".join(_code(sources[name]).split())
        for statement in statements:
            if statement not in code:
                failures.append(f"{name}: masking threshold is not upstream's `{statement}`")
        if WIDE_PRODUCT.search(code):
            failures.append(f"{name}: the masking product is widened before it is formed")
    return failures


def _mask_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    """The masking table: a double product stored as float, never an fp32 one."""
    failures: list[str] = []
    for name in dict.fromkeys((twin.kernel, twin.device, twin.mask_source)):
        if name and f"{MASK_SCALE}f" in _code(sources[name]):
            failures.append(f"{name}: masking table scaled in float; the CPU scales in double")
    if MASK_DOUBLE not in _code(sources[twin.mask_source]):
        failures.append(f"{twin.mask_source}: masking table no longer taken from a double product")
    if twin.mask_call and twin.mask_call not in _code(sources[twin.host]):
        failures.append(
            f"{twin.host}: the masking table no longer comes from {twin.mask_call.rstrip('(')}()"
        )
    return failures


def _device_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    """The kernel file of a twin whose arithmetic lives in a header."""
    if not twin.device:
        return []
    code = _code(sources[twin.device])
    failures: list[str] = []
    if twin.device_store not in " ".join(code.split()):
        failures.append(f"{twin.device}: the kernel no longer stores every term")
    if OWN_FLOAT_SUM.search(code):
        failures.append(f"{twin.device}: a float sum of terms next to the shared helper's")
    return failures


def _kernel_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    failures: list[str] = _mask_failures(twin, sources) + _device_failures(twin, sources)
    code = _code(sources[twin.kernel])
    if re.search(twin.float_root, code):
        failures.append(f"{twin.kernel}: float square root in the masking threshold")
    if twin.product not in code:
        failures.append(f"{twin.kernel}: masking product no longer the CPU's float product")
    if twin.threshold not in code:
        failures.append(f"{twin.kernel}: masking threshold no longer the CPU's product and root")
    if twin.error not in code:
        failures.append(f"{twin.kernel}: coefficient error no longer an integer difference")
    if twin.term not in code:
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
        or (twin.name == "metal" and twin.build_flags not in sources[METAL_MESON])
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
    """``sqrt_rn()``: the correctly rounded fp32 root the SYCL threshold takes."""
    failures: list[str] = []
    code = _code(sources[EXACT_FP])
    body = code[code.find("inline float sqrt_rn(") :]
    body = body[: body.find("\n}\n") + 3]
    if "const float r = sycl::fma(-s, s, x);" not in body:
        failures.append(f"{EXACT_FP}: sqrt_rn() no longer checks its root with an exact residual")
    if "nearer_root(s, other, r) : nearer_root(other, s, r_other)" not in body:
        failures.append(f"{EXACT_FP}: sqrt_rn() no longer picks the nearer of the two neighbours")
    if "sqrt_prod_rn" in code:
        failures.append(f"{EXACT_FP}: the root of an exact product is back (ADR-1488)")
    if re.search(r"\bdouble\b", sources[EXACT_FP]):
        failures.append(f"{EXACT_FP}: fp64 type in the device helpers")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = (
        _helper_failures(sources) + _reference_failures(sources) + _exact_root_failures(sources)
    )
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
                    _planted(twin.meson, twin.build_flags, twin.broken_build_flags)
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
                source = twin.mask_source
                failures = _contract_failures(_planted(source, MASK_DOUBLE, MASK_FLOAT))
                self.assertTrue(any(f"{source}: masking table scaled" in i for i in failures))
                self.assertTrue(any(f"{source}: masking table no longer" in i for i in failures))

    def test_metal_kernel_masking_table_is_detected(self) -> None:
        # psnr_hvs_block_body() of the Metal kernel before the port: its own
        # table, an fp32 product squared.
        planted = "const float m = csf[lid] * 0.3885746225901003f;\n"
        failures = _contract_failures(
            _planted(METAL.device, "inline void hvs_fdct8x8(", planted + "inline void hvs_fdct8x8(")
        )
        self.assertTrue(any(f"{METAL.device}: masking table scaled" in i for i in failures))

    def test_metal_host_table_without_the_helper_is_detected(self) -> None:
        failures = _contract_failures(
            _planted(METAL.host, "vmaf_psnr_hvs_mask_value(vmaf_mtl_hvs_csf[p][k])", "0.0f")
        )
        self.assertTrue(any(f"{METAL.host}: the masking table no longer" in i for i in failures))

    def test_metal_kernel_partial_is_detected(self) -> None:
        # The reduction the Metal kernel had: one float partial per block.
        failures = _contract_failures(_planted(METAL.device, METAL.device_store, "ret += term;"))
        self.assertTrue(any(f"{METAL.device}: the kernel no longer stores" in i for i in failures))
        self.assertTrue(any(f"{METAL.device}: a float sum of terms" in i for i in failures))

    def test_widened_reference_product_is_detected(self) -> None:
        # PR #552's cast, in the scalar reference and in its SIMD forms.
        for name, statements in CPU_THRESHOLDS.items():
            with self.subTest(source=name):
                old = statements[0]
                new = old.replace("sqrt((double)(", "sqrt((double)", 1)
                failures = _contract_failures(_planted(name, old, new))
                self.assertTrue(any(f"{name}: masking threshold is not" in i for i in failures))
                self.assertTrue(
                    any(f"{name}: the masking product is widened" in i for i in failures)
                )

    def test_exact_twin_product_is_detected(self) -> None:
        # The product the twins formed between ADR-1397 / ADR-1401 and
        # ADR-1488: exact, as the fork's CPU then had it.
        for twin in TWINS:
            with self.subTest(twin=twin.name):
                failures = _contract_failures(
                    _planted(twin.kernel, twin.product, twin.wide_product)
                )
                self.assertTrue(
                    any(f"{twin.kernel}: masking product no longer" in i for i in failures)
                )

    def test_float_threshold_is_detected(self) -> None:
        # Another root than the reference's: a float library root on CUDA and
        # HIP, the device's own root on SYCL.
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
                    _planted(twin.kernel, twin.term, "error_sum += (error * csf) * (error * csf);")
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

    def test_unchecked_root_is_detected(self) -> None:
        # The refined estimate returned without the neighbour check: correctly
        # rounded on most operands only.
        failures = _contract_failures(
            _planted(
                EXACT_FP,
                "    return r > 0.0f ? nearer_root(s, other, r) : nearer_root(other, s, r_other);",
                "    return s;",
            )
        )
        self.assertTrue(any("no longer picks the nearer" in item for item in failures))

    def test_exact_product_root_helper_is_detected(self) -> None:
        sources = _sources()
        sources[EXACT_FP] += "\ninline float sqrt_prod_rn(float a, float b);\n"
        failures = _contract_failures(sources)
        self.assertTrue(any("root of an exact product is back" in item for item in failures))


if __name__ == "__main__":
    unittest.main()

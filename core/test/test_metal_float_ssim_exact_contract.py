#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_ssim_metal design (ADR-1498, after ADR-1463 / ADR-1464).

The CPU extractor (``float_ssim.c`` through ``iqa_ssim()``) convolves five
statistics with the eleven-tap Gaussian, forms each window's luminance and
contrast as fp64 quotients of fp32 numerators and denominators and adds every
window's terms into one ``double``, in raster order; the frame means are
rounded to fp32. Metal has no fp64 type, so ``float_ssim_metal``, like its
SYCL twin:

- sums the convolution taps as exact fp32 pairs (``metal_ssim_terms.h``), the
  CPU's fp64 sum of fp32 products rounded once;
- forms the CPU's fp32 window values operation for operation, with no forced 1
  on an identical window (the CPU scores identical flat frames 72.247198959355487
  dB with ``enable_db``), and its two fp64 quotients on values held in 64-bit
  integers (``metal_soft_signed.h``);
- stores every window's term (or lv, cv and sv for ``enable_lcs``) at its
  raster position, with no reduction on the device;
- adds the read-back plane on the host in index order, divides by the window
  count and rounds the mean to fp32, as ``iqa_ssim()`` returns it;
- resolves the CPU's ``float_ssim`` option table, ``scale`` aside, and names
  the pin ``float_ssim_metal=scale=1`` in its scale error.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-port twin had (fp32 terms, ``simd_sum`` per threadgroup,
float partials, a forced 1, a decimal fp32 sum) or a regrouping that rounds
elsewhere, so the contract fails on the old design and passes on the new one.
``test_metal_float_ssim_math`` checks the arithmetic and the whole twin on the
host; ``test_metal_float_ssim_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

KERNEL = "metal/float_ssim.metal"
HOST = "metal/float_ssim_metal.mm"
TERMS = "metal/metal_ssim_terms.h"
REFERENCE = "iqa/ssim_tools.c"
LANE = "iqa/ssim_accumulate_lane.h"
TAPS = "iqa/ssim_tools.h"
CONVOLVE = "iqa/convolve.c"
CPU_EXTRACTOR = "float_ssim.c"
MATH_TEST = ROOT / "core" / "test" / "test_metal_float_ssim_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
DOUBLE_TYPE = re.compile(r"\bdouble\b")
REDUCTION = re.compile(r"\bsimd_sum\b|\bsimd_shuffle\w*|\bthreadgroup\b|\batomic\w*")
FORCED_ONE = re.compile(r"\bisfinite\b|\bisnan\b|:\s*1\.0f\b|\bmax\(|\bmin\(")
HOST_ONLY = "#if !defined(__METAL_VERSION__)"

# The shared subset of MSL, C and C++ (metal_portable.h).
SUBSET = (
    (DOUBLE_TYPE, "uses double"),
    (re.compile(r"\blong\s+long\b"), "uses long long"),
    (re.compile(r"\bstd::|\bnamespace\b|\btemplate\b|\bconstexpr\b"), "uses C++ only"),
    (re.compile(r"\bstatic\b"), "uses static"),
    (re.compile(r"[{,]\s*\.[A-Za-z_]\w*\s*="), "uses a designated initializer"),
    (re.compile(r"\bas_type\b|\bmemcpy\b|bit_cast"), "bit-casts outside VMAF_MTL_F2U/U2F"),
)

KERNEL_PIECES = (
    ('#include "metal_ssim_terms.h"', "the kernel does not include the shared terms header"),
    ("kernel void float_ssim_horiz(", "the horizontal kernel is gone"),
    ("kernel void float_ssim_vert_terms(", "the term kernel is gone"),
    ("kernel void float_ssim_vert_lcs(", "the enable_lcs term kernel is gone"),
    (
        "terms[params.offset + gid.y * params.final_width + gid.x] =",
        "the term is no longer stored at its raster position",
    ),
    (
        "vmaf_mtl_ssim_product_bits(float_ssim_window_terms(hbuf, params, gid.x, gid.y));",
        "the term is not the header's fp64 term",
    ),
    ("luminance[index] = vmaf_mtl_signed_bits(terms.luminance);", "lv is not stored"),
    ("contrast[index] = vmaf_mtl_signed_bits(terms.contrast);", "cv is not stored"),
    ("structure[index] = terms.structure;", "sv is not stored"),
    ("vmaf_mtl_ssim_add_horizontal_tap(sums, ref_f[index], dis_f[index],", "pair sum is gone"),
    ("vmaf_mtl_ssim_add_vertical_tap(sums, row, vmaf_mtl_ssim_gauss[tap]);", "pair sum is gone"),
    ("vmaf_mtl_ssim_double_terms(", "the fp64 quotients are not the header's"),
    ("vmaf_mtl_ssim_float_parts(vmaf_mtl_ssim_round_moments(sums), p.c1, p.c2)", "fp32 parts"),
)

HOST_PIECES = (
    ("vmaf_mtl_ssim_product_sum(terms, windows)", "collect does not add the stored terms"),
    (
        "vmaf_mtl_ssim_frame_sums(terms, contrast, structure, windows)",
        "enable_lcs does not add lv, cv and sv in raster order",
    ),
    ("*mean = (double)(float)*mean;", "the frame mean is not rounded to fp32"),
    ("picture_copy((float *)[dst contents]", "the planes are not the CPU's picture_copy()"),
    ('@"float_ssim_vert_terms"', "the host does not build the term kernel"),
    ('@"float_ssim_vert_lcs"', "the host does not build the enable_lcs term kernel"),
    ("[enc setBytes:&s->window length:sizeof(s->window)", "kernel arguments are not the shared"),
    ("windows * sizeof(uint64_t)", "the readback is not one fp64 pattern per window"),
    ("s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);", "the dB ceiling is not the CPU's"),
    ("vmaf_ssim_emit_score_named(", "the dB options do not go through the CPU's helpers"),
    ("vmaf_ssim_emit_scores_named(", "the dB options do not go through the CPU's helpers"),
    ("float_ssim_metal=scale=1", "the scale error does not name the pin as the CLI spells it"),
)
HOST_FORBIDDEN = (
    (re.compile(r"float_ssim_metal:scale=1"), "the scale error names a pin the CLI rejects"),
    (re.compile(r"\bpartials\b|\bparts\[|\bfloat_ssim_vert_combine\b"), "float partials are back"),
    (re.compile(r"\bsimd_sum\b|\bwg_reduce"), "a device reduction is back"),
    (re.compile(r"\bceil\(\s*10"), "an inline dB ceiling is back"),
    (re.compile(r"\binv\b\s*=\s*1\.0f\s*/"), "the host normalises the planes itself"),
)

# The lines of the CPU the header mirrors (comments stripped, whitespace folded).
REFERENCE_LINES = (
    "ref_sigma_sqd[offset] -= ref_mu[offset] * ref_mu[offset];",
    "cmp_sigma_sqd[offset] -= cmp_mu[offset] * cmp_mu[offset];",
    "ref_sigma_sqd[offset] = (float)(MAX(0.0, ref_sigma_sqd[offset]));",
    "cmp_sigma_sqd[offset] = (float)(MAX(0.0, cmp_sigma_sqd[offset]));",
    "sigma_both[offset] -= ref_mu[offset] * cmp_mu[offset];",
    "const float sigma_ref_sigma_cmp = sqrtf(ref_sigma_sqd[offset] * cmp_sigma_sqd[offset]);",
    "(2.0 * ref_mu[offset] * cmp_mu[offset] + C1) / "
    "(ref_mu[offset] * ref_mu[offset] + cmp_mu[offset] * cmp_mu[offset] + C1);",
    "const double c = (2.0 * sigma_ref_sigma_cmp + C2) / "
    "(ref_sigma_sqd[offset] + cmp_sigma_sqd[offset] + C2);",
    "(sigma_both[offset] < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f : sigma_both[offset];",
    "const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);",
    "*ssim_sum += l * c * s;",
    "*l_mean = (float)(l_sum / (double)(w * h));",
    "return (float)(ssim_sum / (double)(w * h));",
    "const float C1 = (K1 * L) * (K1 * L);",
    "const float C2 = (K2 * L) * (K2 * L);",
    "const float C3 = C2 / 2.0f;",
)
CONVOLVE_LINES = (
    "const float prod = img[img_offset + u] * k->kernel_h[k_offset];",
    "sum += (double)prod;",
    "img_cache[img_offset] = (float)(sum * scale);",
    "sum += (double)(img_cache[img_offset + (ptrdiff_t)v * w] * k->kernel_v[k_offset]);",
    "dst[(ptrdiff_t)y * dst_w + x] = (float)(sum * scale);",
)
LANE_LINES = (
    "const double lv = (2.0 * rm * cm + C1) / l_den;",
    "const double cv = (2.0 * srsc + C2) / c_den;",
    "*local_ssim += lv * cv * sv;",
)
# The same statements as the math test's reference_terms() holds them.
TEST_LINES = (
    "ref_sigma_sqd -= m.ref_mu * m.ref_mu;",
    "cmp_sigma_sqd -= m.cmp_mu * m.cmp_mu;",
    "ref_sigma_sqd = (float)MAX(0.0, ref_sigma_sqd);",
    "cmp_sigma_sqd = (float)MAX(0.0, cmp_sigma_sqd);",
    "sigma_both -= m.ref_mu * m.cmp_mu;",
    "const float sigma_ref_sigma_cmp = sqrtf(ref_sigma_sqd * cmp_sigma_sqd);",
    "(2.0 * m.ref_mu * m.cmp_mu + C1) / (m.ref_mu * m.ref_mu + m.cmp_mu * m.cmp_mu + C1);",
    "const double c = (2.0 * sigma_ref_sigma_cmp + C2) / (ref_sigma_sqd + cmp_sigma_sqd + C2);",
    "(sigma_both < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f : sigma_both;",
    "const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);",
    "*ssim_out = l * c * s;",
)

# The operations of the header, in the CPU's order.
TERMS_STEPS = {
    "a tap is the fp32 product added to a pair": (
        "const float product = sample * weight;",
        "return vmaf_mtl_ff_add(sum, vmaf_mtl_ff_make(product, 0.0f));",
    ),
    "the squares are fp32 products of the samples": (
        "const float ref_sq = ref * ref;",
        "const float cmp_sq = cmp * cmp;",
        "const float ref_cmp = ref * cmp;",
    ),
    "the variances are clamped at zero": (
        "const float ref_var = ref_var_raw < 0.0f ? 0.0f : ref_var_raw;",
        "const float cmp_var = cmp_var_raw < 0.0f ? 0.0f : cmp_var_raw;",
    ),
    "srsc is one fp32 square root of the product": (
        "const float var_product = ref_var * cmp_var;",
        "const float srsc = VMAF_MTL_SQRT(var_product);",
    ),
    "the denominators round left to right": (
        "const float l_den = l_den_sum + c1;",
        "const float c_den = c_den_sum + c2;",
    ),
    "a flat window's covariance is clamped": (
        "const float flat_covariance = (covariance < 0.0f && srsc <= 0.0f) ? 0.0f : covariance;",
    ),
    "S is one fp32 quotient": ("s_num / s_den};",),
    "lv is (2 * rm * cm + C1) / l_den in fp64": (
        "vmaf_mtl_signed_add(vmaf_mtl_signed_twice(mean_product), vmaf_mtl_signed_from_float(c1))",
        "vmaf_mtl_signed_div(l_num, vmaf_mtl_signed_from_float(p.l_den))",
    ),
    "cv is (2 * srsc + C2) / c_den in fp64": (
        "vmaf_mtl_signed_twice(vmaf_mtl_signed_from_float(p.srsc)), vmaf_mtl_signed_from_float(c2)",
        "vmaf_mtl_signed_div(c_num, vmaf_mtl_signed_from_float(p.c_den))",
    ),
    "the term is (lv * cv) * sv": (
        "vmaf_mtl_signed_mul(vmaf_mtl_signed_mul(t.luminance, t.contrast),"
        " vmaf_mtl_signed_from_float(t.structure))",
    ),
    "the host adds the sums in raster order, one double each": (
        "sums->ssim += lv * cv * sv;",
        "sums.luminance += vmaf_mtl_ssim_double_of(luminance[i]);",
        "sum += vmaf_mtl_ssim_double_of(terms[i]);",
    ),
}
GAUSSIAN = re.compile(r"vmaf_mtl_ssim_gauss\[VMAF_MTL_SSIM_TAPS\]\s*=\s*\{([^}]*)\}")


GAUSSIAN_TAPS = 11  # taps of g_gaussian_window_h


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _folded(source: str) -> str:
    """The code with every run of whitespace folded to one space."""
    return re.sub(r"\s+", " ", _code(source))


def _sources() -> dict[str, str]:
    names = (KERNEL, HOST, TERMS, REFERENCE, LANE, TAPS, CONVOLVE, CPU_EXTRACTOR)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["test"] = MATH_TEST.read_text(encoding="utf-8")
    return sources


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _folded(kernel)
    for piece, what in KERNEL_PIECES:
        if re.sub(r"\s+", " ", piece) not in code:
            failures.append(f"{KERNEL}: {what}")
    if REDUCTION.search(code):
        failures.append(f"{KERNEL}: the terms are reduced on the device")
    if DOUBLE_TYPE.search(code):
        failures.append(f"{KERNEL}: the kernel uses double")
    if FORCED_ONE.search(code):
        failures.append(f"{KERNEL}: a forced 1 or a clamp on a window's terms")
    if "/" in code or re.search(r"\bsqrt\(", code):
        failures.append(f"{KERNEL}: the window values are formed outside the header")
    if re.search(r"\bfloat\s+\w+\s*=\s*0\.0f\s*[,;]", code) and "+=" in code:
        failures.append(f"{KERNEL}: a plain fp32 running sum")
    return failures


def _options(source: str) -> list[tuple[str, str]]:
    """(name, type) of every option in a VmafOption table."""
    code = _code(source)
    names = re.findall(r'\.name\s*=\s*"(\w+)"', code)
    types = re.findall(r"\.type\s*=\s*(VMAF_OPT_TYPE_\w+)", code)
    return list(zip(names, types, strict=False))


def _host_failures(host: str, cpu: str) -> list[str]:
    failures: list[str] = []
    code = _folded(host)
    raw = _code(host)
    for piece, what in HOST_PIECES:
        if re.sub(r"\s+", " ", piece) not in code and piece not in host:
            failures.append(f"{HOST}: {what}")
    for pattern, what in HOST_FORBIDDEN:
        if pattern.search(raw) or pattern.search(host):
            failures.append(f"{HOST}: {what}")
    if _options(host) != _options(cpu):
        failures.append(f"{HOST}: the option table is not float_ssim.c's")
    if re.search(r"\bfloat\s*\*\s*\w+\s*=\s*\(const float \*\)s->terms", raw):
        failures.append(f"{HOST}: the host reads float partials")
    return failures


def _device_part(terms: str) -> str:
    return terms.split(HOST_ONLY, maxsplit=1)[0]


def _terms_failures(terms: str) -> list[str]:
    failures: list[str] = []
    code = _folded(terms)
    device = _folded(_device_part(terms))
    for what, lines in TERMS_STEPS.items():
        for line in lines:
            if re.sub(r"\s+", " ", line) not in code:
                failures.append(f"{TERMS}: {what}: `{line.strip()}` is gone")
    for pattern, what in SUBSET:
        if pattern.search(device):
            failures.append(f"{TERMS}: leaves the MSL/C/C++ subset: {what}")
    if FORCED_ONE.search(device):
        failures.append(f"{TERMS}: a forced 1 or a clamp on a window's terms")
    if '#include "metal_soft_signed.h"' not in code:
        failures.append(f"{TERMS}: the quotients are not built on metal_soft_signed.h")
    return failures


def _gaussian(source: str, pattern: re.Pattern[str]) -> list[str]:
    match = pattern.search(_code(source))
    return re.findall(r"\d\.\d+f", match.group(1)) if match else []


def _reference_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    reference = _folded(sources[REFERENCE])
    for line in REFERENCE_LINES:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
    convolve = _folded(sources[CONVOLVE])
    for line in CONVOLVE_LINES:
        if line not in convolve:
            failures.append(f"{CONVOLVE}: no longer holds `{line}`; the twin mirrors it")
    lane = _folded(sources[LANE])
    for line in LANE_LINES:
        if line not in lane:
            failures.append(f"{LANE}: no longer holds `{line}`; the twin mirrors it")
    test = _folded(sources["test"])
    for line in TEST_LINES:
        if line not in test:
            failures.append(f"{MATH_TEST.name}: reference_terms() lost `{line}`")
    cpu_taps = re.findall(r"g_gaussian_window_h\[GAUSSIAN_LEN\]\s*=\s*\{([^}]*)\}", sources[TAPS])
    taps = re.findall(r"\d\.\d+f", cpu_taps[0]) if cpu_taps else []
    if taps != _gaussian(sources[TERMS], GAUSSIAN) or len(taps) != GAUSSIAN_TAPS:
        failures.append(f"{TERMS}: the Gaussian is not g_gaussian_window_h's")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST], sources[CPU_EXTRACTOR])
        + _terms_failures(sources[TERMS])
        + _reference_failures(sources)
    )


def _replaced(name: str, old: str, new: str) -> list[str]:
    """The contract's failures with `old` replaced by `new` in one source."""
    sources = _sources()
    # Whitespace-tolerant: the formatter wraps lines.
    pattern = re.compile(r"\s+".join(re.escape(part) for part in old.split()))
    if not pattern.search(sources[name]):
        raise AssertionError(f"{name}: `{old}` not found, the planted regression is stale")
    sources[name] = pattern.sub(lambda _: new, sources[name], count=1)
    return _contract_failures(sources)


def _appended(name: str, text: str) -> list[str]:
    sources = _sources()
    sources[name] += text
    return _contract_failures(sources)


class FloatSsimMetalExactContract(unittest.TestCase):
    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_simd_sum_reduction_is_detected(self) -> None:
        # The pre-port per-threadgroup simd_sum of the terms.
        failures = _appended(KERNEL, "\ninline float r(float t) { return simd_sum(t); }\n")
        self._assert_detected(failures, "reduced on the device")

    def test_fp32_window_quotients_are_detected(self) -> None:
        # The pre-port kernel divided in fp32 where the CPU divides in fp64.
        failures = _appended(KERNEL, "\ninline float q(float a, float b) { return a / b; }\n")
        self._assert_detected(failures, "formed outside the header")

    def test_forced_one_is_detected(self) -> None:
        # The pre-port kernel: `my_l = lden > 0.0f ? lnum / lden : 1.0f;`.
        failures = _appended(KERNEL, "\ninline float f(float d) { return d > 0.0f ? d : 1.0f; }\n")
        self._assert_detected(failures, "forced 1")

    def test_forced_one_in_the_header_is_detected(self) -> None:
        failures = _replaced(
            TERMS,
            "const float c3 = c2 / 2.0f;",
            "const float c3 = c2 / 2.0f;\n    if (l_den <= 0.0f) { return parts_one(); }\n"
            "    const float one = srsc > 0.0f ? 1.0f : 1.0f;",
        )
        self._assert_detected(failures, "forced 1")

    def test_plain_fp32_tap_sum_is_detected(self) -> None:
        # The pre-port convolution: `mu_r += w * r;` in fp32.
        failures = _replaced(
            TERMS,
            "return vmaf_mtl_ff_add(sum, vmaf_mtl_ff_make(product, 0.0f));",
            "return vmaf_mtl_ff_make(sum.hi + product, 0.0f);",
        )
        self._assert_detected(failures, "a tap is the fp32 product added to a pair")

    def test_dropped_variance_clamp_is_detected(self) -> None:
        failures = _replaced(
            TERMS,
            "const float ref_var = ref_var_raw < 0.0f ? 0.0f : ref_var_raw;",
            "const float ref_var = ref_var_raw;",
        )
        self._assert_detected(failures, "the variances are clamped at zero")

    def test_fp32_contrast_numerator_is_detected(self) -> None:
        failures = _replaced(
            TERMS,
            "vmaf_mtl_signed_div(c_num, vmaf_mtl_signed_from_float(p.c_den))",
            "vmaf_mtl_signed_from_float((float)(p.srsc / p.c_den))",
        )
        self._assert_detected(failures, "cv is (2 * srsc + C2) / c_den in fp64")

    def test_regrouped_product_is_detected(self) -> None:
        # lv * (cv * sv) rounds elsewhere than (lv * cv) * sv.
        failures = _replaced(
            TERMS,
            "vmaf_mtl_signed_mul(vmaf_mtl_signed_mul(t.luminance, t.contrast),"
            " vmaf_mtl_signed_from_float(t.structure))",
            "t.luminance, vmaf_mtl_signed_mul(t.contrast, vmaf_mtl_signed_from_float(t.structure))",
        )
        self._assert_detected(failures, "the term is (lv * cv) * sv")

    def test_unstored_term_is_detected(self) -> None:
        failures = _replaced(
            KERNEL,
            "terms[params.offset + gid.y * params.final_width + gid.x] =",
            "const ulong term =",
        )
        self._assert_detected(failures, "raster position")

    def test_float_partials_on_the_host_are_detected(self) -> None:
        failures = _appended(HOST, "\nstatic float p(const float *parts) { return parts[0]; }\n")
        self._assert_detected(failures, "float partials")

    def test_host_sum_of_a_part_is_detected(self) -> None:
        failures = _replaced(
            HOST,
            "vmaf_mtl_ssim_product_sum(terms, windows)",
            "vmaf_mtl_ssim_product_sum(terms, 1u)",
        )
        self._assert_detected(failures, "collect does not add the stored terms")

    def test_reordered_host_sum_is_detected(self) -> None:
        failures = _replaced(
            TERMS,
            "sum += vmaf_mtl_ssim_double_of(terms[i]);",
            "sum += vmaf_mtl_ssim_double_of(terms[count - 1u - i]);",
        )
        self._assert_detected(failures, "raster order")

    def test_unrounded_frame_mean_is_detected(self) -> None:
        failures = _replaced(HOST, "*mean = (double)(float)*mean;", "")
        self._assert_detected(failures, "rounded to fp32")

    def test_host_normalisation_is_detected(self) -> None:
        # The pre-port submit(): `rf[...] = (float)rs[x] * inv;`.
        failures = _replaced(
            HOST, "picture_copy((float *)[dst contents]", "convert_plane((float *)[dst contents]"
        )
        self._assert_detected(failures, "picture_copy()")

    def test_inline_db_ceiling_is_detected(self) -> None:
        # The pre-port init() computed the ceiling itself.
        failures = _replaced(
            HOST,
            "s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);",
            "s->max_db = ceil(10.0 * log10((double)(w * h)));",
        )
        self._assert_detected(failures, "inline dB ceiling")

    def test_colon_scale_hint_is_detected(self) -> None:
        failures = _replaced(HOST, "float_ssim_metal=scale=1", "float_ssim_metal:scale=1")
        self._assert_detected(failures, "names a pin the CLI rejects")

    def test_changed_option_table_is_detected(self) -> None:
        failures = _replaced(HOST, 'enable_lcs",', 'enable_lcs_x",')
        self._assert_detected(failures, "option table")

    def test_double_in_the_header_is_detected(self) -> None:
        failures = _replaced(
            TERMS,
            "VMAF_MTL_FUNC VmafMtlSsimConstants vmaf_mtl_ssim_constants(void)",
            "VMAF_MTL_FUNC double half(void) { return 0.5; }\n"
            "VMAF_MTL_FUNC VmafMtlSsimConstants vmaf_mtl_ssim_constants(void)",
        )
        self._assert_detected(failures, "uses double")

    def test_changed_gaussian_is_detected(self) -> None:
        failures = _replaced(
            TERMS, "0.036001f, 0.109361f, 0.213006f,", "0.036002f, 0.109361f, 0.213006f,"
        )
        self._assert_detected(failures, "g_gaussian_window_h")

    def test_changed_reference_is_detected(self) -> None:
        failures = _replaced(REFERENCE, "*ssim_sum += l * c * s;", "*ssim_sum += l * (c * s);")
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protect fp64-free SpEED kernels, device-resident twins and explicit SYCL output captures.

Also pins the float motion SAD (ADR-1409, ADR-1411): the device adds the
absolute differences of a row in one work-item, left to right, into one fp32
accumulator, and the host adds the rows and divides in fp32 through
float_motion_sad.h, as compute_motion_simd() does.

Also pins float_ms_ssim_sycl's arithmetic (ADR-1414): every decimate tap is
one fused multiply-add as in ms_ssim_decimate.c, the window sums and the
l / c / s terms come from the shared sycl_ssim_terms.h (the CPU's operand
types as exact fp32 pairs), the frame sums are int64 fixed point, and the
host rounds each per-scale mean to fp32 and combines as ms_ssim.c does.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYCL_ROOT = ROOT / "core" / "src" / "feature" / "sycl"

# ADR-1358 / ADR-1363: the exact fp32 helpers shared by the SpEED pipeline and
# the ssimulacra2 twin are device code and must never mention the fp64 type.
EXACT_FP_HEADER = "sycl_exact_fp.h"
# ADR-1358: every SpEED device kernel lives in the pipeline TU, which must stay
# fp64-free; the extractor and host-setup TUs hold no kernel and never call the
# retired host linear-algebra residual or wait on the queue mid-frame.
SPEED_PIPELINE = "speed_sycl_pipeline.cpp"
SPEED_HOST_TUS = ("speed_chroma_sycl.cpp", "speed_temporal_sycl.cpp", "speed_sycl_host.cpp")
# Calls, not mentions: the sources cite `picture_copy()` in comments.
SPEED_HOST_RESIDUAL = tuple(
    re.compile(rf"\b{name}\(\s*[\w&*]")
    for name in (
        "speed_internal_compute_eigenvalues",
        "speed_internal_qr_factorize",
        "speed_internal_qt_multiply",
        "speed_internal_filter_and_downscale",
        "speed_internal_compute_means",
        "speed_internal_is_matrix_regular",
        "picture_copy",
    )
)
MOMENT_OUTPUT_COUNT = 4
# ADR-1363: ssimulacra2 and float_ms_ssim run the whole frame from submit() and
# wait on the queue once, in collect(). The retired ssimulacra2 host stages
# (per-scale host XYB, host downsample, host SSIM / edge combine, host YUV
# conversion) must not come back.
SSIMULACRA2 = "ssimulacra2_sycl.cpp"
MS_SSIM = "integer_ms_ssim_sycl.cpp"
SSIMULACRA2_HOST_RESIDUAL = tuple(
    re.compile(rf"\b{name}\(")
    for name in (
        "ss2s_host_combine",
        "ss2s_host_linear_rgb_to_xyb",
        "ss2s_downsample_2x2",
        "ss2s_picture_to_linear_rgb",
    )
)
QUEUE_WAIT = re.compile(r"(?:\.|->)wait(?:_and_throw)?\(")


def _sources() -> dict[str, str]:
    names = (
        EXACT_FP_HEADER,
        SPEED_PIPELINE,
        *SPEED_HOST_TUS,
        SSIMULACRA2,
        "float_psnr_sycl.cpp",
        "integer_psnr_sycl.cpp",
        "integer_moment_sycl.cpp",
        MS_SSIM,
    )
    return {name: (SYCL_ROOT / name).read_text(encoding="utf-8") for name in names}


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _single_collect_wait_failures(sources: dict[str, str], name: str) -> list[str]:
    waits = QUEUE_WAIT.findall(sources[name])
    collect = _function_body(sources[name], "collect_fex_sycl")
    if len(waits) != 1 or len(QUEUE_WAIT.findall(collect)) != 1:
        return [f"{name}: the queue must be waited on exactly once, in collect_fex_sycl"]
    return []


EXACT_FP_TUS = (
    "speed_chroma_sycl",
    "speed_temporal_sycl",
    "speed_sycl_pipeline",
    "speed_sycl_host",
    "ssimulacra2_sycl",
)


FEATURE_TU_COMMAND = "+ sycl_feature_tail_args + sycl_feature_inc"
STRICT_TAIL = "sycl_feature_tail_args = ['-std=c++20'] + sycl_strict_fp_args"


def _exact_fp_build_failures(meson: str) -> list[str]:
    """Every TU that relies on sycl_exact_fp.h is built with contraction off.

    ADR-1367: the strict FP line (`sycl_strict_fp_args`, whose flags
    test_strict_fp_compiler_args.py pins) reaches every SYCL feature TU through
    `sycl_feature_tail_args`, so an exact-fp TU is covered by being a feature
    source. A per-TU flag list next to it would be a second policy.
    """
    sources = re.search(r"sycl_feature_sources\s*=\s*\[(.*?)\n\s*\]", meson, re.S)
    listed = set(re.findall(r"sycl/(\w+)\.cpp'", sources.group(1))) if sources else set()
    missing = [name for name in EXACT_FP_TUS if name not in listed]
    loop = meson[meson.find("foreach src : sycl_feature_sources") :]
    loop = loop[: loop.find("\n    endforeach\n")]  # the loop's own, not the AOT-list one
    failures: list[str] = []
    if missing:
        failures.append(f"core/src/meson.build: exact-fp TUs not built as feature TUs: {missing}")
    if STRICT_TAIL not in meson or FEATURE_TU_COMMAND not in loop:
        failures.append("core/src/meson.build: feature TUs lost the SYCL strict FP line")
    if "extra_args" in loop:
        failures.append("core/src/meson.build: per-TU FP arguments beside the strict FP line")
    return failures


def _device_resident_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for helper in SSIMULACRA2_HOST_RESIDUAL:
        if helper.search(sources[SSIMULACRA2]):
            failures.append(f"{SSIMULACRA2}: host residual {helper.pattern} reintroduced")
    failures += _single_collect_wait_failures(sources, SSIMULACRA2)
    failures += _single_collect_wait_failures(sources, MS_SSIM)
    return failures


def _speed_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if re.search(r"\bdouble\b", sources[EXACT_FP_HEADER]):
        failures.append(f"{EXACT_FP_HEADER}: fp64 type appears in the shared device helpers")
    if re.search(r"\bdouble\b", sources[SPEED_PIPELINE]):
        failures.append(f"{SPEED_PIPELINE}: fp64 type appears in the device pipeline")
    for name in (SPEED_PIPELINE, *SPEED_HOST_TUS):
        for helper in SPEED_HOST_RESIDUAL:
            if helper.search(sources[name]):
                failures.append(f"{name}: host residual {helper.pattern} reintroduced")
    # ADR-1380 / Research-1379: the fp32-pair log2 misrounds 48 floats; the
    # shared table in feature/speed_log2_hard_cases.h corrects them.
    if "return speed_log2_hard_case(" not in _function_body(sources[SPEED_PIPELINE], "speed_log2"):
        failures.append(f"{SPEED_PIPELINE}: speed_log2 no longer applies the log2 hard cases")
    # lanczos4 prescale: the CPU scaler evaluates each weight in fp64 with sin()
    # and rounds once; no fp32 device sine reproduces that, and SpEED amplifies
    # the last-bit differences. The host builds the weight table with the
    # scaler's own routine and the scale kernel reads it from device memory,
    # which also keeps the kernel free of private arrays (ADR-1395;
    # T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30).
    pipeline = re.sub(r"/\*.*?\*/|//[^\n]*", " ", sources[SPEED_PIPELINE], flags=re.S)
    if re.search(r"\bsycl::(?:sin|cos|sincos)(?:pi)?\s*\(", pipeline):
        failures.append(f"{SPEED_PIPELINE}: a device sine evaluates a prescale weight")
    if "speed_internal_gpu_lanczos_weights(" not in pipeline:
        failures.append(f"{SPEED_PIPELINE}: the lanczos4 weight table is not built on the host")
    if re.search(r"\bfloat\s+w[xy]\[9\]", pipeline):
        failures.append(f"{SPEED_PIPELINE}: lanczos4 weights held in a private array")
    for name in SPEED_HOST_TUS:
        if re.search(r"\b(?:parallel_for|single_task)\b", sources[name]):
            failures.append(f"{name}: device kernel outside {SPEED_PIPELINE}")
        if re.search(r"\.wait(?:_and_throw)?\(", sources[name]):
            failures.append(f"{name}: host wait outside the pipeline collect")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = _speed_failures(sources)
    failures += _device_resident_failures(sources)

    float_psnr = sources["float_psnr_sycl.cpp"]
    if "FpsnrOutput output" not in float_psnr or "output.partials" not in float_psnr:
        failures.append("float_psnr_sycl.cpp: output pointer lacks its kernel-argument struct")

    integer_psnr = sources["integer_psnr_sycl.cpp"]
    if "PsnrKernelArgs args" not in integer_psnr or "*args.sse" not in integer_psnr:
        failures.append("integer_psnr_sycl.cpp: output pointer lacks its kernel-argument struct")

    moment = sources["integer_moment_sycl.cpp"]
    if "int64_t *const e_sums = d_sums;" not in moment:
        failures.append("integer_moment_sycl.cpp: missing explicit e_sums capture alias")
    if moment.count("atomic64(e_sums[") != MOMENT_OUTPUT_COUNT or "atomic64(d_sums[" in moment:
        failures.append("integer_moment_sycl.cpp: kernel uses the raw d_sums parameter")

    ms_ssim = sources.get("integer_ms_ssim_sycl.cpp", "")
    if "for (unsigned plane = 0; plane < MS_SSIM_MAX_PLANES; plane++)" not in ms_ssim:
        failures.append(
            "integer_ms_ssim_sycl.cpp: free_ms_ssim_pyramid must use bounded MS_SSIM_MAX_PLANES plane loop"
        )
    return failures


# T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29 / ADR-1371: both SYCL motion twins
# run the one diff-first SAD kernel of the pipeline TU, which differences
# prev - cur before the blur like integer_motion.c and stays fp64-free; the
# extractor TUs hold no kernel, and motion_sycl's submit() never waits on the
# device (T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29).
MOTION_PIPELINE = "integer_motion_pipeline_sycl.cpp"
MOTION_TUS = ("integer_motion_sycl.cpp", "integer_motion_v2_sycl.cpp")
MOTION_DIFF_FIRST = re.compile(
    r"read_sample\(args\.prev[^;]*?\)\s*-\s*read_sample\(args\.cur", re.S
)
MOTION_SUBMIT_FNS = ("submit_fex_sycl", "motion_stage_chroma", "motion_stage_plane")


def _motion_sources() -> dict[str, str]:
    return {
        name: (SYCL_ROOT / name).read_text(encoding="utf-8")
        for name in (MOTION_PIPELINE, *MOTION_TUS)
    }


MOTION_WAIT = re.compile(r"\bvmaf_sycl_(?:queue_wait|memcpy_h2d_async)\(|\.wait(?:_and_throw)?\(")


def _motion_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    pipeline = sources[MOTION_PIPELINE]
    if re.search(r"\bdouble\b", pipeline):
        failures.append(f"{MOTION_PIPELINE}: fp64 type appears in the motion kernel")
    if not MOTION_DIFF_FIRST.search(pipeline):
        failures.append(f"{MOTION_PIPELINE}: the tile no longer stages prev - cur before the blur")
    for name in MOTION_TUS:
        if re.search(r"\b(?:parallel_for|single_task)\b", sources[name]):
            failures.append(f"{name}: device kernel outside {MOTION_PIPELINE}")
    motion = sources["integer_motion_sycl.cpp"]
    for fn in MOTION_SUBMIT_FNS:
        body = _function_body(motion, fn)
        if not body:
            failures.append(f"integer_motion_sycl.cpp: {fn}() not found")
        elif MOTION_WAIT.search(body):
            failures.append(
                f"integer_motion_sycl.cpp: {fn}() waits on or uploads through the primary queue"
            )
    return failures


# ADR-1409 / ADR-1411: float_motion_sycl returns the CPU extractor's bits. The
# CPU adds the absolute differences of a row into one float and the rows into
# another; the order is the result, so the row kernel is one plain loop per
# work-item and the host tail is the shared helper.
FLOAT_MOTION = "float_motion_sycl.cpp"
FLOAT_MOTION_SAD = "float_motion_sad.h"
FLOAT_MOTION_SAD_PATH = ROOT / "core" / "src" / "feature" / FLOAT_MOTION_SAD
FLOAT_MOTION_ROW_PIECES = (
    "const float *cur = args.cur_blur + y * args.width;",
    "const float *prev = args.prev_blur + y * args.width;",
    "float accum = 0.0f;",
    "for (unsigned j = 0; j < args.width; j++) {",
    "const float diff = cur[j] - prev[j];",
    "accum += diff < 0.0f ? -diff : diff;",
)
FLOAT_MOTION_ROW_LAUNCH = "cgh.parallel_for(sycl::range<1>(args.height),"
FLOAT_MOTION_HOST_TAIL = "vmaf_float_motion_score_from_row_sads(s->h_row_sad, s->width, s->height)"
# Any of these in the TU adds the SAD in an order the CPU does not use.
FLOAT_MOTION_OTHER_REDUCTION = re.compile(
    r"reduce_over_group|sycl::reduction|atomic_ref|joint_reduce|get_sub_group"
)


def _float_motion_sources() -> dict[str, str]:
    return {
        FLOAT_MOTION: (SYCL_ROOT / FLOAT_MOTION).read_text(encoding="utf-8"),
        FLOAT_MOTION_SAD: FLOAT_MOTION_SAD_PATH.read_text(encoding="utf-8"),
    }


def _code(source: str) -> str:
    """`source` without comments, so prose cannot satisfy or trip a check."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", source, flags=re.S)


def _float_motion_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    twin = _code(sources[FLOAT_MOTION])
    row = _function_body(twin, "fm_row_sad")
    for piece in FLOAT_MOTION_ROW_PIECES:
        if piece not in row:
            failures.append(f"{FLOAT_MOTION}: fm_row_sad() is not the CPU's row sum ({piece})")
    if FLOAT_MOTION_ROW_LAUNCH not in twin:
        failures.append(f"{FLOAT_MOTION}: the row SAD is not launched as one work-item per row")
    if FLOAT_MOTION_OTHER_REDUCTION.search(twin):
        failures.append(
            f"{FLOAT_MOTION}: a group, sub-group or atomic reduction adds the SAD in an "
            "order the CPU does not use"
        )
    if re.search(r"\bdouble\b", row):
        failures.append(f"{FLOAT_MOTION}: fp64 type in the row SAD kernel")
    collect = _function_body(twin, "collect_fex_sycl")
    if FLOAT_MOTION_HOST_TAIL not in collect or "+=" in collect:
        failures.append(
            f"{FLOAT_MOTION}: collect must take the shared "
            "vmaf_float_motion_score_from_row_sads() value and keep no sum of its own"
        )
    helper = _code(sources[FLOAT_MOTION_SAD])
    for piece in (
        "float accum = 0.0f;",
        "accum += row_sad[i];",
        "return (double)(accum / (float)(int)(w * h));",
    ):
        if piece not in helper:
            failures.append(f"{FLOAT_MOTION_SAD}: not the CPU's sum over the rows ({piece})")
    return failures


# ADR-1395: no SYCL kernel uses scratch memory. The ratchet list of kernels
# that still did reached zero when float_adm_sycl's contrast-masking kernels
# stopped indexing a private array with their run-time band; it stays empty,
# and those kernels keep choosing the band by value.
FLOAT_ADM = "float_adm_sycl.cpp"
SCRATCH_RATCHET_PATH = ROOT / "core" / "src" / "sycl" / "scratch_ratchet.txt"
SCRATCH_CHECK_PATH = ROOT / "core" / "src" / "sycl" / "scratch_check.cpp"
FLOAT_ADM_CM_HELPERS = ("fadm_load_cm_pixel", "fadm_aim_cm_term", "fadm_csf_cm_terms")
RUN_TIME_BAND_INDEX = re.compile(r"\[\s*(?:\(int\)\s*)?band\s*\]")


def _scratch_sources() -> dict[str, str]:
    return {
        FLOAT_ADM: (SYCL_ROOT / FLOAT_ADM).read_text(encoding="utf-8"),
        "scratch_ratchet.txt": SCRATCH_RATCHET_PATH.read_text(encoding="utf-8"),
        "scratch_check.cpp": SCRATCH_CHECK_PATH.read_text(encoding="utf-8"),
    }


def _scratch_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    listed = [
        line
        for line in sources["scratch_ratchet.txt"].splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if listed:
        failures.append(
            f"scratch_ratchet.txt: the list was empty and names {len(listed)} kernel(s) again"
        )
    if 'kScratchExtractors = "";' not in sources["scratch_check.cpp"]:
        failures.append("scratch_check.cpp: kScratchExtractors names an extractor again")
    twin = _code(sources[FLOAT_ADM])
    for helper in FLOAT_ADM_CM_HELPERS:
        body = _function_body(twin, helper)
        if not body:
            failures.append(f"{FLOAT_ADM}: {helper}() not found")
        elif RUN_TIME_BAND_INDEX.search(body):
            failures.append(
                f"{FLOAT_ADM}: {helper}() indexes a private array with the run-time band"
            )
    return failures


# ADR-1414: float_ms_ssim_sycl follows the CPU reference operation for
# operation. The per-pixel arithmetic lives in sycl_ssim_terms.h, shared with
# float_ssim_sycl, so the two twins cannot drift apart.
SSIM_TERMS_HEADER = "sycl_ssim_terms.h"
FLOAT_SSIM = "integer_ssim_sycl.cpp"
MS_SSIM_DECIMATE_PIECES = (
    "row_sum = sycl::fma(args.source[y * (int)args.width + x], LPF[horizontal], row_sum);",
    "sum = sycl::fma(row_sum, LPF[vertical], sum);",
)
MS_SSIM_KERNEL_PIECES = (
    "add_horizontal_tap(sums, args.ref[index], args.cmp[index], G[tap]);",
    "add_vertical_tap(sums, row, G[tap]);",
    "const SsimTerms terms = ssim_terms(round_moments(sums), args.c1, args.c2);",
    ".luminance = term_fixed(terms.luminance),",
    ".contrast = term_fixed(terms.contrast),",
    ".structure = term_fixed(Ff{.hi = terms.structure, .lo = 0.0f})",
    "sycl::reduce_over_group(item.get_group(), values.luminance, sycl::plus<std::int64_t>{});",
)
MS_SSIM_HOST_PIECES = (
    "luminance = (double)(float)(total_l.value() / pixels);",
    "contrast = (double)(float)(total_c.value() / pixels);",
    "structure = (double)(float)(total_s.value() / pixels);",
    "std::pow(std::fabs(luminance[scale]), (double)ALPHAS[scale])",
    "std::pow(std::fabs(contrast[scale]), (double)BETAS[scale])",
    "std::pow(std::fabs(structure[scale]), (double)GAMMAS[scale])",
)
# The CPU's operand types in the shared header: fp32 denominators, the l and c
# numerators as pairs, s as one correctly rounded fp32 quotient.
SSIM_TERMS_PIECES = (
    "const float product = sample * weight;",
    "return ff_add(sum, Ff{.hi = product, .lo = 0.0f});",
    "const float l_den = l_den_sum + c1;",
    "const float c_den = c_den_sum + c2;",
    "const Ff product = two_prod(m.reference_mean, m.comparison_mean);",
    "const Ff c_num = two_sum(2.0f * srsc, c2);",
    ".luminance = ff_div(l_num, Ff{.hi = l_den, .lo = 0.0f}),",
    ".contrast = ff_div(c_num, Ff{.hi = c_den, .lo = 0.0f}),",
    ".structure = div_rn(s_num, s_den)",
)
SSIM_TERMS_SHARED = ("ssim_terms(", "add_horizontal_tap(", "add_vertical_tap(", "term_fixed(")


def _ms_ssim_sources() -> dict[str, str]:
    return {
        name: (SYCL_ROOT / name).read_text(encoding="utf-8")
        for name in (MS_SSIM, FLOAT_SSIM, SSIM_TERMS_HEADER)
    }


def _ms_ssim_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    twin = _code(sources[MS_SSIM])
    decimate = _function_body(twin, "decimate_pixel")
    for piece in MS_SSIM_DECIMATE_PIECES:
        if piece not in decimate:
            failures.append(f"{MS_SSIM}: the decimate tap is not one fused multiply-add ({piece})")
    for piece in MS_SSIM_KERNEL_PIECES:
        if piece not in twin:
            failures.append(f"{MS_SSIM}: not the CPU's window or l / c / s arithmetic ({piece})")
    if re.search(r"\bfloat\s*\*\s*[dh]_partials\b", twin):
        failures.append(f"{MS_SSIM}: the l / c / s partials are fp32 sums again")
    for piece in MS_SSIM_HOST_PIECES:
        if piece not in twin:
            failures.append(f"{MS_SSIM}: the host no longer combines as the CPU does ({piece})")
    header = _code(sources[SSIM_TERMS_HEADER])
    for piece in SSIM_TERMS_PIECES:
        if piece not in header:
            failures.append(f"{SSIM_TERMS_HEADER}: not the CPU's operand types ({piece})")
    for name in (MS_SSIM, FLOAT_SSIM):
        source = _code(sources[name])
        if f'#include "{SSIM_TERMS_HEADER}"' not in sources[name]:
            failures.append(f"{name}: does not take the SSIM arithmetic from {SSIM_TERMS_HEADER}")
        for helper in SSIM_TERMS_SHARED:
            if re.search(rf"\binline\b[^;{{]*\b{re.escape(helper)}", source):
                failures.append(f"{name}: a private copy of {helper[:-1]}() beside the shared one")
    return failures


class SyclKernelSourceContractTest(unittest.TestCase):
    def test_live_sources_keep_fp32_and_capture_contracts(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_dropped_log2_hard_cases_are_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            "return speed_log2_hard_case(sycl::bit_cast<uint32_t>(x), rounded);",
            "return rounded;",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("log2 hard cases" in item for item in failures))

    def test_device_sine_for_lanczos_weights_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] += (
            "\ninline float lanczos_weight(float x)\n{\n    return sycl::sinpi(x);\n}\n"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("device sine" in item for item in failures), failures)

    def test_missing_lanczos_host_table_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            "speed_internal_gpu_lanczos_weights(", "local_lanczos_weights("
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("weight table" in item for item in failures), failures)

    def test_private_lanczos_weight_array_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] += "\ninline void weights()\n{\n    float wx[9];\n}\n"
        failures = _contract_failures(sources)
        self.assertTrue(any("private array" in item for item in failures), failures)

    def test_fp64_speed_regression_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] += "\nstruct Wide { double sum; };\n"
        failures = _contract_failures(sources)
        self.assertTrue(any(f"{SPEED_PIPELINE}: fp64 type" in item for item in failures))

    def test_fp64_exact_fp_header_regression_is_detected(self) -> None:
        sources = _sources()
        sources[EXACT_FP_HEADER] = sources[EXACT_FP_HEADER].replace(
            "struct Ff {", "struct Wide { double sum; };\nstruct Ff {", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any(f"{EXACT_FP_HEADER}: fp64 type" in item for item in failures))

    def test_exact_fp_tus_build_with_contraction_off(self) -> None:
        meson = (ROOT / "core" / "src" / "meson.build").read_text(encoding="utf-8")
        self.assertEqual(_exact_fp_build_failures(meson), [])
        unlisted = meson.replace("feature_src_dir + 'sycl/ssimulacra2_sycl.cpp',", "", 1)
        self.assertTrue(_exact_fp_build_failures(unlisted))
        no_strict = meson.replace(STRICT_TAIL, "sycl_feature_tail_args = ['-std=c++20']", 1)
        self.assertTrue(_exact_fp_build_failures(no_strict))
        per_tu = meson.replace(FEATURE_TU_COMMAND, "+ sycl_feature_tail_args + extra_args", 1)
        self.assertTrue(_exact_fp_build_failures(per_tu))

    def test_ssimulacra2_host_residual_regression_is_detected(self) -> None:
        sources = _sources()
        sources[SSIMULACRA2] += "\nss2s_host_combine(s, scale, avg_ssim, avg_ed);\n"
        self.assertTrue(any("host residual" in item for item in _contract_failures(sources)))

    def test_ssimulacra2_mid_frame_wait_is_detected(self) -> None:
        sources = _sources()
        sources[SSIMULACRA2] = sources[SSIMULACRA2].replace(
            "    launch_xyb(q, xyb);\n", "    launch_xyb(q, xyb);\n    q.wait();\n", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any(f"{SSIMULACRA2}: the queue" in item for item in failures))

    def test_ms_ssim_per_scale_wait_is_detected(self) -> None:
        sources = _sources()
        sources[MS_SSIM] = sources[MS_SSIM].replace(
            "            enqueue_scale_lcs(s, q, plane, scale);\n",
            "            enqueue_scale_lcs(s, q, plane, scale);\n            q.wait();\n",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any(f"{MS_SSIM}: the queue" in item for item in failures))

    def test_host_residual_regression_is_detected(self) -> None:
        sources = _sources()
        sources["speed_chroma_sycl.cpp"] += "\nspeed_internal_qr_factorize(a, 25, q, r, t);\n"
        self.assertTrue(any("host residual" in item for item in _contract_failures(sources)))

    def test_kernel_outside_pipeline_is_detected(self) -> None:
        sources = _sources()
        sources["speed_temporal_sycl.cpp"] += "\nq.parallel_for(range, kernel);\n"
        self.assertTrue(
            any("device kernel outside" in item for item in _contract_failures(sources))
        )

    def test_mid_frame_wait_is_detected(self) -> None:
        sources = _sources()
        sources["speed_chroma_sycl.cpp"] += "\nqueue.wait();\n"
        self.assertTrue(any("host wait" in item for item in _contract_failures(sources)))

    def test_raw_moment_capture_regression_is_detected(self) -> None:
        sources = _sources()
        moment = sources["integer_moment_sycl.cpp"].replace(
            "    int64_t *const e_sums = d_sums;\n", "", 1
        )
        sources["integer_moment_sycl.cpp"] = moment.replace("e_sums", "d_sums")
        failures = _contract_failures(sources)
        self.assertTrue(any("raw d_sums" in item for item in failures))

    def test_ms_ssim_pyramid_plane_loop_regression_is_detected(self) -> None:
        sources = _sources()
        sources["integer_ms_ssim_sycl.cpp"] = sources["integer_ms_ssim_sycl.cpp"].replace(
            "for (unsigned plane = 0; plane < MS_SSIM_MAX_PLANES; plane++)",
            "for (MsSsimPlaneGeometry &geometry : s->geom)",
            1,
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("MS_SSIM_MAX_PLANES plane loop" in item for item in failures))

    def test_live_motion_sources_keep_pipeline_contract(self) -> None:
        self.assertEqual(_motion_failures(_motion_sources()), [])

    def test_motion_blur_before_difference_is_detected(self) -> None:
        sources = _motion_sources()
        sources[MOTION_PIPELINE] = sources[MOTION_PIPELINE].replace(
            "read_sample(args.prev, offset, args.bpc) - read_sample(args.cur, offset, args.bpc)",
            "read_sample(args.cur, offset, args.bpc) - read_sample(args.prev, offset, args.bpc)",
            1,
        )
        self.assertTrue(any("prev - cur" in item for item in _motion_failures(sources)))

    def test_motion_kernel_in_extractor_tu_is_detected(self) -> None:
        sources = _motion_sources()
        sources["integer_motion_sycl.cpp"] += "\nq.parallel_for(range, kernel);\n"
        self.assertTrue(any("device kernel outside" in item for item in _motion_failures(sources)))

    def test_motion_submit_wait_is_detected(self) -> None:
        sources = _motion_sources()
        sources["integer_motion_sycl.cpp"] = sources["integer_motion_sycl.cpp"].replace(
            "    motion_stage_chroma(s, ref_pic);\n",
            "    motion_stage_chroma(s, ref_pic);\n    (void)vmaf_sycl_queue_wait(fex->sycl_state);\n",
            1,
        )
        self.assertTrue(any("primary queue" in item for item in _motion_failures(sources)))

    def test_motion_fp64_is_detected(self) -> None:
        sources = _motion_sources()
        sources[MOTION_PIPELINE] += "\nstatic double motion_scale;\n"
        self.assertTrue(any("fp64 type" in item for item in _motion_failures(sources)))

    def test_live_float_motion_adds_the_sad_in_cpu_order(self) -> None:
        self.assertEqual(_float_motion_failures(_float_motion_sources()), [])

    def _float_motion_edit(self, name: str, old: str, new: str) -> dict[str, str]:
        sources = _float_motion_sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return sources

    def test_group_reduced_float_motion_sad_is_detected(self) -> None:
        sources = _float_motion_sources()
        sources[FLOAT_MOTION] += (
            "\nstatic float fm_group_sum(sycl::nd_item<2> item, float v)\n"
            "{\n    return sycl::reduce_over_group(item.get_group(), v, sycl::plus<float>{});\n}\n"
        )
        failures = _float_motion_failures(sources)
        self.assertTrue(any("an order the CPU does not use" in item for item in failures))

    def test_strided_float_motion_row_sum_is_detected(self) -> None:
        sources = self._float_motion_edit(
            FLOAT_MOTION,
            "for (unsigned j = 0; j < args.width; j++) {",
            "for (unsigned j = lane; j < args.width; j += lanes) {",
        )
        failures = _float_motion_failures(sources)
        self.assertTrue(any("not the CPU's row sum" in item for item in failures))

    def test_blocked_float_motion_row_launch_is_detected(self) -> None:
        sources = self._float_motion_edit(
            FLOAT_MOTION,
            FLOAT_MOTION_ROW_LAUNCH,
            "cgh.parallel_for(sycl::range<2>(args.height, 4), [=](sycl::id<2> row) {",
        )
        failures = _float_motion_failures(sources)
        self.assertTrue(any("one work-item per row" in item for item in failures))

    def test_float_motion_host_sum_of_its_own_is_detected(self) -> None:
        sources = self._float_motion_edit(
            FLOAT_MOTION,
            "    const double motion_score =\n"
            "        vmaf_float_motion_score_from_row_sads(s->h_row_sad, s->width, s->height);\n",
            "    double motion_score = 0.0;\n"
            "    for (unsigned i = 0; i < s->height; i++) {\n"
            "        motion_score += (double)s->h_row_sad[i];\n"
            "    }\n",
        )
        failures = _float_motion_failures(sources)
        self.assertTrue(any("keep no sum of its own" in item for item in failures))

    def test_fp64_float_motion_row_total_is_detected(self) -> None:
        sources = self._float_motion_edit(
            FLOAT_MOTION_SAD, "float accum = 0.0f;", "double accum = 0.0;"
        )
        failures = _float_motion_failures(sources)
        self.assertTrue(any("not the CPU's sum over the rows" in item for item in failures))

    def test_live_ms_ssim_follows_the_cpu_arithmetic(self) -> None:
        self.assertEqual(_ms_ssim_failures(_ms_ssim_sources()), [])

    def _ms_ssim_edit(self, name: str, old: str, new: str) -> dict[str, str]:
        sources = _ms_ssim_sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return sources

    def _assert_ms_ssim_detected(self, sources: dict[str, str], needle: str) -> None:
        failures = _ms_ssim_failures(sources)
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_unfused_ms_ssim_decimate_tap_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "sum = sycl::fma(row_sum, LPF[vertical], sum);",
            "sum += row_sum * LPF[vertical];",
        )
        self._assert_ms_ssim_detected(sources, "one fused multiply-add")

    def test_fp32_ms_ssim_window_sum_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "add_horizontal_tap(sums, args.ref[index], args.cmp[index], G[tap]);",
            "ref_mu += G[tap] * args.ref[index];",
        )
        self._assert_ms_ssim_detected(sources, "window or l / c / s arithmetic")

    def test_fp32_ms_ssim_partials_are_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM, "std::int64_t *d_partials;", "float *d_partials;"
        )
        self._assert_ms_ssim_detected(sources, "fp32 sums again")

    def test_unrounded_ms_ssim_mean_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "luminance = (double)(float)(total_l.value() / pixels);",
            "luminance = total_l.value() / pixels;",
        )
        self._assert_ms_ssim_detected(sources, "combines as the CPU does")

    def test_ms_ssim_combine_without_fabs_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "std::pow(std::fabs(contrast[scale]), (double)BETAS[scale])",
            "std::pow(contrast[scale], (double)BETAS[scale])",
        )
        self._assert_ms_ssim_detected(sources, "combines as the CPU does")

    def test_fp32_ssim_luminance_quotient_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            SSIM_TERMS_HEADER,
            ".luminance = ff_div(l_num, Ff{.hi = l_den, .lo = 0.0f}),",
            ".luminance = Ff{.hi = div_rn(l_num.hi, l_den), .lo = 0.0f},",
        )
        self._assert_ms_ssim_detected(sources, "not the CPU's operand types")

    def test_private_ssim_terms_copy_is_detected(self) -> None:
        sources = _ms_ssim_sources()
        sources[MS_SSIM] += (
            "\ninline SsimTerms ssim_terms(const SsimMoments &m, float c1, float c2)\n"
            "{\n    return {};\n}\n"
        )
        self._assert_ms_ssim_detected(sources, "a private copy of ssim_terms()")

    def test_live_sources_use_no_scratch_ratchet(self) -> None:
        self.assertEqual(_scratch_failures(_scratch_sources()), [])

    def test_ratchet_entry_is_detected(self) -> None:
        sources = _scratch_sources()
        sources["scratch_ratchet.txt"] += "some_sycl\t64\t0\t_ZTSsome_kernel\n"
        failures = _scratch_failures(sources)
        self.assertTrue(any("names 1 kernel(s) again" in item for item in failures), failures)

    def test_named_scratch_extractor_is_detected(self) -> None:
        sources = _scratch_sources()
        sources["scratch_check.cpp"] = sources["scratch_check.cpp"].replace(
            'kScratchExtractors = "";', 'kScratchExtractors = "float_adm_sycl";', 1
        )
        failures = _scratch_failures(sources)
        self.assertTrue(any("names an extractor again" in item for item in failures), failures)

    def test_band_indexed_private_array_is_detected(self) -> None:
        sources = _scratch_sources()
        old = "fadm_restore(pixel.original, pixel.transformed, pixel.angle_flag, p.gain_limit);"
        self.assertIn(old, sources[FLOAT_ADM])
        sources[FLOAT_ADM] = sources[FLOAT_ADM].replace(
            old,
            "fadm_restore(pixel.original[band], pixel.transformed[band], pixel.angle_flag, "
            "p.gain_limit);",
        )
        failures = _scratch_failures(sources)
        self.assertTrue(any("run-time band" in item for item in failures), failures)

if __name__ == "__main__":
    unittest.main()

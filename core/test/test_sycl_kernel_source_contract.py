#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protect fp64-free SpEED kernels, device-resident twins and explicit SYCL output captures.

SpEED since ADR-1477: the device chain ends at the variances, the frame's one
readback is the tail block (status words, eigenvalues, variances), and
pipeline_collect() forms the entropies and the score after its wait with
speed_internal_gpu_tail_scores(), which holds Netflix's fp64 `log2()`
statements (libvmaf/src/feature/speed.c, update_entropy() at 796-806 and
get_speed_score() at 892-938 of 9e48141b). No kernel evaluates a logarithm,
and the Givens rotation's `1.0 / sqrt(1 + t * t)` (speed.c:418, :423) goes
through feature/speed_givens.h.

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


def _speed_upstream_form_failures(source: str) -> list[str]:
    """ADR-1477: Netflix's fp64 statements, where each of them runs."""
    failures: list[str] = []
    pipeline = re.sub(r"/\*.*?\*/|//[^\n]*", " ", source, flags=re.S)
    # update_entropy() and get_speed_score() call log2() on the host, in
    # speed_internal_gpu_tail_scores(); a device logarithm is another function.
    if re.search(r"\b(?:sycl::|std::|speed_)?log(?:2|10|1p)?f?\s*\(", pipeline):
        failures.append(f"{SPEED_PIPELINE}: a kernel evaluates a logarithm")
    if "launch_score" in pipeline or "entropy_constant" in pipeline:
        failures.append(f"{SPEED_PIPELINE}: the entropy or the score is formed on the device")
    if "speed_internal_gpu_tail_scores(" not in _function_body(
        source, "speed_sycl::pipeline_collect"
    ):
        failures.append(f"{SPEED_PIPELINE}: pipeline_collect() does not run the host tail")
    if "q.memcpy(p.tail_host, p.tail_device, p.tail_layout.bytes);" not in pipeline:
        failures.append(f"{SPEED_PIPELINE}: the readback is not the tail block")
    # create_givens(): `1.0 / sqrt(1 + t * t)` is an fp64 root and quotient.
    if (
        '#include "feature/speed_givens.h"' not in source
        or "const float unit = speed_givens_unit(1.0f + tt);" not in pipeline
    ):
        failures.append(f"{SPEED_PIPELINE}: create_givens() is not upstream's fp64 statement")
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
    failures += _speed_upstream_form_failures(sources[SPEED_PIPELINE])
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
# ADR-1478 / ADR-1491: one derivation of motion2 / motion3 from the stored
# SADs, integer_motion.c::vmaf_motion_window_flush(). motion_v2_sycl calls it
# for both windows and reads no stored score back; motion_sycl calls it for
# the five-frame window.
MOTION_WINDOW_CALL = "vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window)"
# ADR-1395: the 16-bit SAD kernel fits SIMD-16 with the default register file
# on every target of the default AOT list. Its old SIMD-32 shape asked for 256
# registers, which Xe-LP does not have: it spilled 3200 bytes on a UHD 770.
MOTION_HBD_SHAPE = "class MotionSadHbdKernel : public VmafSyclKernelShape<16, 0>"


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
    if MOTION_HBD_SHAPE not in pipeline:
        failures.append(
            f"{MOTION_PIPELINE}: the 16-bit SAD kernel is not SIMD-16 with the default "
            "register file (it spills on Xe-LP otherwise)"
        )
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
    failures.extend(_motion_window_failures(sources))
    return failures


def _motion_window_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in MOTION_TUS:
        if MOTION_WINDOW_CALL not in _code(sources[name]):
            failures.append(f"{name}: the window is not derived with the CPU's window function")
    if "vmaf_feature_collector_get_score" in _code(sources["integer_motion_v2_sycl.cpp"]):
        failures.append(
            "integer_motion_v2_sycl.cpp: the twin reads stored scores back, a window of its own"
        )
    return failures


# ADR-1395 / ADR-1501: the SIMD-16 horizontal vif kernel of scale 0 spilled on
# Xe-LP (384 B, UHD 770) and Xe-LPG (128 B) at a required SIMD-16 with the
# default register file. It leaves its size to the compiler with the large
# register file; the SIMD-32 instances keep VmafSyclKernelShape<32, 256>.
INTEGER_VIF = "integer_vif_sycl.cpp"
VIF_HORI_SHAPE_PIECES = (
    "return (scale == 0 && sg_size == 16) ? 0 : sg_size;",
    "return (scale == 0) ? 256 : vif_grf_size(sg_size);",
    "class IntegerVifHoriKernel : public VmafSyclKernelShape<vif_hori_sg_size(SCALE, SG_SIZE),",
    "vif_hori_grf_size(SCALE, SG_SIZE)>",
    "return (sg_size == 32) ? 256 : 0;",
)


def _vif_hori_shape_failures(source: str) -> list[str]:
    code = _code(source)
    return [
        f"{INTEGER_VIF}: the horizontal kernel's shape changed ({piece}); scale 0 at a "
        "required SIMD-16 spills on Xe-LP and Xe-LPG"
        for piece in VIF_HORI_SHAPE_PIECES
        if piece not in code
    ]


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
# stopped indexing a private array with their run-time band; it stays empty.
# Since ADR-1434 the per-work-item code of that twin lives in
# sycl_float_adm_math.h, where every helper takes its band as a constant and
# the three CSF weights are named scalars.
FLOAT_ADM = "sycl_float_adm_math.h"
SCRATCH_RATCHET_PATH = ROOT / "core" / "src" / "sycl" / "scratch_ratchet.txt"
SCRATCH_CHECK_PATH = ROOT / "core" / "src" / "sycl" / "scratch_check.cpp"
FLOAT_ADM_CM_HELPERS = ("store_csf", "neighbours", "threshold", "store_terms")
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
    bands = re.search(r"struct Bands \{(.*?)\};", twin, re.S)
    if not bands or "[" in bands.group(1):
        failures.append(f"{FLOAT_ADM}: struct Bands must hold named scalars, no array")
    return failures


# ADR-1414: float_ms_ssim_sycl follows the CPU reference operation for
# operation. The per-pixel arithmetic lives in sycl_ssim_terms.h, shared with
# float_ssim_sycl, so the two twins cannot drift apart. ADR-1466: l and c are
# the CPU's doubles (soft fp64), every window's terms are stored unreduced
# and the host adds them in iqa_ssim()'s raster order.
SSIM_TERMS_HEADER = "sycl_ssim_terms.h"
FLOAT_SSIM = "integer_ssim_sycl.cpp"
MS_SSIM_DECIMATE_PIECES = (
    "row_sum = sycl::fma(args.source[y * (int)args.width + x], LPF[horizontal], row_sum);",
    "sum = sycl::fma(row_sum, LPF[vertical], sum);",
)
MS_SSIM_KERNEL_PIECES = (
    "add_horizontal_tap(sums, args.ref[index], args.cmp[index], G[tap]);",
    "add_vertical_tap(sums, row, G[tap]);",
    "return ssim_double_terms(ssim_float_parts(round_moments(sums), args.c1, args.c2), args.c1,",
    "const size_t index = id[0] * (size_t)a_.final_width + id[1];",
    "a_.luminance[index] = vmaf_sycl_soft::signed_bits(terms.luminance);",
    "a_.contrast[index] = vmaf_sycl_soft::signed_bits(terms.contrast);",
    "a_.structure[index] = terms.structure;",
    "class MsSsimLcsKernel : public VmafSyclKernelShape<MS_SSIM_TERM_SG, MS_SSIM_TERM_GRF>",
    "constexpr int MS_SSIM_TERM_SG = 16;",
    "constexpr int MS_SSIM_TERM_GRF = 256;",
    "__attribute__((flatten, always_inline)) static inline SsimDoubleTerms",
    "ms_ssim_window_terms(const VertArgs &args, size_t x, size_t y)",
)
# The old twin's sums: pair terms in fixed point, reduced per work-group.
MS_SSIM_OLD_SUMS = ("reduce_over_group", "term_fixed(", "FixedSum", "_partials")
MS_SSIM_HOST_PIECES = (
    "ssim_lcs_sums(s->h_terms + offset, s->h_terms + s->window_count + offset,",
    "s->h_structure + offset, windows);",
    "luminance = (double)(float)(sums.luminance / pixels);",
    "contrast = (double)(float)(sums.contrast / pixels);",
    "structure = (double)(float)(sums.structure / pixels);",
    "std::pow(std::fabs(luminance[scale]), (double)ALPHAS[scale])",
    "std::pow(std::fabs(contrast[scale]), (double)BETAS[scale])",
    "std::pow(std::fabs(structure[scale]), (double)GAMMAS[scale])",
)
# The CPU's operand types in the shared header: fp32 denominators, l and c as
# one fp64 quotient each by the converted fp32 denominator, s as one correctly
# rounded fp32 quotient; and the host's three sums in index order.
SSIM_TERMS_PIECES = (
    "const float product = sample * weight;",
    "return ff_add(sum, Ff{.hi = product, .lo = 0.0f});",
    "const float l_den = l_den_sum + c1;",
    "const float c_den = c_den_sum + c2;",
    ".luminance = signed_div(l_num, signed_from_float(p.l_den)),",
    ".contrast = signed_div(c_num, signed_from_float(p.c_den)),",
    ".structure = div_rn(s_num, s_den)",
    "sums.luminance += std::bit_cast<double>(luminance[i]);",
    "sums.contrast += std::bit_cast<double>(contrast[i]);",
    "sums.structure += (double)structure[i];",
)
SSIM_TERMS_SHARED = (
    "ssim_double_terms(",
    "ssim_float_parts(",
    "add_horizontal_tap(",
    "add_vertical_tap(",
    "ssim_lcs_sums(",
)


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
    for name in MS_SSIM_OLD_SUMS:
        if name in twin:
            failures.append(
                f"{MS_SSIM}: the l / c / s sums are reduced on the device again ({name})"
            )
    for piece in MS_SSIM_HOST_PIECES:
        if piece not in twin:
            failures.append(f"{MS_SSIM}: the host no longer combines as the CPU does ({piece})")
    return failures + _ssim_terms_header_failures(sources)


def _ssim_terms_header_failures(sources: dict[str, str]) -> list[str]:
    """The shared SSIM terms header: the CPU's operand types, used by both twins."""
    failures: list[str] = []
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

    def test_speed_device_log2_is_detected(self) -> None:
        # The kernels' own log2 before ADR-1477: fp32 pairs rounded to float.
        sources = _sources()
        anchor = "    a.var[static_cast<size_t>(ch) * a.blocks + block] = variance;"
        self.assertIn(anchor, sources[SPEED_PIPELINE])
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            anchor, anchor.replace("= variance;", "= speed_log2(variance);"), 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("evaluates a logarithm" in item for item in failures))

    def test_speed_device_score_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] += "\nvoid launch_score(sycl::queue &q)\n{\n}\n"
        failures = _contract_failures(sources)
        self.assertTrue(any("formed on the device" in item for item in failures))

    def test_speed_dropped_host_tail_is_detected(self) -> None:
        sources = _sources()
        anchor = "return speed_internal_gpu_tail_scores("
        self.assertIn(anchor, sources[SPEED_PIPELINE])
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(anchor, "return local_scores(", 1)
        failures = _contract_failures(sources)
        self.assertTrue(any("host tail" in item for item in failures))

    def test_speed_readback_of_another_block_is_detected(self) -> None:
        sources = _sources()
        anchor = "q.memcpy(p.tail_host, p.tail_device, p.tail_layout.bytes);"
        self.assertIn(anchor, sources[SPEED_PIPELINE])
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            anchor, "q.memcpy(p.tail_host, p.cov, sizeof(float));", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("not the tail block" in item for item in failures))

    def test_speed_fp32_givens_is_detected(self) -> None:
        # Port #213's form: `1.0f / sqrtf(1.0f + t * t)`.
        sources = _sources()
        anchor = "const float unit = speed_givens_unit(1.0f + tt);"
        self.assertIn(anchor, sources[SPEED_PIPELINE])
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            anchor, "const float unit = div_rn(1.0f, sqrt_rn(1.0f + tt));", 1
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("create_givens()" in item for item in failures))

    def test_device_sine_for_lanczos_weights_is_detected(self) -> None:
        sources = _sources()
        sources[
            SPEED_PIPELINE
        ] += "\ninline float lanczos_weight(float x)\n{\n    return sycl::sinpi(x);\n}\n"
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

    def test_motion_own_window_is_detected(self) -> None:
        for name in MOTION_TUS:
            sources = _motion_sources()
            self.assertIn(MOTION_WINDOW_CALL, sources[name])
            sources[name] = sources[name].replace(
                MOTION_WINDOW_CALL, "motion_flush_scores(s, feature_collector, &window)", 1
            )
            self.assertTrue(
                any("the CPU's window function" in item for item in _motion_failures(sources)),
                name,
            )

    def test_motion_v2_reading_scores_back_is_detected(self) -> None:
        sources = _motion_sources()
        sources["integer_motion_v2_sycl.cpp"] += (
            "\nstatic int own_window(VmafFeatureCollector *fc, double *score)\n"
            '{\n    return vmaf_feature_collector_get_score(fc, "sad", score, 0u);\n}\n'
        )
        self.assertTrue(any("a window of its own" in item for item in _motion_failures(sources)))

    def test_motion_hbd_simd32_shape_is_detected(self) -> None:
        sources = _motion_sources()
        sources[MOTION_PIPELINE] = sources[MOTION_PIPELINE].replace(
            MOTION_HBD_SHAPE, "class MotionSadHbdKernel : public VmafSyclKernelShape<32, 256>", 1
        )
        self.assertTrue(any("spills on Xe-LP" in item for item in _motion_failures(sources)))

    def test_live_vif_hori_shape(self) -> None:
        source = (SYCL_ROOT / INTEGER_VIF).read_text(encoding="utf-8")
        self.assertEqual(_vif_hori_shape_failures(source), [])

    def test_vif_hori_required_simd16_is_detected(self) -> None:
        source = (SYCL_ROOT / INTEGER_VIF).read_text(encoding="utf-8")
        for old, new in (
            (VIF_HORI_SHAPE_PIECES[0], "return sg_size;"),
            (VIF_HORI_SHAPE_PIECES[1], "return vif_grf_size(sg_size);"),
        ):
            self.assertIn(old, source)
            failures = _vif_hori_shape_failures(source.replace(old, new, 1))
            self.assertTrue(any("spills on Xe-LP" in item for item in failures), old)

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

    def test_ms_ssim_group_reduction_is_detected(self) -> None:
        # The pre-ADR-1466 store_lcs_group().
        sources = _ms_ssim_sources()
        sources[MS_SSIM] += (
            "\nstatic std::int64_t r(sycl::nd_item<2> item, std::int64_t value)\n{\n"
            "    return sycl::reduce_over_group(item.get_group(), value,"
            " sycl::plus<std::int64_t>{});\n}\n"
        )
        self._assert_ms_ssim_detected(sources, "reduced on the device again")

    def test_ms_ssim_partials_are_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM, "std::uint64_t *d_terms;", "std::int64_t *d_partials;"
        )
        self._assert_ms_ssim_detected(sources, "reduced on the device again")

    def test_ms_ssim_unstored_contrast_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "a_.contrast[index] = vmaf_sycl_soft::signed_bits(terms.contrast);",
            "",
        )
        self._assert_ms_ssim_detected(sources, "window or l / c / s arithmetic")

    def test_ms_ssim_wider_sub_group_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM, "constexpr int MS_SSIM_TERM_SG = 16;", "constexpr int MS_SSIM_TERM_SG = 32;"
        )
        self._assert_ms_ssim_detected(sources, "window or l / c / s arithmetic")

    def test_ms_ssim_sum_of_another_span_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "ssim_lcs_sums(s->h_terms + offset, s->h_terms + s->window_count + offset,",
            "ssim_lcs_sums(s->h_terms + offset, s->h_terms + windows + offset,",
        )
        self._assert_ms_ssim_detected(sources, "combines as the CPU does")

    def test_reordered_ms_ssim_host_sum_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            SSIM_TERMS_HEADER,
            "sums.contrast += std::bit_cast<double>(contrast[i]);",
            "sums.contrast += std::bit_cast<double>(contrast[count - 1U - i]);",
        )
        self._assert_ms_ssim_detected(sources, "not the CPU's operand types")

    def test_unrounded_ms_ssim_mean_is_detected(self) -> None:
        sources = self._ms_ssim_edit(
            MS_SSIM,
            "luminance = (double)(float)(sums.luminance / pixels);",
            "luminance = sums.luminance / pixels;",
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
            ".luminance = signed_div(l_num, signed_from_float(p.l_den)),",
            ".luminance = signed_from_float(div_rn(p.reference_mean, p.l_den)),",
        )
        self._assert_ms_ssim_detected(sources, "not the CPU's operand types")

    def test_private_ssim_terms_copy_is_detected(self) -> None:
        sources = _ms_ssim_sources()
        sources[MS_SSIM] += (
            "\ninline SsimFloatParts ssim_float_parts(const SsimMoments &m, float c1, float c2)\n"
            "{\n    return {};\n}\n"
        )
        self._assert_ms_ssim_detected(sources, "a private copy of ssim_float_parts()")

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
        old = "        den_term(rfactor, src, a.is_cube, a.p_norm);"
        self.assertIn(old, sources[FLOAT_ADM])
        sources[FLOAT_ADM] = sources[FLOAT_ADM].replace(
            old, "        den_term(bd.rfactor[band], src, a.is_cube, a.p_norm);"
        )
        failures = _scratch_failures(sources)
        self.assertTrue(any("run-time band" in item for item in failures), failures)

    def test_csf_weight_array_is_detected(self) -> None:
        sources = _scratch_sources()
        old = "    float rfactor_h;\n    float rfactor_v;\n    float rfactor_d;\n"
        self.assertIn(old, sources[FLOAT_ADM])
        sources[FLOAT_ADM] = sources[FLOAT_ADM].replace(old, "    float rfactor[3];\n")
        failures = _scratch_failures(sources)
        self.assertTrue(any("named scalars" in item for item in failures), failures)


if __name__ == "__main__":
    unittest.main()

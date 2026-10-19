#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the HIP RC3 parity design at the source level, device-free.

ADR-1377 (motion), ADR-1381 (tiny-frame guards) and ADR-1382 (CPU option
parity) move HIP twins onto the CPU's arithmetic. No AMD device runs in CI, so
this test reads the sources and checks the load-bearing shapes: the motion
kernel differences prev - cur before it filters and rounds after each pass,
the motion twins keep raw frames and wait on the host only in collect(), the
tile loads and the ADM scale-0 vertical DWT clamp their indices, vif_hip
declares its CPU fallback, and the option twins call the CPU's shared helpers
instead of copies of the math. ADR-1403 makes float_ms_ssim_hip the CPU's
arithmetic: the kernels and the extractor compute through
integer_ms_ssim/ms_ssim_arith.h (fused decimate taps, window sums as an exact
fp32 pair, the CPU's fp32 denominators and quotient, fp32 per-scale means)
and keep no arithmetic of their own. ADR-1491: the motion twins derive
motion2 / motion3 of the five-frame window, and motion_v2_hip of both
windows, with the CPU's vmaf_motion_window_flush(); neither holds a window
of its own. Every check has a planted-regression case that reintroduces the
old code and must be detected.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HIP_FEATURE = ROOT / "core" / "src" / "feature" / "hip"
HIP_RUNTIME = ROOT / "core" / "src" / "hip"

MOTION_KERNEL = "integer_motion_v2/motion_v2_score.hip"
MOTION_SAD = "integer_motion_sad_hip.c"
# The one derivation of motion2 / motion3 from the stored SADs (ADR-1478, ADR-1491).
MOTION_WINDOW_CALL = "vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window)"
MOTION_TUS = ("integer_motion_hip.c", "integer_motion_v2_hip.c")
MOTION_LAUNCH_FNS = {
    "integer_motion_hip.c": "msh_launch",
    "integer_motion_v2_hip.c": "mv2_hip_launch",
}
ADM_KERNEL = "integer_adm/adm_dwt2.hip"
ADM_ROWS = "integer_adm/adm_dwt2_rows.h"
TILE_INDEX = "hip_tile_index.h"
VIF_HOST = "integer_vif_hip.c"
PSNR_HOST = "integer_psnr_hip.c"
ISSIM_HOST = "integer_ssim_hip.c"
ISSIM_KERNEL = "integer_ssim/integer_ssim_score.hip"
FSSIM_HOST = "float_ssim_hip.c"
FSSIM_KERNEL = "float_ssim/ssim_score.hip"
FSSIM_DECIMATE = "float_ssim/ssim_decimate.h"
FMOTION_HOST = "float_motion_hip.c"
FMOTION_KERNEL = "float_motion/float_motion_score.hip"
MS_ARITH = "integer_ms_ssim/ms_ssim_arith.h"
MS_KERNEL = "integer_ms_ssim/ms_ssim_score.hip"
MS_HOST = "integer_ms_ssim_hip.c"
PICTURE = "picture_hip.c"
# What makes float_ms_ssim_hip the CPU's arithmetic (ADR-1403).
MS_ARITH_PIECES = (
    "row_acc = fmaf(row[xi], vmaf_hip_ms_ssim_lpf_tap(ku), row_acc);",
    "acc = fmaf(row_acc, vmaf_hip_ms_ssim_lpf_tap(kv), acc);",
    "vmaf_hip_ms_pair_add(&sums.ref_mu, r * tap);",
    "vmaf_hip_ms_pair_add(&sums.ref_sq, r_sq * tap);",
    "vmaf_hip_ms_pair_add(&sums.refcmp, r_c * tap);",
    "vmaf_hip_ms_pair_add(&sums.ref_mu, planes->ref_mu[idx] * tap);",
    "const float error = (sum->hi - hi_virtual) + (term - term_virtual);",
    "sum->lo = sum->lo + error;",
    "return sum->hi + sum->lo;",
    "const float ref_var = (0.0f > ref_diff) ? 0.0f : ref_diff;",
    "const float sigma = sqrtf(ref_var * cmp_var);",
    "(double)(ref_mu * ref_mu + cmp_mu * cmp_mu + c1);",
    "(double)(ref_var + cmp_var + c2);",
    "out.s = (double)((clamped_covar + c3) / (sigma + c3));",
    "*c1 = (K1 * (float)L) * (K1 * (float)L);",
    "*c3 = *c2 / 2.0f;",
    "return (double)(float)(sum / samples);",
    "pow(fabs(l_means[i]), (double)alphas[i])",
    "pow(fabs(c_means[i]), (double)betas[i])",
)
# The kernels and the extractor go through the header for every sample.
MS_KERNEL_CALLS = (
    "vmaf_hip_ms_ssim_decimate_sample(src, (int)w, (int)h, (int)x_out, (int)y_out);",
    "vmaf_hip_ms_ssim_horizontal(ref_in + src_idx, cmp_in + src_idx);",
    "vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);",
    "vmaf_hip_ms_ssim_lcs(&stats, (float)c1, (float)c2, (float)c3);",
)
MS_HOST_CALLS = (
    "vmaf_hip_ms_ssim_constants(&c1, &c2, &c3);",
    "l_means[i] = vmaf_hip_ms_ssim_scale_mean(total_l, n_pix);",
    "c_means[i] = vmaf_hip_ms_ssim_scale_mean(total_c, n_pix);",
    "s_means[i] = vmaf_hip_ms_ssim_scale_mean(total_s, n_pix);",
    "vmaf_hip_ms_ssim_combine(l_means, c_means, s_means);",
)
# float_ssim_hip goes through the same header for both window passes and for
# l / c / s (ADR-1441).
FSSIM_KERNEL_CALLS = (
    "vmaf_hip_ms_ssim_horizontal(ref_taps, cmp_taps);",
    "vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);",
    "vmaf_hip_ms_ssim_lcs(&m, c1, c2, c2 / 2.0f);",
)
# `x += a * b`: neither the reference's fused decimate tap nor its fp64
# window sum.
MULTIPLY_ACCUMULATE = re.compile(r"\b[\w.>-]+ \+= [^;]*\*[^;]*;")
# Arithmetic the float_ms_ssim kernels or the extractor would have to own.
MS_OWN_MATH = re.compile(r"\b(?:fmaf?|sqrtf?|pow)\s*\(")

# A host wait or a synchronous copy: none may run between a frame's submit()
# and its collect().
HOST_WAIT = re.compile(
    r"\b(?:hipStreamSynchronize|hipEventSynchronize|hipDeviceSynchronize|hipMemcpy|"
    r"hipMemcpy2D|vmaf_hip_picture_upload|vmaf_hip_kernel_collect_wait)\s*\("
)
# float_ssim decimation: the window index on both axes, and one window sum
# per plane (reference, comparison) in the kernel.
SSIM_DECIMATE_AXES = 2
SSIM_PLANES = 2
MOTION_DIFF_FIRST = re.compile(r"mv2_sample<T>\(prev[^;]*?\)\s*-\s*mv2_sample<T>\(cur", re.S)
TILE_CLAMP = re.compile(r"vmaf_hip_tile_index\(\s*vmaf_hip_reflect_101\(")
VERTICAL_ROUND = re.compile(r"\(\s*sum\s*\+\s*round_y\s*\)\s*>>\s*shift_y")
HORIZONTAL_ROUND = re.compile(r"\(\s*blurred\s*\+\s*\(\(int64_t\)1 << 15\)\s*\)\s*>>\s*16")
# The motion tile loader clamps both axes: x and y.
TILE_AXES = 2
# float_motion: both axes of the one tile loader its kernels share.
FM_TILE_LOADS = 2
# float_motion: motion3 from collect() and from the flush() tail.
FM_MOTION3_BLEND_EMITS = 2
# float_motion (ADR-1409): the SAD in the CPU's order. The kernels and the
# extractor compute through this header.
FMOTION_ROWS = "float_motion/float_motion_rows.h"
FMOTION_ROWS_PIECES = (
    "return (diff < 0.0f) ? -diff : diff;",
    "return ((group * width) + x) * VMAF_HIP_FLOAT_MOTION_ROW_GROUP + lane;",
    "float accum = 0.0f;",
    "accum += row[(size_t)j * VMAF_HIP_FLOAT_MOTION_ROW_GROUP];",
    "float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);",
    "score += (float)vmaf_float_motion_score_from_row_sads(",
    "return (double)score;",
)
FMOTION_KERNEL_CALLS = (
    "vmaf_hip_float_motion_abs_diff(blurred, prev_blur[off]);",
    "diff[vmaf_hip_float_motion_diff_index(x, y, width)] =",
    "vmaf_hip_float_motion_scale1_abs_diff(&scale1, x, y);",
    "diff[vmaf_hip_float_motion_diff_index(x, y, scaled_width)] =",
    "row_sad[y] = vmaf_hip_float_motion_row_sum(diff, width, y);",
)


def _sources() -> dict[str, str]:
    names = (
        MOTION_KERNEL,
        MOTION_SAD,
        *MOTION_TUS,
        ADM_KERNEL,
        ADM_ROWS,
        TILE_INDEX,
        VIF_HOST,
        PSNR_HOST,
        ISSIM_HOST,
        ISSIM_KERNEL,
        FSSIM_HOST,
        FSSIM_KERNEL,
        FSSIM_DECIMATE,
        FMOTION_HOST,
        FMOTION_KERNEL,
        MS_ARITH,
        MS_KERNEL,
        MS_HOST,
    )
    sources = {name: (HIP_FEATURE / name).read_text(encoding="utf-8") for name in names}
    sources[PICTURE] = (HIP_RUNTIME / PICTURE).read_text(encoding="utf-8")
    sources[FMOTION_ROWS] = (HIP_FEATURE / FMOTION_ROWS).read_text(encoding="utf-8")
    return sources


def _function_body(source: str, name: str) -> str:
    """The definition of C function `name`, signature to closing brace."""
    match = re.search(
        rf"^(?:static )?[A-Za-z_][\w \*]*\b{name}\((?:[^;{{]|\n)*?\)\s*\{{.*?^}}$",
        source,
        re.S | re.M,
    )
    return match.group(0) if match else ""


DRAIN_CALL = re.compile(r"^.*\b\w+_drain_after_error\(.*$", re.M)


def _drains_only_on_error_returns(body: str) -> bool:
    """Every call of an error-path drain helper is a `return` statement."""
    return all(line.strip().startswith("return ") for line in DRAIN_CALL.findall(body))


# The SAD reads the kept plane as `prev`; the copy that replaces it with this
# frame's luma is enqueued behind the SAD on the same stream (ADR-1408).
SAD_READS_KEPT = "const void *prev = f->keep;"
SAD_LAUNCH_CALL = "motion_sad_launch(k, frame, str)"
KEEP_COPY_CALL = "hipMemcpyAsync(frame->keep, frame->cur,"


def _staging_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in MOTION_TUS:
        text = src[name]
        if re.search(r"s->frame_[wh]\s*=", _function_body(text, "submit_fex_hip")):
            failures.append(f"{name}: submit() rewrites the geometry init() sized the buffers for")
    submit = _function_body(src[MOTION_SAD], "vmaf_hip_motion_sad_submit")
    staged = _function_body(src[PICTURE], "vmaf_hip_picture_upload_staged")
    launch_at = submit.find(SAD_LAUNCH_CALL)
    keep_at = submit.find(KEEP_COPY_CALL)
    if SAD_READS_KEPT not in src[MOTION_SAD] or launch_at < 0 or keep_at < launch_at:
        failures.append(
            f"{MOTION_SAD}: the kept plane is replaced before the SAD read the previous frame"
        )
    if "motion_sad_drain_after_error(" not in submit or "hip_pic_drain_after_error(" not in staged:
        failures.append("a failed enqueue returns while earlier work still uses the buffers")
    if not _drains_only_on_error_returns(submit) or not _drains_only_on_error_returns(staged):
        failures.append("an error-path drain is reachable outside an error return")
    return failures


def _motion_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    kernel = src[MOTION_KERNEL]
    if not MOTION_DIFF_FIRST.search(kernel):
        failures.append(f"{MOTION_KERNEL}: the tile no longer stages prev - cur before the blur")
    if not VERTICAL_ROUND.search(kernel) or not HORIZONTAL_ROUND.search(kernel):
        failures.append(f"{MOTION_KERNEL}: the vertical or horizontal pass lost the CPU rounding")
    if len(TILE_CLAMP.findall(kernel)) != TILE_AXES:
        failures.append(f"{MOTION_KERNEL}: a tile load reflects without the index clamp")
    for name in MOTION_TUS:
        text = src[name]
        if re.search(r"\bblur\[|motion_score_hsaco", text):
            failures.append(f"{name}: blurred-frame ping-pong or the blur-then-diff kernel is back")
        if "hipModuleLaunchKernel" in text:
            failures.append(f"{name}: launches a kernel outside {MOTION_SAD}")
        body = _function_body(text, MOTION_LAUNCH_FNS[name])
        if not body:
            failures.append(f"{name}: {MOTION_LAUNCH_FNS[name]}() not found")
        elif HOST_WAIT.search(body) or "vmaf_hip_motion_sad_submit" not in body:
            failures.append(
                f"{name}: the frame is not staged through the SAD pipeline without a wait"
            )
        elif "vmaf_hip_plane_source_acquire_luma(" not in body:
            failures.append(f"{name}: the frame's luma does not come from the shared frame")
        if "vmaf_hip_kernel_collect_wait" not in _function_body(text, "collect_fex_hip"):
            failures.append(f"{name}: collect() no longer holds the frame's one wait")
    submit = _function_body(src[MOTION_SAD], "vmaf_hip_motion_sad_submit")
    if not submit or HOST_WAIT.search(submit) or "vmaf_hip_picture_upload" in submit:
        failures.append(f"{MOTION_SAD}: the SAD pipeline waits on the host or uploads a picture")
    staged = _function_body(src[PICTURE], "vmaf_hip_picture_upload_staged")
    if not staged or re.search(r"Synchronize\s*\(", staged):
        failures.append(f"{PICTURE}: the staged upload waits on the host")
    failures += _motion_hip_failures(src["integer_motion_hip.c"])
    failures += _motion_v2_failures(src["integer_motion_v2_hip.c"])
    return failures


def _motion_hip_failures(motion: str) -> list[str]:
    failures: list[str] = []
    if not re.search(r"\"VMAF_integer_feature_motion_score\",\s*motion_clip_hip\(", motion):
        failures.append("integer_motion_hip.c: the debug motion score skips motion_clip")
    if not re.search(r"\"VMAF_integer_feature_motion_sad_score\",\s*motion_clip_hip\(", motion):
        failures.append("integer_motion_hip.c: the CPU's motion_sad_score is not emitted")
    if not re.search(r'\.name = "debug"[^}]*\.default_val\.b = false', motion):
        failures.append("integer_motion_hip.c: `debug` no longer defaults to false like the CPU")
    if MOTION_WINDOW_CALL not in _code(_function_body(motion, "msh_flush_window")):
        failures.append(
            "integer_motion_hip.c: the five-frame window is not derived with the CPU's "
            "window function"
        )
    return failures


def _motion_v2_failures(motion_v2: str) -> list[str]:
    failures: list[str] = []
    if "MIN(sad_score * s->motion_fps_weight, s->motion_max_val)" not in motion_v2:
        failures.append("integer_motion_v2_hip.c: the stored SAD is not weighted and capped")
    # integer_motion.c::vmaf_motion_window_flush() is the one derivation of
    # motion2 / motion3 from the stored SADs (ADR-1478); the twin calls it for
    # both windows (ADR-1491) and so has no end case or weighting of its own.
    if MOTION_WINDOW_CALL not in _code(_function_body(motion_v2, "flush_fex_hip")):
        failures.append(
            "integer_motion_v2_hip.c: flush does not derive motion2_v2 / motion3_v2 "
            "with the CPU's window function"
        )
    if "vmaf_feature_collector_get_score" in _code(motion_v2):
        failures.append(
            "integer_motion_v2_hip.c: the twin reads stored scores back, a window of its own"
        )
    return failures


def _float_ssim_decimation_failures(src: dict[str, str]) -> list[str]:
    """ADR-1405: float_ssim_hip decimates on the device with the CPU's arithmetic."""
    failures: list[str] = []
    header = src[FSSIM_DECIMATE]
    sample = _function_body(
        header.replace("VMAF_HIP_HOST_DEVICE float", "static float"),
        "vmaf_hip_ssim_decimate_sample",
    )
    if "int64_t sum = 0;" not in sample or "sum += vmaf_hip_ssim_fixed(product);" not in sample:
        failures.append(f"{FSSIM_DECIMATE}: the window is not summed exactly in int64")
    if "return (float)sum * VMAF_HIP_SSIM_FIXED_INV;" not in sample:
        failures.append(f"{FSSIM_DECIMATE}: the window sum is not rounded to fp32 once")
    if sample.count("vmaf_hip_ssim_symmetric_index(") != SSIM_DECIMATE_AXES:
        failures.append(f"{FSSIM_DECIMATE}: a window index skips the CPU's symmetric mirror")
    kernel = src[FSSIM_KERNEL]
    if kernel.count("vmaf_hip_ssim_decimate_sample(&d, centre_x, centre_y);") != SSIM_PLANES:
        failures.append(f"{FSSIM_KERNEL}: the decimation kernel has its own copy of the window sum")
    host = src[FSSIM_HOST]
    submit = _function_body(host, "submit_fex_hip")
    if not re.search(
        r"if \(err == 0 && s->scale > 1\)\s*err = ssim_hip_launch_decimate\(s, str\);", submit
    ):
        failures.append(f"{FSSIM_HOST}: a scale above 1 does not run the device decimation")
    launch = _function_body(host, "ssim_hip_launch_decimate")
    if "1.0f / (float)(s->scale * s->scale)" not in launch or HOST_WAIT.search(launch):
        failures.append(f"{FSSIM_HOST}: the decimation launch is not the CPU's tap, or it waits")
    if "(unsigned)iqa_decimate_dim((int)extent, scale)" not in host:
        failures.append(f"{FSSIM_HOST}: the decimated plane is not sized by iqa_decimate_dim()")
    return failures


def _float_ssim_arithmetic_failures(src: dict[str, str]) -> list[str]:
    """ADR-1441: float_ssim_hip's window sums and l / c / s are the shared CPU arithmetic."""
    failures: list[str] = []
    kernel = _code(src[FSSIM_KERNEL])
    for call in FSSIM_KERNEL_CALLS:
        if call not in kernel:
            failures.append(f"{FSSIM_KERNEL}: does not compute through {MS_ARITH} ({call})")
    if MULTIPLY_ACCUMULATE.search(kernel) or MS_OWN_MATH.search(kernel):
        failures.append(f"{FSSIM_KERNEL}: the kernel has window or l / c / s arithmetic of its own")
    return failures


def _guard_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    load = _function_body(
        src[ADM_KERNEL].replace("__device__ __forceinline__ void", "static void"),
        "adm_dwt2_load_column",
    )
    if "adm_dwt2_source_row(" not in load or "abs(" in load:
        failures.append(f"{ADM_KERNEL}: the scale-0 vertical load reflects without the clamp")
    if "vmaf_hip_tile_index(adm_dwt2_reflect_row(" not in src[ADM_ROWS]:
        failures.append(f"{ADM_ROWS}: adm_dwt2_source_row() lost the clamp")
    if "(reflected >= extent) ? extent - 1 : reflected" not in src[TILE_INDEX]:
        failures.append(f"{TILE_INDEX}: vmaf_hip_tile_index() no longer clamps to the plane")
    vif = src[VIF_HOST]
    if (
        ".context_check = check_context_hip" not in vif
        or '.context_fallback_name = "vif"' not in vif
    ):
        failures.append(f"{VIF_HOST}: vif_hip no longer declares its CPU fallback (ADR-1324)")
    init = _function_body(vif, "init_fex_hip")
    guard = init.find("vif_hip_min_dim()")
    scaffold = init.find("return -ENOSYS;")
    device = init.find("vif_hip_stream_init(")
    if guard < 0 or scaffold < 0 or not scaffold < guard < device:
        failures.append(
            f"{VIF_HOST}: init() must return -ENOSYS first without HIPCC (ADR-1264) and "
            "check the minimum size before any device work"
        )
    fm_kernel = src[FMOTION_KERNEL]
    if (
        "fm_mirror" in fm_kernel
        or "vmaf_hip_tile_index(vmaf_hip_reflect_101(" not in fm_kernel
        or fm_kernel.count("fm_tile_index(tile_o") != FM_TILE_LOADS
    ):
        failures.append(f"{FMOTION_KERNEL}: a tile load reflects without the index clamp")
    return failures


def _issim_raster_failures(src: dict[str, str]) -> list[str]:
    """ADR-1438: every frame is summed in the CPU's raster order, on the host."""
    failures: list[str] = []
    kernel = src[ISSIM_KERNEL]
    terms = _function_body(kernel.replace("__global__ void\n", "void "), "integer_ssim_vert_terms")
    if "terms[(size_t)y * width + x] = issim_cpu_term(issim_factors(m, samplemax));" not in terms:
        failures.append(f"{ISSIM_KERNEL}: pass 2 does not store the CPU's term in raster order")
    if "s_term" in kernel or "double my_term" in kernel:
        failures.append(f"{ISSIM_KERNEL}: the per-pixel terms are reduced on the device")
    if "s_weight[tid] += s_weight[tid + half];" not in terms:
        failures.append(f"{ISSIM_KERNEL}: the integer weights are no longer reduced per block")
    host = src[ISSIM_HOST]
    if "s->term_count = (size_t)w * h;" not in host:
        failures.append(f"{ISSIM_HOST}: the terms read back are not one per pixel")
    if "hipModuleLaunchKernel(s->func_vert_terms," not in _function_body(
        host, "issim_hip_launch_vert"
    ):
        failures.append(f"{ISSIM_HOST}: pass 2 is not the per-pixel kernel at every frame size")
    collect = _function_body(host, "collect_fex_hip")
    if "for (size_t i = 0u; i < s->term_count; i++) total_term += terms[i];" not in _squeeze(
        collect
    ):
        failures.append(f"{ISSIM_HOST}: collect() no longer adds the terms in ascending order")
    return failures


def _float_ssim_raster_failures(src: dict[str, str]) -> list[str]:
    """T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02: the host adds every window in raster order."""
    failures: list[str] = []
    kernel = _code(src[FSSIM_KERNEL])
    for store in (
        "terms[(size_t)y * w_final + x] = ssim_pixel(m, c1, c2, lcs);",
        "terms[window] = ssim_pixel(m, c1, c2, lcs);",
        "lcs_terms[(size_t)k * windows + window] = lcs[k];",
    ):
        if store not in kernel:
            failures.append(f"{FSSIM_KERNEL}: pass 2 does not store a window's term ({store})")
    if "__shared__" in kernel or "__shfl" in kernel:
        failures.append(f"{FSSIM_KERNEL}: the per-window terms are reduced on the device")
    host = src[FSSIM_HOST]
    if "s->windows = (size_t)s->w_final * s->h_final;" not in host:
        failures.append(f"{FSSIM_HOST}: the terms read back are not one per window")
    sums = _squeeze(_function_body(host, "fssim_hip_frame_sums"))
    ascending = "for (size_t i = 0u; i < s->windows; i++)"
    if (
        f"{ascending} ssim_sum += ssim[i];" not in sums
        or f"{ascending} {{ ssim_sum += ssim[i]; l_sum += l[i]; c_sum += c[i]; s_sum += sv[i]; }}"
        not in sums
    ):
        failures.append(f"{FSSIM_HOST}: the frame sums are no longer added in ascending order")
    return failures


def _option_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    psnr = src[PSNR_HOST]
    for helper in (
        "vmaf_psnr_peak(",
        "vmaf_psnr_max(",
        "vmaf_psnr_from_mse(",
        "vmaf_psnr_aggregate(",
        "vmaf_psnr_clip_sse_add(",
    ):
        if helper not in psnr:
            failures.append(f"{PSNR_HOST}: {helper}) of psnr_score.h is not called")
    if "log10(" in psnr:
        failures.append(f"{PSNR_HOST}: a local copy of the PSNR math is back")
    for name in (ISSIM_HOST, FSSIM_HOST):
        if "vmaf_ssim_max_db(" not in src[name] or "s->enable_db, s->max_db" not in src[name]:
            failures.append(f"{name}: enable_db / clip_db do not reach the SSIM emitter")
    if "f.lum_num == f.lum_den" in src[ISSIM_KERNEL]:
        failures.append(f"{ISSIM_KERNEL}: an identical window is forced to its weight again")
    failures += _issim_raster_failures(src)
    failures += _float_ssim_arithmetic_failures(src)
    failures += _float_ssim_raster_failures(src)
    if "calculate_ssim_hip_vert_combine_lcs" not in src[FSSIM_HOST]:
        failures.append(f"{FSSIM_HOST}: enable_lcs has no device kernel")
    if "return lcs[0] * lcs[1] * lcs[2];" not in src[FSSIM_KERNEL] or re.search(
        r"\bnum\s*==\s*den\b", src[FSSIM_KERNEL]
    ):
        failures.append(f"{FSSIM_KERNEL}: the per-pixel term is not the CPU's l * c * s")
    if "*mean = (double)(float)ratio;" not in src[FSSIM_HOST]:
        failures.append(f"{FSSIM_HOST}: the frame mean is not rounded to fp32 like the CPU's")
    if ".flags = VMAF_FEATURE_EXTRACTOR_HIP | VMAF_FEATURE_EXTRACTOR_TEMPORAL" not in psnr:
        failures.append(f"{PSNR_HOST}: psnr_hip is not temporal like the CPU psnr")
    if not re.search(r"\"VMAF_feature_motion_score\",\s*fm_hip_motion_clip\(", src[FMOTION_HOST]):
        failures.append(f"{FMOTION_HOST}: the debug motion score skips motion_clip")
    failures += _float_motion_sum_failures(src)
    failures += _float_motion_option_failures(src)
    return failures


def _squeeze(source: str) -> str:
    """`source` with every run of whitespace as one space, so a check does not depend on wrapping."""
    return " ".join(source.split())


def _float_motion_sum_failures(src: dict[str, str]) -> list[str]:
    """ADR-1409: float_motion_hip adds its SAD in the CPU's order."""
    failures: list[str] = []
    rows = _squeeze(src[FMOTION_ROWS])
    for piece in FMOTION_ROWS_PIECES:
        if piece not in rows:
            failures.append(f"{FMOTION_ROWS}: no longer the CPU's sum ({piece})")
    kernel = _squeeze(src[FMOTION_KERNEL])
    for call in FMOTION_KERNEL_CALLS:
        if call not in kernel:
            failures.append(
                f"{FMOTION_KERNEL}: a kernel no longer goes through {FMOTION_ROWS} ({call})"
            )
    if "__shfl_down" in kernel or "partials" in kernel:
        failures.append(f"{FMOTION_KERNEL}: a kernel reduces the SAD per wave or per block")
    host = src[FMOTION_HOST]
    score = _squeeze(_function_body(host, "fm_hip_frame_score"))
    if "score += vmaf_hip_float_motion_plane_score(rows + p->off0, p->w, p->h," not in score:
        failures.append(f"{FMOTION_HOST}: the frame score is not built from the CPU's row sums")
    if re.search(r"\+= \(double\)|double total", score):
        failures.append(f"{FMOTION_HOST}: the row sums are added in double")
    if "#define FMH_ROW_THREADS VMAF_HIP_FLOAT_MOTION_ROW_GROUP" not in host:
        failures.append(f"{FMOTION_HOST}: the row kernel's block is not one group of rows")
    return failures


def _float_motion_option_failures(src: dict[str, str]) -> list[str]:
    """ADR-1404: motion3 and the CPU float_motion options on float_motion_hip."""
    failures: list[str] = []
    host = src[FMOTION_HOST]
    kernel = src[FMOTION_KERNEL]
    blend = _function_body(host, "fm_hip_motion_blend_clip")
    if "motion_blend(score * s->motion_fps_weight, s->motion_blend_factor" not in blend:
        failures.append(f"{FMOTION_HOST}: motion3 does not blend through motion_blend_tools.h")
    emits = len(re.findall(r"\"VMAF_feature_motion3_score\",\s*fm_hip_motion_blend_clip\(", host))
    if emits != FM_MOTION3_BLEND_EMITS:
        failures.append(f"{FMOTION_HOST}: a motion3 score skips motion_blend_clip")
    if '"VMAF_feature_motion3_score", 0.0, 0u);' not in _function_body(host, "flush_fex_hip"):
        failures.append(f"{FMOTION_HOST}: a one-frame run emits no motion3")
    if "fm_blur_pixel(s_tile, fm_filter(filter_size))" not in kernel:
        failures.append(f"{FMOTION_KERNEL}: motion_filter_size does not select the blur filter")
    launch = _function_body(host, "fm_hip_launch_kernels")
    sads = _function_body(host, "fm_hip_launch_sads")
    if "fm_hip_launch_sads(" not in launch or "fm_hip_launch_scale1(" not in sads:
        failures.append(f"{FMOTION_HOST}: motion_add_scale1 does not run the scale-1 SAD kernel")
    if "p->diff[1], p->off1, p->sw, p->sh" not in sads:
        failures.append(f"{FMOTION_HOST}: the scale-1 differences are not added row by row")
    if "c < s->n_planes" not in launch or "s->n_planes = FMH_MAX_PLANES;" not in host:
        failures.append(f"{FMOTION_HOST}: motion_add_uv does not run the chroma planes")
    if HOST_WAIT.search(launch) or HOST_WAIT.search(sads):
        failures.append(f"{FMOTION_HOST}: the frame's kernels wait on the host")
    return failures


def _code(source: str) -> str:
    """C / HIP source without its comments."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def _ms_ssim_failures(src: dict[str, str]) -> list[str]:
    """ADR-1403: float_ms_ssim_hip computes with the CPU's arithmetic."""
    failures: list[str] = []
    arith = _code(src[MS_ARITH])
    for piece in MS_ARITH_PIECES:
        if piece not in arith:
            failures.append(f"{MS_ARITH}: no longer the CPU's arithmetic ({piece})")
    if MULTIPLY_ACCUMULATE.search(arith):
        failures.append(f"{MS_ARITH}: an fp32 multiply-accumulate replaces the CPU's sum")
    if "#pragma" in arith:
        failures.append(f"{MS_ARITH}: a pragma changes the arithmetic the build flags set")
    kernel = _code(src[MS_KERNEL])
    for call in MS_KERNEL_CALLS:
        if call not in kernel:
            failures.append(f"{MS_KERNEL}: a kernel no longer computes through {MS_ARITH} ({call})")
    if MULTIPLY_ACCUMULATE.search(kernel) or MS_OWN_MATH.search(kernel):
        failures.append(f"{MS_KERNEL}: a kernel has sample arithmetic of its own")
    host = _code(src[MS_HOST])
    for call in MS_HOST_CALLS:
        if call not in host:
            failures.append(f"{MS_HOST}: no longer combines as the CPU does ({call})")
    if re.search(r"\bpow\s*\(", host):
        failures.append(f"{MS_HOST}: the extractor has a Wang combine of its own")
    return failures


# float_ms_ssim_hip stores the l / c / s of every window at its raster position.
MS_TERM_STORES = (
    "terms[window] = lcs.l;",
    "terms[windows + window] = lcs.c;",
    "terms[2u * windows + window] = lcs.s;",
)


def _ms_ssim_raster_failures(src: dict[str, str]) -> list[str]:
    """T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02: the host adds every scale in raster order."""
    failures: list[str] = []
    kernel = _code(src[MS_KERNEL])
    for store in MS_TERM_STORES:
        if store not in kernel:
            failures.append(f"{MS_KERNEL}: pass 2 does not store a window's term ({store})")
    if "const size_t window = (size_t)y * w_final + x;" not in kernel:
        failures.append(f"{MS_KERNEL}: the terms are not stored at the window's raster position")
    if "__shared__" in kernel or "__shfl" in kernel:
        failures.append(f"{MS_KERNEL}: the per-window terms are reduced on the device")
    host = _code(src[MS_HOST])
    if "pl->scale_windows[i] = (size_t)pl->scale_w_final[i] * pl->scale_h_final[i];" not in host:
        failures.append(f"{MS_HOST}: the terms read back are not one per window")
    if "hipHostMallocWriteCombined" in host:
        failures.append(f"{MS_HOST}: the host reads the terms from write-combined memory")
    sums = _squeeze(_function_body(host, "ms_ssim_hip_scale_sums"))
    if (
        "for (size_t j = 0u; j < windows; j++) { l_sum += l[j]; c_sum += c[j]; s_sum += sv[j]; }"
        not in sums
    ):
        failures.append(f"{MS_HOST}: the per-scale sums are no longer added in ascending order")
    if "ms_ssim_hip_scale_sums(pl, i, &total_l, &total_c, &total_s);" not in host:
        failures.append(f"{MS_HOST}: collect() does not take the sums in the CPU's order")
    return failures


# T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06: with enable_chroma the twin
# runs the same pipeline once per plane, as float_ms_ssim.c does, and emits the
# CPU's three plane features. It used to accept the option and keep one plane.
MS_CHROMA_PIECES = (
    "s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);",
    "const int err = ms_ssim_hip_submit_plane(s, str, ref_pic, dist_pic, p);",
    "ms_ssim_hip_plane_scores(&s->planes[p], &scores[p]);",
    'static const char *provided_features[] = {"float_ms_ssim", "float_ms_ssim_cb", '
    '"float_ms_ssim_cr", NULL};',
)


def _ms_ssim_chroma_failures(src: dict[str, str]) -> list[str]:
    host = _squeeze(_code(src[MS_HOST]))
    return [
        f"{MS_HOST}: the planes enable_chroma scores are not the CPU's ({piece})"
        for piece in MS_CHROMA_PIECES
        if piece not in host
    ]


def _failures(src: dict[str, str]) -> list[str]:
    return (
        _motion_failures(src)
        + _guard_failures(src)
        + _option_failures(src)
        + _staging_failures(src)
        + _float_ssim_decimation_failures(src)
        + _ms_ssim_failures(src)
        + _ms_ssim_raster_failures(src)
        + _ms_ssim_chroma_failures(src)
    )


def _replace(src: dict[str, str], name: str, old: str, new: str) -> dict[str, str]:
    if old not in src[name]:
        raise AssertionError(f"planted regression anchor missing in {name}: {old!r}")
    out = dict(src)
    out[name] = src[name].replace(old, new, 1)
    return out


class HipKernelSourceContractTest(unittest.TestCase):
    def assert_detected(self, src: dict[str, str], needle: str) -> None:
        failures = _failures(src)
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_live_sources_keep_the_contract(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_blur_before_difference_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_KERNEL,
            "mv2_sample<T>(prev + gy * prev_stride, gx) -",
            "mv2_sample<T>(cur + gy * cur_stride, gx) -",
        )
        self.assert_detected(src, "prev - cur")

    def test_unrounded_vertical_pass_is_detected(self) -> None:
        src = _replace(_sources(), MOTION_KERNEL, "(sum + round_y) >> shift_y", "sum >> shift_y")
        self.assert_detected(src, "CPU rounding")

    def test_unclamped_motion_tile_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_KERNEL,
            "vmaf_hip_tile_index(\n            "
            "vmaf_hip_reflect_101(tile_origin_x + (int)tx, (int)width), (int)width)",
            "vmaf_hip_reflect_101(tile_origin_x + (int)tx, (int)width)",
        )
        self.assert_detected(src, "without the index clamp")

    def test_blurred_ping_pong_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            "    void *prev_luma[2];",
            "    void *prev_luma[2];\n    void *blur[2];",
        )
        self.assert_detected(src, "blurred-frame ping-pong")

    def test_waiting_upload_in_motion_submit_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            "    err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);",
            "    (void)vmaf_hip_picture_upload(NULL, 0u, s->lc.str);\n"
            "    err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);",
        )
        self.assert_detected(src, "without a wait")

    def test_stream_sync_in_sad_pipeline_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_SAD,
            "    if (frame->have_prev) {",
            "    (void)hipStreamSynchronize(NULL);\n    if (frame->have_prev) {",
        )
        self.assert_detected(src, "waits on the host")

    def test_waiting_staged_upload_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PICTURE,
            "        at += p->rows * p->row_bytes;",
            "        (void)hipStreamSynchronize(str);\n        at += p->rows * p->row_bytes;",
        )
        self.assert_detected(src, "staged upload waits")

    def test_raw_debug_motion_score_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            '"VMAF_integer_feature_motion_score",\n'
            "                                                     motion_clip_hip(s, s->score), index);",
            '"VMAF_integer_feature_motion_score",\n'
            "                                                     s->score, index);",
        )
        self.assert_detected(src, "debug motion score skips motion_clip")

    def test_missing_motion_sad_score_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            '"VMAF_integer_feature_motion_sad_score",\n'
            "                                                    motion_clip_hip(s, s->score), index);",
            '"VMAF_integer_feature_motion_sad_score",\n'
            "                                                    s->score, index);",
        )
        self.assert_detected(src, "motion_sad_score is not emitted")

    def test_unclamped_adm_row_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ADM_KERNEL,
            "adm_dwt2_source_row(y_out, i, h)",
            "abs(adm_dwt2_reflect_row(y_out, i, h))",
        )
        self.assert_detected(src, "without the clamp")

    def test_vif_without_fallback_is_detected(self) -> None:
        src = _replace(_sources(), VIF_HOST, "    .context_check = check_context_hip,\n", "")
        self.assert_detected(src, "CPU fallback")

    def test_local_psnr_math_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PSNR_HOST,
            "    const double psnr =\n        vmaf_psnr_from_mse(",
            "    const double psnr = 10.0 * log10(1.0) +\n        vmaf_psnr_from_mse(",
        )
        self.assert_detected(src, "local copy of the PSNR math")

    def test_ssim_db_dropped_is_detected(self) -> None:
        src = _replace(
            _sources(), ISSIM_HOST, "s->enable_db, s->max_db, index);", "0, 0.0, index);"
        )
        self.assert_detected(src, "do not reach the SSIM emitter")

    def test_integer_ssim_forced_identical_window_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_KERNEL,
            "    return f.w_d * f.lum_num * f.cs_num / (f.lum_den * f.cs_den);",
            "    if (f.lum_num == f.lum_den && f.cs_num == f.cs_den)\n        return f.w_d;\n"
            "    return f.w_d * f.lum_num * f.cs_num / (f.lum_den * f.cs_den);",
        )
        self.assert_detected(src, "forced to its weight again")

    def test_integer_ssim_unordered_terms_are_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_KERNEL,
            "terms[(size_t)y * width + x] = issim_cpu_term(issim_factors(m, samplemax));",
            "terms[blockIdx.y * gridDim.x + blockIdx.x] += issim_cpu_term(issim_factors(m, samplemax));",
        )
        self.assert_detected(src, "does not store the CPU's term in raster order")

    def test_integer_ssim_device_term_reduction_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_KERNEL,
            "    __shared__ int64_t s_weight[ISSIM_BLOCK_SZ];",
            "    __shared__ double s_term[ISSIM_BLOCK_SZ];\n"
            "    __shared__ int64_t s_weight[ISSIM_BLOCK_SZ];",
        )
        self.assert_detected(src, "reduced on the device")

    def test_integer_ssim_block_sized_readback_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "s->term_count = (size_t)w * h;",
            "s->term_count = (size_t)s->grid_x * s->grid_y;",
        )
        self.assert_detected(src, "not one per pixel")

    def test_integer_ssim_other_pass_two_kernel_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "hipModuleLaunchKernel(s->func_vert_terms,",
            "hipModuleLaunchKernel(s->func_horiz_8,",
        )
        self.assert_detected(src, "not the per-pixel kernel at every frame size")

    def test_integer_ssim_descending_collect_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "for (size_t i = 0u; i < s->term_count; i++)",
            "for (size_t i = s->term_count; i-- > 0u;)",
        )
        self.assert_detected(src, "ascending order")

    def test_fp32_float_ssim_window_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    const VmafHipMsWindow sums = vmaf_hip_ms_ssim_horizontal(ref_taps, cmp_taps);",
            "    VmafHipMsWindow sums = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};\n"
            "    for (int u = 0; u < SSIM_K; u++)\n"
            "        sums.ref_mu += vmaf_hip_ms_ssim_window_tap(u) * ref_taps[u];",
        )
        self.assert_detected(src, "vmaf_hip_ms_ssim_horizontal(ref_taps, cmp_taps);")
        self.assert_detected(src, "arithmetic of its own")

    def test_fp32_float_ssim_vertical_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    return vmaf_hip_ms_ssim_vertical(&planes, (size_t)y * w_horiz + x, w_horiz);",
            "    VmafHipMsWindow m = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};\n"
            "    for (int v = 0; v < SSIM_K; v++)\n"
            "        m.ref_mu += vmaf_hip_ms_ssim_window_tap(v) * planes.ref_mu[(y + v) * w_horiz + x];\n"
            "    return m;",
        )
        self.assert_detected(src, "vmaf_hip_ms_ssim_vertical(")

    def test_float_ssim_terms_of_its_own_are_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    const VmafHipMsLcs terms = vmaf_hip_ms_ssim_lcs(&m, c1, c2, c2 / 2.0f);",
            "    VmafHipMsLcs terms;\n"
            "    terms.l = (2.0 * m.ref_mu * m.cmp_mu + c1) / (m.ref_mu * m.ref_mu + c1);\n"
            "    terms.c = (2.0 * sqrtf(m.ref_sq * m.cmp_sq) + c2) / (m.ref_sq + m.cmp_sq + c2);\n"
            "    terms.s = 1.0;",
        )
        self.assert_detected(src, "vmaf_hip_ms_ssim_lcs(&m, c1, c2, c2 / 2.0f);")

    def test_forced_identical_float_ssim_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    return lcs[0] * lcs[1] * lcs[2];",
            "    const double num = lcs[0], den = lcs[1] * lcs[2];\n"
            "    return num == den ? 1.0 : num * den;",
        )
        self.assert_detected(src, "not the CPU's l * c * s")

    def test_float_ssim_block_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    terms[(size_t)y * w_final + x] = ssim_pixel(m, c1, c2, lcs);",
            "    __shared__ double s_term[256];\n"
            "    s_term[threadIdx.y * blockDim.x + threadIdx.x] = ssim_pixel(m, c1, c2, lcs);",
        )
        self.assert_detected(src, "does not store a window's term")
        self.assert_detected(src, "reduced on the device")

    def test_float_ssim_unordered_lcs_terms_are_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "lcs_terms[(size_t)k * windows + window] = lcs[k];",
            "lcs_terms[(size_t)k * gridDim.x * gridDim.y + blockIdx.x] += lcs[k];",
        )
        self.assert_detected(src, "does not store a window's term")

    def test_float_ssim_block_sized_readback_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_HOST,
            "s->windows = (size_t)s->w_final * s->h_final;",
            "s->windows = (size_t)grid_x * grid_y;",
        )
        self.assert_detected(src, "not one per window")

    def test_float_ssim_descending_frame_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_HOST,
            "        for (size_t i = 0u; i < s->windows; i++)\n            ssim_sum += ssim[i];",
            "        for (size_t i = s->windows; i-- > 0u;)\n            ssim_sum += ssim[i];",
        )
        self.assert_detected(src, "ascending order")

    def test_float_ssim_descending_lcs_sums_are_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_HOST,
            "        for (size_t i = 0u; i < s->windows; i++) {",
            "        for (size_t i = s->windows; i-- > 0u;) {",
        )
        self.assert_detected(src, "ascending order")

    def test_double_float_ssim_mean_is_detected(self) -> None:
        src = _replace(_sources(), FSSIM_HOST, "*mean = (double)(float)ratio;", "*mean = ratio;")
        self.assert_detected(src, "rounded to fp32")

    def test_fp32_float_ssim_decimation_sum_is_detected(self) -> None:
        src = _replace(_sources(), FSSIM_DECIMATE, "    int64_t sum = 0;\n", "    float sum = 0;\n")
        self.assert_detected(src, "not summed exactly in int64")

    def test_double_rounded_float_ssim_decimation_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_DECIMATE,
            "return (float)sum * VMAF_HIP_SSIM_FIXED_INV;",
            "return (float)((double)sum * VMAF_HIP_SSIM_FIXED_INV);",
        )
        self.assert_detected(src, "not rounded to fp32 once")

    def test_clamped_float_ssim_decimation_edge_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_DECIMATE,
            "vmaf_hip_ssim_symmetric_index(centre_x + c - half, (int)d->width)",
            "vmaf_hip_tile_index(centre_x + c - half, (int)d->width)",
        )
        self.assert_detected(src, "skips the CPU's symmetric mirror")

    def test_skipped_float_ssim_decimation_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_HOST,
            "    if (err == 0 && s->scale > 1)\n        err = ssim_hip_launch_decimate(s, str);\n",
            "",
        )
        self.assert_detected(src, "does not run the device decimation")

    def test_floor_sized_float_ssim_plane_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_HOST,
            "(unsigned)iqa_decimate_dim((int)extent, scale)",
            "extent / (unsigned)scale",
        )
        self.assert_detected(src, "not sized by iqa_decimate_dim()")

    def test_unclamped_float_motion_tile_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_KERNEL,
            "return vmaf_hip_tile_index(vmaf_hip_reflect_101(idx, sup), sup);",
            "return vmaf_hip_reflect_101(idx, sup);",
        )
        self.assert_detected(src, "float_motion_score.hip: a tile load reflects")

    def test_unweighted_motion_v2_sad_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_v2_hip.c",
            "MIN(sad_score * s->motion_fps_weight, s->motion_max_val)",
            "sad_score",
        )
        self.assert_detected(src, "not weighted and capped")

    def test_motion_v2_own_flush_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_v2_hip.c",
            MOTION_WINDOW_CALL,
            "mv2_hip_flush_scores(s, feature_collector, &window)",
        )
        self.assert_detected(src, "the CPU's window function")

    def test_motion_v2_reading_scores_back_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_v2_hip.c",
            "    const int err = " + MOTION_WINDOW_CALL + ";\n",
            "    double score_cur = 0.0;\n"
            "    (void)vmaf_feature_collector_get_score(feature_collector, window.sad_feature,\n"
            "                                           &score_cur, 0u);\n"
            "    const int err = " + MOTION_WINDOW_CALL + ";\n",
        )
        self.assert_detected(src, "a window of its own")

    def test_motion_own_five_frame_window_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            MOTION_WINDOW_CALL,
            "msh_flush_five_frame_scores(s, feature_collector, &window)",
        )
        self.assert_detected(src, "five-frame window is not derived")

    def test_non_temporal_psnr_is_detected(self) -> None:
        src = _replace(
            _sources(),
            PSNR_HOST,
            ".flags = VMAF_FEATURE_EXTRACTOR_HIP | VMAF_FEATURE_EXTRACTOR_TEMPORAL",
            ".flags = VMAF_FEATURE_EXTRACTOR_HIP",
        )
        self.assert_detected(src, "not temporal")

    def test_motion_debug_default_true_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            '.type = VMAF_OPT_TYPE_BOOL, .default_val.b = false},\n    {.name = "motion_force_zero"',
            '.type = VMAF_OPT_TYPE_BOOL, .default_val.b = true},\n    {.name = "motion_force_zero"',
        )
        self.assert_detected(src, "defaults to false")

    def test_private_motion_upload_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_v2_hip.c",
            "vmaf_hip_plane_source_acquire_luma(&s->planes, shared,",
            "mv2_hip_own_upload(&s->planes, shared,",
        )
        self.assert_detected(src, "does not come from the shared frame")

    def test_keep_copy_ahead_of_the_sad_is_detected(self) -> None:
        text = _sources()[MOTION_SAD]
        copy = (
            "    const hipError_t rc =\n"
            "        hipMemcpyAsync(frame->keep, frame->cur, bytes, hipMemcpyDeviceToDevice, str);\n"
        )
        guard = "    if (frame->have_prev) {\n"
        assert copy in text and guard in text
        size = (
            "    const size_t bytes = vmaf_hip_motion_sad_plane_bytes(frame->width, frame->height, "
            "frame->bpc);\n"
        )
        assert size in text
        moved = (
            text.replace(copy, "", 1).replace(size, "", 1).replace(guard, size + copy + guard, 1)
        )
        src = _sources()
        src[MOTION_SAD] = moved
        self.assert_detected(src, "replaced before the SAD read the previous frame")

    def test_sad_against_the_current_frame_is_detected(self) -> None:
        src = _replace(_sources(), MOTION_SAD, SAD_READS_KEPT, "const void *prev = f->cur;")
        self.assert_detected(src, "replaced before the SAD read the previous frame")

    def test_drain_on_the_success_path_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_SAD,
            "        return motion_sad_drain_after_error(stream, vmaf_hip_rc_to_errno(rc));\n"
            "    return vmaf_hip_rc_to_errno(rc);",
            "        return vmaf_hip_rc_to_errno(rc);\n"
            "    (void)motion_sad_drain_after_error(stream, 0);\n"
            "    return vmaf_hip_rc_to_errno(rc);",
        )
        self.assert_detected(src, "reachable outside an error return")

    def test_scaffold_vif_min_dim_first_is_detected(self) -> None:
        src = _sources()
        vif = src[VIF_HOST]
        scaffold = (
            "#ifndef HAVE_HIPCC\n    /* Scaffold posture: -ENOSYS and nothing else (ADR-1264). */"
        )
        assert scaffold in vif
        src[VIF_HOST] = vif.replace(scaffold, "    (void)vif_hip_min_dim();\n" + scaffold, 1)
        self.assert_detected(src, "return -ENOSYS first")

    def test_unweighted_float_motion_debug_score_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "fm_hip_motion_clip(s, motion_score), index);",
            "motion_score, index);",
        )
        self.assert_detected(src, "skips motion_clip")

    def test_unblended_float_motion3_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "fm_hip_motion_blend_clip(s, motion2), index - 1u);",
            "fm_hip_motion_clip(s, motion2), index - 1u);",
        )
        self.assert_detected(src, "a motion3 score skips motion_blend_clip")

    def test_local_float_motion_blend_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "motion_blend(score * s->motion_fps_weight, s->motion_blend_factor",
            "fm_local_blend(score * s->motion_fps_weight, s->motion_blend_factor",
        )
        self.assert_detected(src, "does not blend through motion_blend_tools.h")

    def test_missing_one_frame_float_motion3_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            '"VMAF_feature_motion3_score", 0.0, 0u);',
            '"VMAF_feature_motion2_score", 0.0, 0u);',
        )
        self.assert_detected(src, "a one-frame run emits no motion3")

    def test_fixed_float_motion_filter_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_KERNEL,
            "fm_blur_pixel(s_tile, fm_filter(filter_size))",
            "fm_blur_pixel(s_tile, FM_FILT)",
        )
        self.assert_detected(src, "motion_filter_size does not select the blur filter")

    def test_skipped_float_motion_scale1_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "    err = fm_hip_launch_scale1(s, p, pstr);\n",
            "",
        )
        self.assert_detected(src, "motion_add_scale1 does not run the scale-1 SAD kernel")

    def test_unsummed_float_motion_scale1_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "(const float *)p->diff[1], p->off1, p->sw, p->sh, pstr);",
            "(const float *)p->diff[0], p->off1, p->sw, p->sh, pstr);",
        )
        self.assert_detected(src, "scale-1 differences are not added row by row")

    def test_block_reduced_float_motion_sad_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_KERNEL,
            "    row_sad[y] = vmaf_hip_float_motion_row_sum(diff, width, y);",
            "    float v = diff[y];\n"
            "    for (int off = (int)warpSize / 2; off > 0; off >>= 1)\n"
            "        v += __shfl_down(v, off);\n"
            "    row_sad[y] = v;",
        )
        self.assert_detected(src, "vmaf_hip_float_motion_row_sum(diff, width, y)")
        self.assert_detected(src, "per wave or per block")

    def test_double_float_motion_row_sum_is_detected(self) -> None:
        src = _replace(
            _sources(), FMOTION_ROWS, "    float accum = 0.0f;", "    double accum = 0.0;"
        )
        self.assert_detected(src, "float accum = 0.0f;")

    def test_untransposed_float_motion_row_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_ROWS,
            "accum += row[(size_t)j * VMAF_HIP_FLOAT_MOTION_ROW_GROUP];",
            "accum += row[j];",
        )
        self.assert_detected(src, "accum += row[(size_t)j")

    def test_double_float_motion_scale_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_ROWS,
            "    float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);",
            "    double score = vmaf_float_motion_score_from_row_sads(rows, width, height);",
        )
        self.assert_detected(src, "float score = (float)vmaf_float_motion_score_from_row_sads")

    def test_host_double_float_motion_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FMOTION_HOST,
            "        score += vmaf_hip_float_motion_plane_score(rows + p->off0, p->w, p->h,",
            "        double total = 0.0;\n"
            "        for (unsigned i = 0u; i < p->h; i++)\n"
            "            total += (double)rows[p->off0 + i];\n"
            "        score += total / ((double)p->w * (double)p->h) + 0.0 * (double)(scale1_rows != NULL) *",
        )
        self.assert_detected(src, "not built from the CPU's row sums")
        self.assert_detected(src, "added in double")

    def test_luma_only_float_motion_add_uv_is_detected(self) -> None:
        src = _replace(
            _sources(), FMOTION_HOST, "s->n_planes = FMH_MAX_PLANES;", "s->n_planes = 1u;"
        )
        self.assert_detected(src, "motion_add_uv does not run the chroma planes")

    def test_unfused_ms_ssim_decimate_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_ARITH,
            "acc = fmaf(row_acc, vmaf_hip_ms_ssim_lpf_tap(kv), acc);",
            "acc += row_acc * vmaf_hip_ms_ssim_lpf_tap(kv);",
        )
        self.assert_detected(src, "fmaf(row_acc")
        self.assert_detected(src, "fp32 multiply-accumulate")

    def test_fp32_ms_ssim_window_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_ARITH,
            "vmaf_hip_ms_pair_add(&sums.ref_sq, r_sq * tap);",
            "sums.ref_sq.hi += r_sq * tap;",
        )
        self.assert_detected(src, "fp32 multiply-accumulate")

    def test_ms_ssim_pair_sum_without_its_error_term_is_detected(self) -> None:
        src = _replace(_sources(), MS_ARITH, "sum->lo = sum->lo + error;", "(void)error;")
        self.assert_detected(src, "sum->lo = sum->lo + error;")

    def test_fp64_ms_ssim_denominator_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_ARITH,
            "(double)(ref_var + cmp_var + c2);",
            "((double)ref_var + (double)cmp_var + (double)c2);",
        )
        self.assert_detected(src, "ref_var + cmp_var + c2")

    def test_fp64_ms_ssim_structure_quotient_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_ARITH,
            "out.s = (double)((clamped_covar + c3) / (sigma + c3));",
            "out.s = ((double)clamped_covar + (double)c3) / ((double)sigma + (double)c3);",
        )
        self.assert_detected(src, "clamped_covar + c3")

    def test_unrounded_ms_ssim_scale_mean_is_detected(self) -> None:
        src = _replace(
            _sources(), MS_ARITH, "return (double)(float)(sum / samples);", "return sum / samples;"
        )
        self.assert_detected(src, "(float)(sum / samples)")

    def test_ms_ssim_combine_without_fabs_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_ARITH,
            "pow(fabs(l_means[i]), (double)alphas[i])",
            "pow(l_means[i], (double)alphas[i])",
        )
        self.assert_detected(src, "pow(fabs(l_means[i])")

    def test_ms_ssim_kernel_with_its_own_window_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_KERNEL,
            "const VmafHipMsWindow sums = vmaf_hip_ms_ssim_horizontal(ref_in + src_idx, "
            "cmp_in + src_idx);",
            "VmafHipMsWindow sums = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};\n"
            "    for (int u = 0; u < 11; u++)\n"
            "        sums.ref_mu += ref_in[src_idx + u] * vmaf_hip_ms_ssim_window_tap(u);",
        )
        self.assert_detected(src, "vmaf_hip_ms_ssim_horizontal")
        self.assert_detected(src, "sample arithmetic of its own")

    def test_fp64_ms_ssim_host_constants_are_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "vmaf_hip_ms_ssim_constants(&c1, &c2, &c3);",
            "c1 = (float)((0.01 * 255.0) * (0.01 * 255.0));",
        )
        self.assert_detected(src, "vmaf_hip_ms_ssim_constants")

    def test_ms_ssim_host_combine_of_its_own_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "const double msssim = vmaf_hip_ms_ssim_combine(l_means, c_means, s_means);",
            "const double msssim = pow(l_means[4], 0.1333) * pow(c_means[4], 0.1333);",
        )
        self.assert_detected(src, "Wang combine of its own")

    def test_ms_ssim_block_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_KERNEL,
            "    terms[window] = lcs.l;",
            "    __shared__ double s_l_warp[4];\n"
            "    s_l_warp[threadIdx.y] = lcs.l + __shfl_down(lcs.l, 1);",
        )
        self.assert_detected(src, "does not store a window's term")
        self.assert_detected(src, "reduced on the device")

    def test_ms_ssim_unordered_terms_are_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_KERNEL,
            "const size_t window = (size_t)y * w_final + x;",
            "const size_t window = (size_t)x * h_final + y;",
        )
        self.assert_detected(src, "not stored at the window's raster position")

    def test_ms_ssim_block_sized_readback_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "pl->scale_windows[i] = (size_t)pl->scale_w_final[i] * pl->scale_h_final[i];",
            "pl->scale_windows[i] = (size_t)pl->scale_grid_x[i] * pl->scale_grid_y[i];",
        )
        self.assert_detected(src, "not one per window")

    def test_ms_ssim_write_combined_readback_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "ms_ssim_terms_bytes(pl, i), hipHostMallocDefault);",
            "ms_ssim_terms_bytes(pl, i), hipHostMallocWriteCombined);",
        )
        self.assert_detected(src, "write-combined memory")

    def test_ms_ssim_descending_scale_sum_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "    for (size_t j = 0u; j < windows; j++) {",
            "    for (size_t j = windows; j-- > 0u;) {",
        )
        self.assert_detected(src, "ascending order")

    def test_ms_ssim_block_order_collect_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "        ms_ssim_hip_scale_sums(pl, i, &total_l, &total_c, &total_s);",
            "        ms_ssim_hip_block_sums(pl, i, &total_l, &total_c, &total_s);",
        )
        self.assert_detected(src, "does not take the sums in the CPU's order")

    def test_ms_ssim_luma_only_plane_count_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MS_HOST,
            "s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);",
            "s->n_planes = 1u;",
        )
        self.assert_detected(src, "enable_chroma scores")

    def test_ms_ssim_luma_only_provided_features_are_detected(self) -> None:
        src = _replace(_sources(), MS_HOST, '"float_ms_ssim_cb", "float_ms_ssim_cr",', "")
        self.assert_detected(src, "enable_chroma scores")


if __name__ == "__main__":
    unittest.main()

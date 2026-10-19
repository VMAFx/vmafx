#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CUDA RC3 parity design at source level.

ADR-1372 to ADR-1374, ADR-1392, ADR-1399, ADR-1403, ADR-1409.

Device-free, so it runs on every host. Each contract has a planted-regression
case that edits the live source the way the old code read and must fail:

- motion: both twins run the one diff-first SAD kernel through
  integer_motion_sad_cuda.c; the kernel stages prev - cur before the blur; no
  submit path waits on the host, the previous frame is ordered by an event,
  and motion_cuda reads its SAD slots back with one synchronisation;
- PSNR / SSIM / float motion options: the host arithmetic is the CPU's shared
  helpers (psnr_score.h, nonfinite_score.h, motion_clip), not a copy; the
  chroma accumulators of psnr_cuda are zeroed on the kernels' picture
  stream, and psnr_cuda is TEMPORAL like the CPU psnr;
- motion_v2: the published SAD score is the CPU's weighted, capped value and
  flush derives motion2_v2 / motion3_v2 from it with the CPU's own function,
  vmaf_motion_window_flush() (ADR-1478, ADR-1491): the twin reads no stored
  score back and holds no window of its own; motion_cuda calls the same
  function for the five-frame window;
- float SSIM: each pixel is the CPU's l * c * s with double numerators over
  fp32 denominators (no forced exact 1), and the frame mean is rounded to
  fp32; `enable_chroma` stays declared as an ignored option (HISS-14);
- float SSIM pipeline (ADR-1399): the decimation window is summed exactly in
  int64 and rounded once, every convolution tap is an fp32 product added to
  a double sum (iqa/convolve.c), the plane size is the CPU's
  iqa_decimate_dim(), and submit never waits on the host;
- integer SSIM: the combine compiles with --fmad=false, like every CUDA
  fatbin since ADR-1403, and groups the term as the CPU does,
  ((w * a) * b) / den;
- float MS-SSIM (ADR-1403): the decimate fuses each tap explicitly as
  ms_ssim_decimate.c does, the window sums are fp32 products summed in an
  exact fp32 pair that stands for iqa_convolve()'s fp64 sum, l / c / s keep
  the CPU's fp32 denominators and quotient, and the host rounds each
  per-scale mean to fp32 and takes fp32 stabilisation constants;
- float motion SAD (ADR-1409): the device adds the absolute differences of a
  row in one thread, left to right, into one fp32 accumulator, and the host
  adds the rows and divides in fp32 through float_motion_sad.h, as
  compute_motion_simd() does; no block, warp or atomic reduction;
- ADM: the DWT kernels read their rows and taps through adm_dwt2_rows.h;
- VIF: vif_cuda declares its minimum size through the ADR-1324 gate;
- engine: libvmaf.c initialises a submit / collect extractor before it picks
  the asynchronous or the extract() path, because the motion twins' init()
  swaps the first for the second under motion_force_zero (the first frame
  used to call the submit() init() had cleared);
- reductions (ADR-1392): the motion SAD and PSNR kernels add one atomic per
  block, the moment kernel one per accumulator per block, the motion kernel
  computes its vertical pass once per block, and the PSNR kernel never
  indexes its by-value VmafPicture parameters with the runtime plane (nvcc
  then copies both pictures to every thread's stack).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CUDA_ROOT = ROOT / "core" / "src" / "feature" / "cuda"

MOTION_KERNEL = "integer_motion_v2/motion_v2_score.cu"
MOTION_SAD = "integer_motion_sad_cuda.c"
MOTION_TUS = ("integer_motion_cuda.c", "integer_motion_v2_cuda.c")
SSIM_KERNEL = "integer_ssim/ssim_score.cu"
ADM_KERNEL = "integer_adm/adm_dwt2.cu"
MESON = "meson.build"
MESON_PATH = ROOT / "core" / "src" / MESON
LIBVMAF = "libvmaf.c"
LIBVMAF_PATH = ROOT / "core" / "src" / LIBVMAF
# ADR-1403: one FP flag list for every fatbin, and the fatbin command takes it.
# core/test/test_strict_fp_compiler_args.py pins the policy itself.
CUDA_DEVICE_FMAD = "cuda_device_strict_fp_args = vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
CUDA_FATBIN_FP_ARGS = "cuda_flags + cuda_device_strict_fp_args"
INTEGER_SSIM_KERNEL = "integer_ssim/integer_ssim_score.cu"
PSNR_KERNEL = "integer_psnr/psnr_score.cu"
MOMENT_KERNEL = "integer_moment/moment_score.cu"
MOTION_V2_SAD_SCORE = "MIN(sad_score * s->motion_fps_weight, s->motion_max_val)"
MOTION_WINDOW_CALL = "vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window)"
FLOAT_SSIM_MEAN = "*mean = (double)(float)*mean;"
MS_SSIM_KERNEL = "integer_ms_ssim/ms_ssim_score.cu"
MS_SSIM_HOST = "integer_ms_ssim_cuda.c"
# What makes float_ms_ssim_cuda the CPU's arithmetic (ADR-1403).
MS_SSIM_KERNEL_PIECES = (
    "row_acc = __fmaf_rn(src_buf[yi * (int)w + xi], LPF[ku], row_acc);",
    "acc = __fmaf_rn(row_acc, LPF[kv], acc);",
    "ms_pair_add(ref_mu_h, __fmul_rn(r, w));",
    "ms_pair_add(ref_sq_h, __fmul_rn(__fmul_rn(r, r), w));",
    "ms_pair_add(refcmp_h, __fmul_rn(__fmul_rn(r, c), w));",
    "ms_pair_add(ref_mu_v, __fmul_rn(h.ref_mu[src_idx], w));",
    "sum.lo = __fadd_rn(sum.lo, error);",
    "return __fadd_rn(sum.hi, sum.lo);",
    "const float sigma_xy_geom = __fsqrt_rn(ref_var * cmp_var);",
    "(double)(ref_mu * ref_mu + cmp_mu * cmp_mu + C1);",
    "(double)(ref_var + cmp_var + C2);",
    "out.s = (double)((clamped_covar + C3) / (sigma_xy_geom + C3));",
)
MS_SSIM_HOST_PIECES = (
    "l_means[i] = (double)(float)(total_l / n_pixels);",
    "c_means[i] = (double)(float)(total_c / n_pixels);",
    "s_means[i] = (double)(float)(total_s / n_pixels);",
    "const float C1 = (K1 * (float)L) * (K1 * (float)L);",
    "const float C3 = C2 / 2.0f;",
)
FLOAT_MOTION_KERNEL = "float_motion/float_motion_score.cu"
FLOAT_MOTION_HOST = "float_motion_cuda.c"
FLOAT_MOTION_SAD = "float_motion_sad.h"
FLOAT_MOTION_SAD_PATH = ROOT / "core" / "src" / "feature" / FLOAT_MOTION_SAD
# What makes float_motion_cuda's SAD the CPU's running fp32 sum (ADR-1409).
FLOAT_MOTION_KERNEL_PIECES = (
    "const unsigned y = blockIdx.x * blockDim.x + threadIdx.x;",
    "for (unsigned j = 0; j < width; j++) {",
    "const float diff = cur[j] - prev[j];",
    "accum += diff < 0.0f ? -diff : diff;",
    "row_sad[y] = accum;",
)
FLOAT_MOTION_SAD_PIECES = (
    "float accum = 0.0f;",
    "accum += row_sad[i];",
    "return (double)(accum / (float)(int)(w * h));",
)
FLOAT_MOTION_REDUCE = "return vmaf_float_motion_score_from_row_sads("
# Any of these in the kernel file is a reduction in another order.
FLOAT_MOTION_OTHER_REDUCTION = re.compile(r"__shfl\w*|atomicAdd|__shared__\s+float\s+\w*sad")
SOURCES = (
    MOTION_KERNEL,
    MOTION_SAD,
    *MOTION_TUS,
    SSIM_KERNEL,
    INTEGER_SSIM_KERNEL,
    ADM_KERNEL,
    PSNR_KERNEL,
    MOMENT_KERNEL,
    "integer_psnr_cuda.c",
    "ssim_cuda.c",
    "integer_ssim_cuda.c",
    MS_SSIM_KERNEL,
    MS_SSIM_HOST,
    FLOAT_MOTION_KERNEL,
    FLOAT_MOTION_HOST,
    "integer_vif_cuda.c",
)

HOST_WAIT = re.compile(
    r"\bcu(?:StreamSynchronize|CtxSynchronize|EventSynchronize)\s*\(|"
    r"\bvmaf_cuda_kernel_collect_wait\s*\("
)
MOTION_DIFF_FIRST = re.compile(r"load_sample<T>\(prev,[^;]*?\)\s*-\s*load_sample<T>\(cur,", re.S)
SUBMIT_FNS = {
    MOTION_SAD: ("vmaf_cuda_motion_sad_submit", "motion_sad_stage", "motion_sad_launch"),
    "integer_motion_cuda.c": ("submit_fex_cuda",),
    "integer_motion_v2_cuda.c": ("submit_fex_cuda",),
}


MOTION_CLIP_SITES = 3  # motion_clip() call sites in the motion twin
MOTION_BLEND_CLIP_SITES = 3  # motion_blend_clip() call sites in the motion twin
SSIM_TERMS_SITES = 3  # ssim_terms() call sites in the ssim kernel
DWT2_TAP_SITES = 4  # adm_dwt2_s123_tap() call sites in the ADM kernel


def _code(source: str) -> str:
    """The source without its comments, so prose cannot satisfy or trip a check."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def _code_meson(source: str) -> str:
    """meson.build without its comments."""
    return re.sub(r"#[^\n]*", "", source)


def _sources() -> dict[str, str]:
    sources = {name: (CUDA_ROOT / name).read_text(encoding="utf-8") for name in SOURCES}
    sources[MESON] = MESON_PATH.read_text(encoding="utf-8")
    sources[LIBVMAF] = LIBVMAF_PATH.read_text(encoding="utf-8")
    sources[FLOAT_MOTION_SAD] = FLOAT_MOTION_SAD_PATH.read_text(encoding="utf-8")
    return sources


def _function_body(source: str, name: str) -> str:
    """Text of the top-level C function `name`, signature to closing brace."""
    match = re.search(
        rf"^(?:static )?(?:inline )?[\w ]*\b{name}\([^;{{]*?\)\s*^{{.*?^}}$", source, re.S | re.M
    )
    return match.group(0) if match else ""


def _motion_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if not MOTION_DIFF_FIRST.search(sources[MOTION_KERNEL]):
        failures.append(f"{MOTION_KERNEL}: the tile no longer stages prev - cur before the blur")
    for name in MOTION_TUS:
        source = _code(sources[name])
        if "vmaf_cuda_motion_sad_submit(" not in source:
            failures.append(f"{name}: does not run the shared diff-first SAD pipeline")
        if re.search(r"\bcuModuleLoadData\s*\(|\bcuLaunchKernel\s*\(", source):
            failures.append(f"{name}: loads or launches a motion kernel of its own")
        if re.search(r"\bblur(?:red)?\s*\[", source):
            failures.append(f"{name}: keeps a blurred frame across submits")
    for name, functions in SUBMIT_FNS.items():
        for fn in functions:
            body = _function_body(sources[name], fn)
            if not body:
                failures.append(f"{name}: {fn}() not found")
            elif HOST_WAIT.search(body):
                failures.append(f"{name}: {fn}() waits on the host mid-frame")
    submit = _function_body(sources[MOTION_SAD], "vmaf_cuda_motion_sad_submit")
    if not re.search(r"cuStreamWaitEvent\(stream,\s*frame->prev_done", submit):
        failures.append(f"{MOTION_SAD}: the ping-pong is not ordered by the previous frame's event")
    motion = sources["integer_motion_cuda.c"]
    if len(re.findall(r"\bcuStreamSynchronize\s*\(", motion)) != 1:
        failures.append("integer_motion_cuda.c: the SAD readback must wait exactly once")
    return failures


def _option_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    psnr = sources["integer_psnr_cuda.c"]
    for helper in (
        "vmaf_psnr_peak(",
        "vmaf_psnr_max(",
        "vmaf_psnr_from_mse(",
        "vmaf_psnr_aggregate(",
        "vmaf_psnr_clip_sse_add(",
    ):
        if helper not in psnr:
            failures.append(f"integer_psnr_cuda.c: does not call psnr_score.h {helper}")
    if re.search(r"\blog10\s*\(", _code(psnr)):
        failures.append("integer_psnr_cuda.c: carries its own copy of the PSNR arithmetic")
    for name in ("ssim_cuda.c", "integer_ssim_cuda.c"):
        if "vmaf_ssim_max_db(s->clip_db" not in sources[name]:
            failures.append(f"{name}: clip_db ceiling does not come from vmaf_ssim_max_db()")
        if "s->enable_db, s->max_db" not in sources[name]:
            failures.append(f"{name}: the score is not emitted through enable_db / max_db")
    fssim = sources["integer_ssim_cuda.c"]
    if '.name = "enable_chroma"' not in fssim or '"ignored: float_ssim is luma only' not in fssim:
        failures.append(
            "integer_ssim_cuda.c: float_ssim_cuda must keep enable_chroma as a documented, "
            "ignored option (HISS-14)"
        )
    failures.extend(_psnr_stream_failures(psnr))
    failures.extend(_motion_v2_option_failures(sources["integer_motion_v2_cuda.c"]))
    failures.extend(_motion_five_frame_failures(sources["integer_motion_cuda.c"]))
    motion = sources[FLOAT_MOTION_HOST]
    if motion.count("motion_clip(s, ") != MOTION_CLIP_SITES:
        failures.append(
            "float_motion_cuda.c: motion2, debug motion and tail motion2 must all "
            "be motion_clip()ped"
        )
    failures.extend(_float_motion3_failures(motion))
    return failures


def _float_motion3_failures(motion: str) -> list[str]:
    """float_motion_cuda emits the CPU's motion3 (T-GPU-FLOAT-MOTION3-MISSING-2026-09-30)."""
    failures: list[str] = []
    if '"VMAF_feature_motion3_score", NULL}' not in _code(motion):
        failures.append("float_motion_cuda.c: motion3 is not a provided feature")
    if _code(motion).count("motion_blend_clip(s, ") != MOTION_BLEND_CLIP_SITES:
        failures.append(
            "float_motion_cuda.c: motion3 of frame 0, of each middle frame and of the tail "
            "must all be motion_blend_clip()ped"
        )
    return failures


def _psnr_stream_failures(psnr: str) -> list[str]:
    failures: list[str] = []
    submit = _code(_function_body(psnr, "submit_fex_cuda"))
    memsets = re.findall(r"cuMemsetD8Async\(([^;]*)\)\);", submit)
    if not memsets or any(not m.rstrip().endswith("pic_stream") for m in memsets):
        failures.append(
            "integer_psnr_cuda.c: a plane accumulator is zeroed off the kernels' picture stream"
        )
    if "VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_TEMPORAL" not in psnr:
        failures.append("integer_psnr_cuda.c: psnr_cuda is not TEMPORAL like the CPU psnr")
    return failures


def _motion_v2_option_failures(source: str) -> list[str]:
    failures: list[str] = []
    if MOTION_V2_SAD_SCORE not in _function_body(source, "collect_fex_cuda"):
        failures.append(
            "integer_motion_v2_cuda.c: the SAD score is not fps-weighted and capped like the CPU"
        )
    # integer_motion.c::vmaf_motion_window_flush() is the one derivation of
    # motion2 / motion3 from the stored SADs (ADR-1478); the twin calls it for
    # both windows (ADR-1491) and so has no end case or weighting of its own.
    code = _code(source)
    if MOTION_WINDOW_CALL not in _code(_function_body(source, "flush_fex_cuda")):
        failures.append(
            "integer_motion_v2_cuda.c: flush does not derive motion2_v2 / motion3_v2 "
            "with the CPU's window function"
        )
    if "vmaf_feature_collector_get_score" in code:
        failures.append(
            "integer_motion_v2_cuda.c: the twin reads stored scores back, a window of its own"
        )
    return failures


def _motion_five_frame_failures(source: str) -> list[str]:
    """motion_cuda derives the five-frame window with the CPU's function (ADR-1491)."""
    if MOTION_WINDOW_CALL in _code(_function_body(source, "motion_flush_window")):
        return []
    return [
        "integer_motion_cuda.c: the five-frame window is not derived with the CPU's window function"
    ]


def _ssim_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    kernel = _code(sources[SSIM_KERNEL])
    if re.search(r"==[^;?]*\?\s*1\.0f?\s*:", kernel):
        failures.append(f"{SSIM_KERNEL}: forces identical windows to exactly 1; the CPU does not")
    for piece in (
        "(double)l_den)",
        "(double)c_den)",
        "__fdiv_rn(__fadd_rn(csb, c3), __fadd_rn(srsc, c3))",
        "__dmul_rn(__dmul_rn(t.l, t.c), t.s)",
    ):
        if piece not in kernel:
            failures.append(
                f"{SSIM_KERNEL}: ssim_terms() no longer has the CPU's types and rounding ({piece})"
            )
    if kernel.count("ssim_terms(") != SSIM_TERMS_SITES:
        failures.append(f"{SSIM_KERNEL}: both pass-2 kernels must share ssim_terms()")
    if FLOAT_SSIM_MEAN not in sources["integer_ssim_cuda.c"]:
        failures.append("integer_ssim_cuda.c: the frame mean is not rounded to fp32 like the CPU's")
    meson = _code_meson(sources[MESON])
    if CUDA_DEVICE_FMAD not in meson or CUDA_FATBIN_FP_ARGS not in meson:
        failures.append("core/src/meson.build: integer_ssim_score is compiled with FMA contraction")
    if "*term = w_d * a * b / den;" not in sources[INTEGER_SSIM_KERNEL]:
        failures.append(f"{INTEGER_SSIM_KERNEL}: the SSIM term is not grouped as the CPU groups it")
    return failures


FLOAT_SSIM_TAP = "return __dadd_rn(sum, (double)__fmul_rn(sample, weight));"
# add_tap(): one definition, five horizontal and five vertical taps.
FLOAT_SSIM_TAP_USES = 11
FLOAT_SSIM_WINDOW_TERM = "sum += __float2ll_rz(__fmul_rn(product, DECIMATE_FIXED_ONE));"
FLOAT_SSIM_WINDOW_ROUND = "return __fmul_rn(__ll2float_rn(sum), DECIMATE_FIXED_INV);"
FLOAT_SSIM_PLANE_SIZE = "(unsigned)iqa_decimate_dim((int)extent, scale)"
FLOAT_SSIM_SUBMIT_FNS = (
    "submit_fex_cuda",
    "float_ssim_launch_passes",
    "float_ssim_launch_decimate",
    "float_ssim_launch_horiz_planes",
    "integer_ssim_launch_horiz",
    "integer_ssim_launch_vert",
)


def _float_ssim_pipeline_failures(sources: dict[str, str]) -> list[str]:
    """ADR-1399: the CPU's decimation and convolution arithmetic on the device."""
    failures: list[str] = []
    kernel = _code(sources[SSIM_KERNEL])
    if FLOAT_SSIM_TAP not in kernel or kernel.count("add_tap(") != FLOAT_SSIM_TAP_USES:
        failures.append(
            f"{SSIM_KERNEL}: a convolution tap is not an fp32 product added to a double sum"
        )
    if re.search(r"\+=\s*(?:w|weight|G\[\w+\])\s*\*", kernel):
        failures.append(f"{SSIM_KERNEL}: a convolution sum accumulates in fp32")
    for piece in (FLOAT_SSIM_WINDOW_TERM, FLOAT_SSIM_WINDOW_ROUND, "const int half = g.scale / 2;"):
        if piece not in kernel:
            failures.append(
                f"{SSIM_KERNEL}: the decimation window is not iqa_decimate()'s exact sum ({piece})"
            )
    host = sources["integer_ssim_cuda.c"]
    if FLOAT_SSIM_PLANE_SIZE not in _function_body(host, "decimated_extent"):
        failures.append("integer_ssim_cuda.c: the decimated plane size is not iqa_decimate_dim()")
    if "s->tap_weight = 1.0f / (float)(s->scale * s->scale);" not in _code(host):
        failures.append("integer_ssim_cuda.c: the low-pass tap is not ssim.c's fp32 reciprocal")
    for fn in FLOAT_SSIM_SUBMIT_FNS:
        body = _function_body(host, fn)
        if not body:
            failures.append(f"integer_ssim_cuda.c: {fn}() not found")
        elif HOST_WAIT.search(body):
            failures.append(f"integer_ssim_cuda.c: {fn}() waits on the host mid-frame")
    return failures


def _ms_ssim_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    kernel = _code(sources[MS_SSIM_KERNEL])
    for piece in MS_SSIM_KERNEL_PIECES:
        if piece not in kernel:
            failures.append(f"{MS_SSIM_KERNEL}: no longer the CPU's arithmetic ({piece})")
    # A plain `acc += a * b` is neither the reference's fused decimate tap nor
    # its fp64 window sum (carried as an exact fp32 pair).
    if re.search(r"\b\w+ \+= [^;]*\*[^;]*;", kernel):
        failures.append(f"{MS_SSIM_KERNEL}: an fp32 multiply-accumulate replaces the CPU's sum")
    host = _code(sources[MS_SSIM_HOST])
    for piece in MS_SSIM_HOST_PIECES:
        if piece not in host:
            failures.append(f"{MS_SSIM_HOST}: no longer combines as the CPU does ({piece})")
    return failures


def _float_motion_sum_failures(sources: dict[str, str]) -> list[str]:
    """float_motion_cuda adds its SAD in compute_motion_simd()'s order (ADR-1409)."""
    failures: list[str] = []
    kernel = _code(sources[FLOAT_MOTION_KERNEL])
    for piece in FLOAT_MOTION_KERNEL_PIECES:
        if piece not in kernel:
            failures.append(f"{FLOAT_MOTION_KERNEL}: not the CPU's row sum ({piece})")
    if FLOAT_MOTION_OTHER_REDUCTION.search(kernel):
        failures.append(
            f"{FLOAT_MOTION_KERNEL}: a block, warp or atomic reduction adds the SAD in an "
            "order the CPU does not use"
        )
    helper = _code(sources[FLOAT_MOTION_SAD])
    for piece in FLOAT_MOTION_SAD_PIECES:
        if piece not in helper:
            failures.append(f"{FLOAT_MOTION_SAD}: not the CPU's sum over the rows ({piece})")
    reduce_body = _function_body(_code(sources[FLOAT_MOTION_HOST]), "reduce_sad")
    if FLOAT_MOTION_REDUCE not in reduce_body or "+=" in reduce_body:
        failures.append(
            f"{FLOAT_MOTION_HOST}: reduce_sad() must return the shared "
            "vmaf_float_motion_score_from_row_sads() value and keep no sum of its own"
        )
    return failures


FLOAT_MOTION_TILE_INDEX = "vmaf_cuda_tile_index(vmaf_cuda_reflect_101(idx, sup), sup)"


def _guard_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    # T-CUDA-FLOAT-MOTION-TILE-READ-BEFORE-PLANE-2026-10-05: a padding load of
    # a plane narrower or shorter than one tile reflected to a negative index.
    if FLOAT_MOTION_TILE_INDEX not in _code(sources[FLOAT_MOTION_KERNEL]):
        failures.append(
            f"{FLOAT_MOTION_KERNEL}: a tile load can reflect to an index outside the plane"
        )
    adm = sources[ADM_KERNEL]
    if "adm_dwt2_source_row(y_out, i, h)" not in adm:
        failures.append(f"{ADM_KERNEL}: scale-0 rows bypass the clamped adm_dwt2_source_row()")
    if adm.count("adm_dwt2_s123_tap(") != DWT2_TAP_SITES:
        failures.append(f"{ADM_KERNEL}: scale 1-3 taps bypass adm_dwt2_s123_tap()")
    vif = sources["integer_vif_cuda.c"]
    if (
        ".context_check = check_context_cuda" not in vif
        or '.context_fallback_name = "vif"' not in vif
    ):
        failures.append("integer_vif_cuda.c: vif_cuda no longer declares its CPU fallback")
    init = _function_body(vif, "init_fex_cuda")
    if init.find("vif_cuda_min_dim()") < 0 or init.find("vif_cuda_min_dim()") > init.find(
        "fex->cu_state"
    ):
        failures.append("integer_vif_cuda.c: init() must refuse small frames before CUDA state")
    return failures


# (engine function, the init call, the first text of the path decision)
DISPATCH_SITES = (
    (
        "read_pictures_cuda_submit_current",
        "init_before_dispatch(fex_ctx, ref_device)",
        "!fex_ctx->fex->submit || !fex_ctx->fex->collect",
    ),
    (
        "read_pictures_dispatch_one",
        "init_before_dispatch(fex_ctx, ref)",
        "dispatch_gpu_double_buffer(",
    ),
)


def _dispatch_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for function, init_call, decision in DISPATCH_SITES:
        body = _code(_function_body(sources[LIBVMAF], function))
        at_init = body.find(init_call)
        at_decision = body.find(decision)
        if at_init < 0 or at_decision < 0 or at_init > at_decision:
            failures.append(f"core/src/libvmaf.c: {function}() picks a path before init()")
    return failures


def _reduction_failures(sources: dict[str, str]) -> list[str]:
    """ADR-1392: one atomic per block, the separable motion filter, no
    runtime index into a by-value picture parameter."""
    failures: list[str] = []
    for name in (MOTION_KERNEL, PSNR_KERNEL):
        code = _code(sources[name])
        if len(re.findall(r"\batomicAdd\s*\(", code)) != 1 or not re.search(
            r"if \(lid == 0u\)\s*atomicAdd\(", code
        ):
            failures.append(f"{name}: the block sum must reach memory with one atomic per block")
    if "vertical_pass<VAcc>(s_diff, s_v, bpc);" not in _code(sources[MOTION_KERNEL]):
        failures.append(f"{MOTION_KERNEL}: the vertical pass is no longer computed once per block")
    if re.search(r"\.(?:data|stride)\[plane\]", _code(sources[PSNR_KERNEL])):
        failures.append(f"{PSNR_KERNEL}: indexes the by-value picture with the runtime plane")
    moment = _code(sources[MOMENT_KERNEL])
    if len(re.findall(r"\batomicAdd\s*\(", moment)) != 1 or not re.search(
        r"if \(lid < MOMENT_SUMS\) \{[^}]*atomicAdd\(&acc\[lid\], sum\);", moment
    ):
        failures.append(
            f"{MOMENT_KERNEL}: each block sum must reach its accumulator with one atomic per block"
        )
    return failures


def _all_failures(sources: dict[str, str]) -> list[str]:
    return [
        *_motion_failures(sources),
        *_reduction_failures(sources),
        *_option_failures(sources),
        *_ssim_failures(sources),
        *_float_ssim_pipeline_failures(sources),
        *_ms_ssim_failures(sources),
        *_float_motion_sum_failures(sources),
        *_guard_failures(sources),
        *_dispatch_failures(sources),
    ]


class CudaKernelSourceContractTest(unittest.TestCase):
    def test_live_sources_keep_the_contracts(self) -> None:
        self.assertEqual(_all_failures(_sources()), [])

    def _assert_detected(self, sources: dict[str, str], needle: str) -> None:
        failures = _all_failures(sources)
        self.assertTrue(any(needle in item for item in failures), failures)

    def _edit(self, name: str, old: str, new: str) -> dict[str, str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return sources

    def test_blur_before_difference_is_detected(self) -> None:
        sources = self._edit(
            MOTION_KERNEL,
            "load_sample<T>(prev, prev_stride, gy, gx) - load_sample<T>(cur, cur_stride, gy, gx)",
            "load_sample<T>(cur, cur_stride, gy, gx) - load_sample<T>(prev, prev_stride, gy, gx)",
        )
        self._assert_detected(sources, "prev - cur")

    def test_own_motion_kernel_is_detected(self) -> None:
        sources = _sources()
        sources["integer_motion_cuda.c"] += "\nstatic CUmodule m; cuModuleLoadData(&m, p);\n"
        self._assert_detected(sources, "of its own")

    def test_blurred_frame_ring_is_detected(self) -> None:
        sources = _sources()
        sources["integer_motion_cuda.c"] += "\nVmafCudaBuffer *blur[2];\n"
        self._assert_detected(sources, "blurred frame")

    def test_submit_host_wait_is_detected(self) -> None:
        sources = self._edit(
            "integer_motion_cuda.c",
            "    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, pic_stream));\n",
            "    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, pic_stream));\n"
            "    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(pic_stream));\n",
        )
        self._assert_detected(sources, "waits on the host mid-frame")

    def test_unordered_ping_pong_is_detected(self) -> None:
        sources = self._edit(
            MOTION_SAD,
            "cuStreamWaitEvent(stream, frame->prev_done",
            "cuStreamWaitEvent(stream, NULL",
        )
        self._assert_detected(sources, "ordered by the previous frame's event")

    def test_double_readback_wait_is_detected(self) -> None:
        sources = _sources()
        sources["integer_motion_cuda.c"] += "\nstatic void f(void) { cuStreamSynchronize(s); }\n"
        self._assert_detected(sources, "wait exactly once")

    def test_copied_psnr_arithmetic_is_detected(self) -> None:
        sources = _sources()
        sources["integer_psnr_cuda.c"] += "\nstatic double p(double x) { return log10(x); }\n"
        self._assert_detected(sources, "own copy of the PSNR arithmetic")

    def test_unweighted_debug_motion_is_detected(self) -> None:
        sources = self._edit(
            "float_motion_cuda.c",
            "motion_clip(s, motion_score), index);",
            "motion_score, index);",
        )
        self._assert_detected(sources, "motion_clip()ped")

    def test_dropped_motion3_feature_is_detected(self) -> None:
        sources = self._edit(
            "float_motion_cuda.c",
            '"VMAF_feature_motion3_score", NULL}',
            "NULL}",
        )
        self._assert_detected(sources, "motion3 is not a provided feature")

    def test_unblended_tail_motion3_is_detected(self) -> None:
        sources = self._edit(
            "float_motion_cuda.c",
            "motion_blend_clip(s, s->prev_motion_score), s->index);",
            "motion_clip(s, s->prev_motion_score), s->index);",
        )
        self._assert_detected(sources, "motion_blend_clip()ped")

    def test_removed_enable_chroma_is_detected(self) -> None:
        sources = self._edit("integer_ssim_cuda.c", '.name = "enable_chroma"', '.name = "chroma"')
        self._assert_detected(sources, "HISS-14")

    def test_forced_exact_one_is_detected(self) -> None:
        sources = self._edit(
            SSIM_KERNEL,
            "t.ssim = __dmul_rn(__dmul_rn(t.l, t.c), t.s);",
            "t.ssim = (l_den == c_den) ? 1.0 : __dmul_rn(__dmul_rn(t.l, t.c), t.s);",
        )
        self._assert_detected(sources, "forces identical windows")

    def test_fp32_ssim_denominator_dropped_is_detected(self) -> None:
        sources = self._edit(SSIM_KERNEL, "(double)l_den)", "(double)l_den * 1.0)")
        self._assert_detected(sources, "types and rounding")

    def test_unrounded_frame_mean_is_detected(self) -> None:
        sources = self._edit("integer_ssim_cuda.c", FLOAT_SSIM_MEAN, "(void)mean;")
        self._assert_detected(sources, "rounded to fp32")

    def test_fp32_convolution_sum_is_detected(self) -> None:
        # The pre-ADR-1399 passes accumulated in fp32 (and NVCC fused each tap).
        sources = self._edit(
            SSIM_KERNEL,
            FLOAT_SSIM_TAP,
            "return (double)((float)sum + sample * weight);",
        )
        self._assert_detected(sources, "added to a double sum")
        sources = _sources()
        sources[SSIM_KERNEL] += "\nvoid f(float &m, float w, float r) { m += w * r; }\n"
        self._assert_detected(sources, "accumulates in fp32")

    def test_fp32_decimation_window_is_detected(self) -> None:
        sources = self._edit(SSIM_KERNEL, FLOAT_SSIM_WINDOW_TERM, "fsum += product;")
        self._assert_detected(sources, "iqa_decimate()'s exact sum")
        sources = self._edit(SSIM_KERNEL, FLOAT_SSIM_WINDOW_ROUND, "return (float)sum * 0x1p-52f;")
        self._assert_detected(sources, "iqa_decimate()'s exact sum")

    def test_own_decimated_size_rule_is_detected(self) -> None:
        sources = self._edit(
            "integer_ssim_cuda.c", FLOAT_SSIM_PLANE_SIZE, "(extent + scale - 1) / scale"
        )
        self._assert_detected(sources, "iqa_decimate_dim()")

    def test_float_ssim_submit_host_wait_is_detected(self) -> None:
        sources = self._edit(
            "integer_ssim_cuda.c",
            "    return integer_ssim_launch_vert(s, cu_f, stream, grid_x, grid_y);\n",
            "    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));\n"
            "    return integer_ssim_launch_vert(s, cu_f, stream, grid_x, grid_y);\n",
        )
        self._assert_detected(sources, "float_ssim_launch_passes() waits on the host")

    def test_contracted_integer_ssim_is_detected(self) -> None:
        sources = self._edit(MESON, CUDA_FATBIN_FP_ARGS, "cuda_flags")
        self._assert_detected(sources, "FMA contraction")
        sources = self._edit(MESON, CUDA_DEVICE_FMAD, "cuda_device_strict_fp_args = []")
        self._assert_detected(sources, "FMA contraction")

    def test_unfused_ms_ssim_decimate_is_detected(self) -> None:
        sources = self._edit(
            MS_SSIM_KERNEL,
            "acc = __fmaf_rn(row_acc, LPF[kv], acc);",
            "acc += row_acc * LPF[kv];",
        )
        self._assert_detected(sources, "__fmaf_rn(row_acc")
        self._assert_detected(sources, "fp32 multiply-accumulate")

    def test_fp32_ms_ssim_window_sum_is_detected(self) -> None:
        sources = self._edit(
            MS_SSIM_KERNEL,
            "ms_pair_add(ref_sq_h, __fmul_rn(__fmul_rn(r, r), w));",
            "ref_sq_h.hi += (r * r) * w;",
        )
        self._assert_detected(sources, "fp32 multiply-accumulate")

    def test_ms_ssim_pair_sum_without_its_error_term_is_detected(self) -> None:
        sources = self._edit(MS_SSIM_KERNEL, "sum.lo = __fadd_rn(sum.lo, error);", "(void)error;")
        self._assert_detected(sources, "sum.lo = __fadd_rn")

    def test_approximate_ms_ssim_square_root_is_detected(self) -> None:
        sources = self._edit(
            MS_SSIM_KERNEL, "__fsqrt_rn(ref_var * cmp_var)", "sqrtf(ref_var * cmp_var)"
        )
        self._assert_detected(sources, "__fsqrt_rn(ref_var * cmp_var)")

    def test_fp64_ms_ssim_denominator_is_detected(self) -> None:
        sources = self._edit(
            MS_SSIM_KERNEL,
            "(double)(ref_var + cmp_var + C2);",
            "((double)ref_var + (double)cmp_var + c2);",
        )
        self._assert_detected(sources, "ref_var + cmp_var + C2")

    def test_unrounded_ms_ssim_scale_mean_is_detected(self) -> None:
        sources = self._edit(
            MS_SSIM_HOST,
            "c_means[i] = (double)(float)(total_c / n_pixels);",
            "c_means[i] = total_c / n_pixels;",
        )
        self._assert_detected(sources, "combines as the CPU does")

    def test_unclamped_float_motion_tile_mirror_is_detected(self) -> None:
        # The master form: reflect-101 without the clamp into the plane.
        sources = self._edit(
            FLOAT_MOTION_KERNEL,
            FLOAT_MOTION_TILE_INDEX,
            "idx < 0 ? -idx : (idx >= sup ? 2 * (sup - 1) - idx : idx)",
        )
        self._assert_detected(sources, "reflect to an index outside the plane")

    def test_block_reduced_float_motion_sad_is_detected(self) -> None:
        sources = _sources()
        sources[FLOAT_MOTION_KERNEL] += (
            "\n__device__ float fm_warp_reduce(float v)\n"
            "{\n    return v + __shfl_down_sync(0xffffffffu, v, 16);\n}\n"
        )
        self._assert_detected(sources, "an order the CPU does not use")

    def test_pairwise_float_motion_row_sum_is_detected(self) -> None:
        sources = self._edit(
            FLOAT_MOTION_KERNEL,
            "for (unsigned j = 0; j < width; j++) {",
            "for (unsigned j = threadIdx.y; j < width; j += blockDim.y) {",
        )
        self._assert_detected(sources, "not the CPU's row sum")

    def test_fp64_float_motion_row_total_is_detected(self) -> None:
        sources = self._edit(
            FLOAT_MOTION_SAD,
            "float accum = 0.0f;",
            "double accum = 0.0;",
        )
        self._assert_detected(sources, "not the CPU's sum over the rows")

    def test_fp64_float_motion_division_is_detected(self) -> None:
        sources = self._edit(
            FLOAT_MOTION_SAD,
            "return (double)(accum / (float)(int)(w * h));",
            "return (double)accum / ((double)w * h);",
        )
        self._assert_detected(sources, "not the CPU's sum over the rows")

    def test_own_float_motion_host_sum_is_detected(self) -> None:
        sources = self._edit(
            FLOAT_MOTION_HOST,
            "    return vmaf_float_motion_score_from_row_sads((const float *)s->rb.host_pinned, "
            "s->frame_w,\n                                                 s->frame_h);",
            "    double total = 0.0;\n"
            "    for (unsigned i = 0; i < s->frame_h; i++)\n"
            "        total += (double)((const float *)s->rb.host_pinned)[i];\n"
            "    return total / ((double)s->frame_w * s->frame_h);",
        )
        self._assert_detected(sources, "keep no sum of its own")

    def test_regrouped_integer_ssim_term_is_detected(self) -> None:
        sources = self._edit(
            INTEGER_SSIM_KERNEL, "*term = w_d * a * b / den;", "*term = w_d * (a * b / den);"
        )
        self._assert_detected(sources, "grouped as the CPU")

    def test_chroma_memset_on_readback_stream_is_detected(self) -> None:
        sources = self._edit(
            "integer_psnr_cuda.c",
            "cuMemsetD8Async(s->rb[p].device->data, 0, s->rb[p].bytes, pic_stream)",
            "cuMemsetD8Async(s->rb[p].device->data, 0, s->rb[p].bytes, s->lc.str)",
        )
        self._assert_detected(sources, "off the kernels' picture stream")

    def test_non_temporal_psnr_is_detected(self) -> None:
        sources = self._edit(
            "integer_psnr_cuda.c",
            "VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_TEMPORAL",
            "VMAF_FEATURE_EXTRACTOR_CUDA",
        )
        self._assert_detected(sources, "TEMPORAL")

    def test_unweighted_motion_v2_sad_is_detected(self) -> None:
        sources = self._edit("integer_motion_v2_cuda.c", MOTION_V2_SAD_SCORE, "sad_score")
        self._assert_detected(sources, "fps-weighted and capped")

    def test_motion_v2_own_flush_is_detected(self) -> None:
        sources = self._edit(
            "integer_motion_v2_cuda.c",
            MOTION_WINDOW_CALL,
            "motion_v2_flush_scores(s, feature_collector, &window)",
        )
        self._assert_detected(sources, "the CPU's window function")

    def test_motion_v2_reading_scores_back_is_detected(self) -> None:
        sources = self._edit(
            "integer_motion_v2_cuda.c",
            "    const int err = " + MOTION_WINDOW_CALL + ";\n",
            "    double score_cur = 0.0;\n"
            "    (void)vmaf_feature_collector_get_score(feature_collector, window.sad_feature,\n"
            "                                           &score_cur, 0u);\n"
            "    const int err = " + MOTION_WINDOW_CALL + ";\n",
        )
        self._assert_detected(sources, "a window of its own")

    def test_motion_own_five_frame_window_is_detected(self) -> None:
        sources = self._edit(
            "integer_motion_cuda.c",
            MOTION_WINDOW_CALL,
            "motion_flush_five_frame_scores(s, feature_collector, &window)",
        )
        self._assert_detected(sources, "five-frame window is not derived")

    def test_unclamped_adm_rows_are_detected(self) -> None:
        sources = self._edit(
            ADM_KERNEL,
            "adm_dwt2_source_row(y_out, i, h)",
            "adm_dwt2_reflect_row(y_out, i, h)",
        )
        self._assert_detected(sources, "clamped adm_dwt2_source_row()")

    def test_missing_vif_fallback_is_detected(self) -> None:
        sources = self._edit(
            "integer_vif_cuda.c",
            '.context_fallback_name = "vif"',
            ".context_fallback_name = NULL",
        )
        self._assert_detected(sources, "CPU fallback")

    def test_cuda_dispatch_before_init_is_detected(self) -> None:
        sources = self._edit(LIBVMAF, "err = init_before_dispatch(fex_ctx, ref_device);", "")
        self._assert_detected(sources, "read_pictures_cuda_submit_current() picks a path")

    def test_per_warp_psnr_atomic_is_detected(self) -> None:
        sources = self._edit(
            PSNR_KERNEL,
            "if ((lid & 31u) == 0u)\n        s_warp[lid >> 5] = v;",
            "if ((lid & 31u) == 0u)\n        atomicAdd(sse, v);",
        )
        self._assert_detected(sources, "one atomic per block")

    def test_per_warp_motion_atomic_is_detected(self) -> None:
        sources = self._edit(
            MOTION_KERNEL,
            "if ((lid & 31u) == 0u)\n        s_warp[lid >> 5] = v;",
            "if ((lid & 31u) == 0u)\n        atomicAdd(sad, v);",
        )
        self._assert_detected(sources, "one atomic per block")

    def test_per_warp_moment_atomic_is_detected(self) -> None:
        sources = self._edit(
            MOMENT_KERNEL,
            "s_warp[k][lid >> 5] = m.v[k];",
            "atomicAdd(&acc[k], m.v[k]);",
        )
        self._assert_detected(sources, "one atomic per block")

    def test_per_output_vertical_pass_is_detected(self) -> None:
        sources = self._edit(MOTION_KERNEL, "vertical_pass<VAcc>(s_diff, s_v, bpc);", "")
        self._assert_detected(sources, "once per block")

    def test_runtime_plane_index_is_detected(self) -> None:
        sources = self._edit(
            PSNR_KERNEL,
            "const T *ref_row = plane_row<T>(ref, plane, y);",
            "const T *ref_row = reinterpret_cast<const T *>(\n"
            "        reinterpret_cast<const uint8_t *>(ref.data[plane]) + y * ref.stride[plane]);",
        )
        self._assert_detected(sources, "runtime plane")

    def test_gpu_dispatch_before_init_is_detected(self) -> None:
        sources = self._edit(
            LIBVMAF,
            "const int init_err = init_before_dispatch(fex_ctx, ref);",
            "const int init_err = 0;",
        )
        self._assert_detected(sources, "read_pictures_dispatch_one() picks a path")


if __name__ == "__main__":
    unittest.main()

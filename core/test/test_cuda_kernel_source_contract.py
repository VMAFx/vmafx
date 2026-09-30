#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CUDA RC3 parity design at source level (ADR-1372, ADR-1373, ADR-1374, ADR-1392).

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
  flush derives motion2_v2 / motion3_v2 from it without re-weighting, also
  for a one-frame input;
- float SSIM: each pixel is the CPU's l * c * s with double numerators over
  fp32 denominators (no forced exact 1), and the frame mean is rounded to
  fp32; `enable_chroma` stays declared as an ignored option (HISS-14);
- integer SSIM: the combine compiles with --fmad=false and groups the term
  as the CPU does, ((w * a) * b) / den;
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
INTEGER_SSIM_FMAD = "'integer_ssim_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
INTEGER_SSIM_KERNEL = "integer_ssim/integer_ssim_score.cu"
PSNR_KERNEL = "integer_psnr/psnr_score.cu"
MOMENT_KERNEL = "integer_moment/moment_score.cu"
MOTION_V2_SAD_SCORE = "MIN(sad_score * s->motion_fps_weight, s->motion_max_val)"
FLOAT_SSIM_MEAN = "*mean = (double)(float)*mean;"
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
    "float_motion_cuda.c",
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
    motion = sources["float_motion_cuda.c"]
    if motion.count("motion_clip(s, ") != 3:
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
    if _code(motion).count("motion_blend_clip(s, ") != 3:
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
    emit = _code(_function_body(source, "motion_v2_emit_frame"))
    if "motion_fps_weight" in emit:
        failures.append("integer_motion_v2_cuda.c: flush re-weights the stored SAD scores")
    if "if (n_frames == 0)" not in _function_body(source, "flush_fex_cuda"):
        failures.append(
            "integer_motion_v2_cuda.c: a one-frame input emits no motion2_v2 / motion3_v2"
        )
    return failures


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
    if kernel.count("ssim_terms(") != 3:
        failures.append(f"{SSIM_KERNEL}: both pass-2 kernels must share ssim_terms()")
    if FLOAT_SSIM_MEAN not in sources["integer_ssim_cuda.c"]:
        failures.append("integer_ssim_cuda.c: the frame mean is not rounded to fp32 like the CPU's")
    if INTEGER_SSIM_FMAD not in _code_meson(sources[MESON]):
        failures.append("core/src/meson.build: integer_ssim_score is compiled with FMA contraction")
    if "*term = w_d * a * b / den;" not in sources[INTEGER_SSIM_KERNEL]:
        failures.append(f"{INTEGER_SSIM_KERNEL}: the SSIM term is not grouped as the CPU groups it")
    return failures


def _guard_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    adm = sources[ADM_KERNEL]
    if "adm_dwt2_source_row(y_out, i, h)" not in adm:
        failures.append(f"{ADM_KERNEL}: scale-0 rows bypass the clamped adm_dwt2_source_row()")
    if adm.count("adm_dwt2_s123_tap(") != 4:
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

    def test_contracted_integer_ssim_is_detected(self) -> None:
        sources = self._edit(MESON, INTEGER_SSIM_FMAD, "'integer_ssim_score' : []")
        self._assert_detected(sources, "FMA contraction")

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

    def test_motion_v2_reweighting_is_detected(self) -> None:
        sources = self._edit(
            "integer_motion_v2_cuda.c",
            "    vmaf_feature_collector_get_score(feature_collector, sad_name, &score_cur, i);\n",
            "    vmaf_feature_collector_get_score(feature_collector, sad_name, &score_cur, i);\n"
            "    score_cur *= s->motion_fps_weight;\n",
        )
        self._assert_detected(sources, "re-weights")

    def test_motion_v2_one_frame_drop_is_detected(self) -> None:
        sources = self._edit("integer_motion_v2_cuda.c", "if (n_frames == 0)", "if (n_frames < 2)")
        self._assert_detected(sources, "one-frame input")

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

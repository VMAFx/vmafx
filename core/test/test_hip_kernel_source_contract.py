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
instead of copies of the math. Every check has a planted-regression case that
reintroduces the old code and must be detected.
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
PICTURE = "picture_hip.c"

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
    )
    sources = {name: (HIP_FEATURE / name).read_text(encoding="utf-8") for name in names}
    sources[PICTURE] = (HIP_RUNTIME / PICTURE).read_text(encoding="utf-8")
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
    return failures


def _motion_v2_failures(motion_v2: str) -> list[str]:
    failures: list[str] = []
    if "MIN(sad_score * s->motion_fps_weight, s->motion_max_val)" not in motion_v2:
        failures.append("integer_motion_v2_hip.c: the stored SAD is not weighted and capped")
    if "motion_fps_weight" in _function_body(motion_v2, "mv2_hip_motion2"):
        failures.append("integer_motion_v2_hip.c: motion2_v2 weights the stored SAD again")
    if "if (n_frames == 0u)" not in _function_body(motion_v2, "flush_fex_hip"):
        failures.append("integer_motion_v2_hip.c: a one-frame run skips motion2_v2 / motion3_v2")
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
    """ADR-1400: frames up to the bound are summed in the CPU's raster order."""
    failures: list[str] = []
    terms = _function_body(
        src[ISSIM_KERNEL].replace("__global__ void\n", "void "),
        "integer_ssim_vert_terms",
    )
    if "partials[idx] = issim_cpu_term(issim_factors(m, samplemax));" not in terms:
        failures.append(f"{ISSIM_KERNEL}: the per-pixel pass does not write the CPU's own term")
    if "const size_t idx = (size_t)y * width + x;" not in terms:
        failures.append(f"{ISSIM_KERNEL}: the per-pixel terms are not stored in raster order")
    host = src[ISSIM_HOST]
    if "s->raster = (size_t)w * h <= ISSIM_HIP_RASTER_MAX_PIXELS;" not in host:
        failures.append(f"{ISSIM_HOST}: small frames no longer select the raster sum")
    if "s->raster ? s->func_vert_terms : s->func_vert;" not in _function_body(
        host, "issim_hip_launch_vert"
    ):
        failures.append(f"{ISSIM_HOST}: the raster path does not launch the per-pixel pass")
    if "for (unsigned i = 0u; i < s->pair_count; i++) {" not in _function_body(
        host, "collect_fex_hip"
    ):
        failures.append(f"{ISSIM_HOST}: collect() no longer adds the pairs in ascending order")
    return failures


def _option_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    psnr = src[PSNR_HOST]
    for helper in (
        "vmaf_psnr_peak(",
        "vmaf_psnr_max(",
        "vmaf_psnr_from_mse(",
        "vmaf_psnr_aggregate(",
    ):
        if helper not in psnr:
            failures.append(f"{PSNR_HOST}: {helper}) of psnr_score.h is not called")
    if "log10(" in psnr:
        failures.append(f"{PSNR_HOST}: a local copy of the PSNR math is back")
    for name in (ISSIM_HOST, FSSIM_HOST):
        if "vmaf_ssim_max_db(" not in src[name] or "s->enable_db, s->max_db" not in src[name]:
            failures.append(f"{name}: enable_db / clip_db do not reach the SSIM emitter")
    if "if (f.lum_num == f.lum_den && f.cs_num == f.cs_den)" not in src[ISSIM_KERNEL]:
        failures.append(f"{ISSIM_KERNEL}: an identical window no longer scores exactly 1")
    failures += _issim_raster_failures(src)
    fssim = _function_body(
        src[FSSIM_KERNEL].replace("__device__ __forceinline__ void", "static void"),
        "ssim_lcs",
    )
    if "#pragma clang fp contract(off)" not in fssim:
        failures.append(f"{FSSIM_KERNEL}: the CPU-typed L / C / S may be contracted")
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
    failures += _float_motion_option_failures(src)
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
    if "p->wg1 != 0u && compute_sad != 0u" not in launch or "fm_hip_launch_scale1(" not in launch:
        failures.append(f"{FMOTION_HOST}: motion_add_scale1 does not run the scale-1 SAD kernel")
    if "c < s->n_planes" not in launch or "s->n_planes = FMH_MAX_PLANES;" not in host:
        failures.append(f"{FMOTION_HOST}: motion_add_uv does not run the chroma planes")
    if HOST_WAIT.search(launch):
        failures.append(f"{FMOTION_HOST}: the frame's kernels wait on the host")
    return failures


def _failures(src: dict[str, str]) -> list[str]:
    return (
        _motion_failures(src)
        + _guard_failures(src)
        + _option_failures(src)
        + _staging_failures(src)
        + _float_ssim_decimation_failures(src)
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
            "    void *prev_luma;",
            "    void *prev_luma;\n    void *blur[2];",
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

    def test_integer_ssim_identical_window_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_KERNEL,
            "    if (f.lum_num == f.lum_den && f.cs_num == f.cs_den)\n        return f.w_d;\n",
            "",
        )
        self.assert_detected(src, "exactly 1")

    def test_integer_ssim_raster_sum_with_tree_term_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_KERNEL,
            "partials[idx] = issim_cpu_term(issim_factors(m, samplemax));",
            "partials[idx] = issim_pixel_term(m, samplemax);",
        )
        self.assert_detected(src, "does not write the CPU's own term")

    def test_integer_ssim_raster_path_dropped_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "s->raster = (size_t)w * h <= ISSIM_HIP_RASTER_MAX_PIXELS;",
            "s->raster = false;",
        )
        self.assert_detected(src, "no longer select the raster sum")

    def test_integer_ssim_raster_kernel_unselected_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "s->raster ? s->func_vert_terms : s->func_vert;",
            "s->func_vert;",
        )
        self.assert_detected(src, "does not launch the per-pixel pass")

    def test_integer_ssim_descending_collect_is_detected(self) -> None:
        src = _replace(
            _sources(),
            ISSIM_HOST,
            "for (unsigned i = 0u; i < s->pair_count; i++) {",
            "for (unsigned i = s->pair_count; i-- > 0u;) {",
        )
        self.assert_detected(src, "ascending order")

    def test_contracted_float_ssim_is_detected(self) -> None:
        src = _sources()
        kernel = src[FSSIM_KERNEL]
        start = kernel.index("__device__ __forceinline__ void ssim_lcs")
        pragma = kernel.index("#pragma clang fp contract(off)\n", start)
        src[FSSIM_KERNEL] = (
            kernel[:pragma] + kernel[pragma + len("#pragma clang fp contract(off)\n") :]
        )
        self.assert_detected(src, "may be contracted")

    def test_forced_identical_float_ssim_is_detected(self) -> None:
        src = _replace(
            _sources(),
            FSSIM_KERNEL,
            "    return lcs[0] * lcs[1] * lcs[2];",
            "    const double num = lcs[0], den = lcs[1] * lcs[2];\n"
            "    return num == den ? 1.0 : num * den;",
        )
        self.assert_detected(src, "not the CPU's l * c * s")

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

    def test_one_frame_motion_v2_skip_is_detected(self) -> None:
        src = _replace(
            _sources(), "integer_motion_v2_hip.c", "if (n_frames == 0u)", "if (n_frames < 2u)"
        )
        self.assert_detected(src, "one-frame run")

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
            "        if (err == 0 && p->wg1 != 0u && compute_sad != 0u)\n"
            "            err = fm_hip_launch_scale1(s, p, pstr);\n",
            "",
        )
        self.assert_detected(src, "motion_add_scale1 does not run the scale-1 SAD kernel")

    def test_luma_only_float_motion_add_uv_is_detected(self) -> None:
        src = _replace(
            _sources(), FMOTION_HOST, "s->n_planes = FMH_MAX_PLANES;", "s->n_planes = 1u;"
        )
        self.assert_detected(src, "motion_add_uv does not run the chroma planes")


if __name__ == "__main__":
    unittest.main()

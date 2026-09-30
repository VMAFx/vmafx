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
FMOTION_HOST = "float_motion_hip.c"
FMOTION_KERNEL = "float_motion/float_motion_score.hip"
PICTURE = "picture_hip.c"

# A host wait or a synchronous copy: none may run between a frame's submit()
# and its collect().
HOST_WAIT = re.compile(
    r"\b(?:hipStreamSynchronize|hipEventSynchronize|hipDeviceSynchronize|hipMemcpy|"
    r"hipMemcpy2D|vmaf_hip_picture_upload|vmaf_hip_kernel_collect_wait)\s*\("
)
MOTION_DIFF_FIRST = re.compile(r"mv2_sample<T>\(prev[^;]*?\)\s*-\s*mv2_sample<T>\(cur", re.S)
TILE_CLAMP = re.compile(r"vmaf_hip_tile_index\(\s*vmaf_hip_reflect_101\(")
VERTICAL_ROUND = re.compile(r"\(\s*sum\s*\+\s*round_y\s*\)\s*>>\s*shift_y")
HORIZONTAL_ROUND = re.compile(r"\(\s*blurred\s*\+\s*\(\(int64_t\)1 << 15\)\s*\)\s*>>\s*16")
# The motion tile loader clamps both axes: x and y.
TILE_AXES = 2
# float_motion: both axes in each of the 8- and 16-bpc kernels.
FM_TILE_LOADS = 4


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


def _staging_failures(src: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in MOTION_TUS:
        text = src[name]
        if ".staging_bytes = s->plane_bytes" not in _function_body(text, MOTION_LAUNCH_FNS[name]):
            failures.append(f"{name}: the staged upload no longer gets the allocated size")
        if re.search(r"s->frame_[wh]\s*=", _function_body(text, "submit_fex_hip")):
            failures.append(f"{name}: submit() rewrites the geometry init() sized the buffers for")
    submit = _function_body(src[MOTION_SAD], "vmaf_hip_motion_sad_submit")
    staged = _function_body(src[PICTURE], "vmaf_hip_picture_upload_staged")
    if "frame->staging_bytes" not in submit:
        failures.append(f"{MOTION_SAD}: the staging bound is derived from the frame, not the owner")
    if "motion_sad_drain_after_error(" not in submit or "hip_pic_drain_after_error(" not in staged:
        failures.append("a failed enqueue returns while earlier copies still use the staging")
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
        if "vmaf_hip_kernel_collect_wait" not in _function_body(text, "collect_fex_hip"):
            failures.append(f"{name}: collect() no longer holds the frame's one wait")
    submit = _function_body(src[MOTION_SAD], "vmaf_hip_motion_sad_submit")
    if not submit or HOST_WAIT.search(submit) or "vmaf_hip_picture_upload_staged" not in submit:
        failures.append(f"{MOTION_SAD}: the frame upload waits on the host or skips the staging")
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
    if "if (lum_num == lum_den && cs_num == cs_den)" not in src[ISSIM_KERNEL]:
        failures.append(f"{ISSIM_KERNEL}: an identical window no longer scores exactly 1")
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
    return failures


def _failures(src: dict[str, str]) -> list[str]:
    return (
        _motion_failures(src)
        + _guard_failures(src)
        + _option_failures(src)
        + _staging_failures(src)
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
            "    void *pix[2];",
            "    void *pix[2];\n    void *blur[2];",
        )
        self.assert_detected(src, "blurred-frame ping-pong")

    def test_waiting_upload_in_motion_submit_is_detected(self) -> None:
        src = _replace(
            _sources(),
            "integer_motion_hip.c",
            "    const int err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);",
            "    (void)vmaf_hip_picture_upload(NULL, 0u, s->lc.str);\n"
            "    const int err = vmaf_hip_motion_sad_submit(&s->sad_kernel, &frame, s->lc.str);",
        )
        self.assert_detected(src, "without a wait")

    def test_stream_sync_in_sad_pipeline_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_SAD,
            "    if (err != 0 || frame->prev == NULL)",
            "    (void)hipStreamSynchronize(NULL);\n    if (err != 0 || frame->prev == NULL)",
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
            "    if (lum_num == lum_den && cs_num == cs_den)\n        return w_d;\n",
            "",
        )
        self.assert_detected(src, "exactly 1")

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

    def test_frame_derived_staging_bound_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_SAD,
            "frame->staging, frame->staging_bytes, stream);",
            "frame->staging,\n"
            "        vmaf_hip_motion_sad_plane_bytes(frame->width, frame->height, frame->bpc), stream);",
        )
        self.assert_detected(src, "derived from the frame")

    def test_drain_on_the_success_path_is_detected(self) -> None:
        src = _replace(
            _sources(),
            MOTION_SAD,
            "        return motion_sad_drain_after_error(stream, launch_err);\n    return 0;",
            "        return launch_err;\n    (void)motion_sad_drain_after_error(stream, 0);\n"
            "    return 0;",
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


if __name__ == "__main__":
    unittest.main()

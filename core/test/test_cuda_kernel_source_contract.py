#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CUDA RC3 parity design at source level (ADR-1372, ADR-1373, ADR-1374).

Device-free, so it runs on every host. Each contract has a planted-regression
case that edits the live source the way the old code read and must fail:

- motion: both twins run the one diff-first SAD kernel through
  integer_motion_sad_cuda.c; the kernel stages prev - cur before the blur; no
  submit path waits on the host, the previous frame is ordered by an event,
  and motion_cuda reads its SAD slots back with one synchronisation;
- PSNR / SSIM / float motion options: the host arithmetic is the CPU's shared
  helpers (psnr_score.h, nonfinite_score.h, motion_clip), not a copy;
- float SSIM: the combine rounds its three products with __fmul_rn and scores
  numerator == denominator as exactly 1; the integer SSIM combine compiles
  with --fmad=false, so it evaluates the CPU expression operand for operand;
- ADM: the DWT kernels read their rows and taps through adm_dwt2_rows.h;
- VIF: vif_cuda declares its minimum size through the ADR-1324 gate.
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
INTEGER_SSIM_FMAD = "'integer_ssim_score' : vmaf_cuda_host_strict_fp_args + ['--fmad=false']"
SOURCES = (
    MOTION_KERNEL,
    MOTION_SAD,
    *MOTION_TUS,
    SSIM_KERNEL,
    ADM_KERNEL,
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
    if '"enable_chroma"' in sources["integer_ssim_cuda.c"]:
        failures.append("integer_ssim_cuda.c: float_ssim_cuda declares enable_chroma again")
    motion = sources["float_motion_cuda.c"]
    if motion.count("motion_clip(s, ") != 3:
        failures.append(
            "float_motion_cuda.c: motion2, debug motion and tail motion2 must all "
            "be motion_clip()ped"
        )
    return failures


def _ssim_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    kernel = sources[SSIM_KERNEL]
    for product in (
        "__fmul_rn(m.ref_mu, m.ref_mu)",
        "__fmul_rn(m.cmp_mu, m.cmp_mu)",
        "__fmul_rn(m.ref_mu, m.cmp_mu)",
    ):
        if product not in kernel:
            failures.append(f"{SSIM_KERNEL}: {product} can be contracted into an FMA")
    if "(num == den) ? 1.0f : num / den" not in kernel:
        failures.append(f"{SSIM_KERNEL}: identical windows no longer score exactly 1")
    if kernel.count("ssim_from_moments(") != 3:
        failures.append(f"{SSIM_KERNEL}: both pass-2 kernels must share ssim_from_moments()")
    if INTEGER_SSIM_FMAD not in _code_meson(sources[MESON]):
        failures.append("core/src/meson.build: integer_ssim_score is compiled with FMA contraction")
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


def _all_failures(sources: dict[str, str]) -> list[str]:
    return [
        *_motion_failures(sources),
        *_option_failures(sources),
        *_ssim_failures(sources),
        *_guard_failures(sources),
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

    def test_phantom_enable_chroma_is_detected(self) -> None:
        sources = _sources()
        sources["integer_ssim_cuda.c"] += '\n{.name = "enable_chroma"},\n'
        self._assert_detected(sources, "enable_chroma again")

    def test_contracted_ssim_product_is_detected(self) -> None:
        sources = self._edit(SSIM_KERNEL, "__fmul_rn(m.ref_mu, m.ref_mu)", "m.ref_mu * m.ref_mu")
        self._assert_detected(sources, "contracted into an FMA")

    def test_contracted_integer_ssim_is_detected(self) -> None:
        sources = self._edit(MESON, INTEGER_SSIM_FMAD, "'integer_ssim_score' : []")
        self._assert_detected(sources, "FMA contraction")

    def test_missing_exact_one_is_detected(self) -> None:
        sources = self._edit(SSIM_KERNEL, "(num == den) ? 1.0f : num / den", "num / den")
        self._assert_detected(sources, "exactly 1")

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


if __name__ == "__main__":
    unittest.main()

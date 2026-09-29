#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protect fp64-free SpEED kernels and explicit SYCL output captures."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYCL_ROOT = ROOT / "core" / "src" / "feature" / "sycl"

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


def _sources() -> dict[str, str]:
    names = (
        SPEED_PIPELINE,
        *SPEED_HOST_TUS,
        "float_psnr_sycl.cpp",
        "integer_psnr_sycl.cpp",
        "integer_moment_sycl.cpp",
        "integer_ms_ssim_sycl.cpp",
    )
    return {name: (SYCL_ROOT / name).read_text(encoding="utf-8") for name in names}


def _speed_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if re.search(r"\bdouble\b", sources[SPEED_PIPELINE]):
        failures.append(f"{SPEED_PIPELINE}: fp64 type appears in the device pipeline")
    for name in (SPEED_PIPELINE, *SPEED_HOST_TUS):
        for helper in SPEED_HOST_RESIDUAL:
            if helper.search(sources[name]):
                failures.append(f"{name}: host residual {helper.pattern} reintroduced")
    for name in SPEED_HOST_TUS:
        if re.search(r"\b(?:parallel_for|single_task)\b", sources[name]):
            failures.append(f"{name}: device kernel outside {SPEED_PIPELINE}")
        if re.search(r"\.wait(?:_and_throw)?\(", sources[name]):
            failures.append(f"{name}: host wait outside the pipeline collect")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures = _speed_failures(sources)

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


def _function_body(source: str, name: str) -> str:
    match = re.search(rf"^static [^\n]*\b{name}\(.*?^}}$", source, re.S | re.M)
    return match.group(0) if match else ""


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


class SyclKernelSourceContractTest(unittest.TestCase):
    def test_live_sources_keep_fp32_and_capture_contracts(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp64_speed_regression_is_detected(self) -> None:
        sources = _sources()
        sources[SPEED_PIPELINE] = sources[SPEED_PIPELINE].replace(
            "struct Ff {", "struct Wide { double sum; };\nstruct Ff {", 1
        )
        self.assertTrue(any("fp64 type" in item for item in _contract_failures(sources)))

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


if __name__ == "__main__":
    unittest.main()

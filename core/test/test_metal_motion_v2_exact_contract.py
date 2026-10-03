#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin motion_v2_metal to the CPU `motion_v2`'s stored score, window and options.

Item (3) of T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30, ADR-1498:
integer_motion_v2.c stores MIN(SAD / 256 / (w * h) * motion_fps_weight,
motion_max_val) per frame (0 below min_idx) and derives motion2_v2 and
motion3_v2 of every frame from those stored values with
vmaf_motion_window_flush() (motion_window.h, ADR-1478). motion_v2_metal stored
the raw SAD, weighted it again in a flush of its own without the
motion_max_val cap on motion2_v2, blended the unweighted SAD into the
motion3_v2 stamp, returned from flush() without output for one frame, added
its block sums in double, and lacked the CPU options motion_force_zero and
motion_five_frame_window.

The port follows motion_v2_hip / motion_v2_sycl / motion_v2_cuda (#1636,
#1645, ADR-1491): collect() stores the CPU's value from a uint64 SAD, flush()
is vmaf_motion_window_flush(), the five-frame window takes the SAD against
frame n - 2 (the twin keeps the luma of the last `depth` frames),
motion_force_zero stores 0 and runs no kernel, and the option table is the
CPU's. The kernel's SAD stays an exact integer per threadgroup.

Device-free: reads the sources only. test_metal_motion_v2_parity compares the
values on a device (every output of every frame at ==).
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metal_option_tables as tables

FEATURE = tables.FEATURE
KERNEL = "metal/integer_motion_v2.metal"
HOST = "metal/integer_motion_v2_metal.mm"
REFERENCE = "integer_motion_v2.c"

STORED_SCORE = (
    "const double score = (double)sad / 256. / (s->frame_w * s->frame_h);",
    "const double weighted = score * s->motion_fps_weight;",
    "(weighted < s->motion_max_val) ? weighted : s->motion_max_val, index);",
)
SAD_SUM = (
    "const uint32_t *partials = (const uint32_t *)s->rb.host_view;",
    "uint64_t sum = 0U;",
    "sum += partials[i];",
)
WINDOW = (
    '#include "motion_window.h"',
    ".motion_blend_factor = s->motion_blend_factor,",
    ".motion_blend_offset = s->motion_blend_offset,",
    ".motion_max_val = s->motion_max_val,",
    ".motion_five_frame_window = s->motion_five_frame_window,",
    ".motion_moving_average = s->motion_moving_average,",
    "const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
    ".flush = flush_fex_metal,",
)
FIVE_FRAME = (
    "s->depth = s->motion_five_frame_window ? 2U : 1U;",
    "id<MTLBuffer> slot = (__bridge id<MTLBuffer>)s->prev_luma[index % s->depth];",
    "if (index >= s->depth) {",
    "copy_y_plane(ref_pic, [slot contents], row_bytes);",
)
FORCE_ZERO = (
    "if (s->motion_force_zero) { return 0; }",
    "if (s->motion_force_zero || index < s->depth) {",
)
# A flush of the twin's own: its blend, an early return, a motion2 append.
OWN_WINDOW = re.compile(r"\bmotion_blend\s*\(|n_frames\s*<\s*2|\blast_score\b")
FLOAT_SAD = re.compile(r"\bdouble\s+sad\w*\s*=|\+=\s*\(double\)")
KERNEL_SUM = (
    "uint abs_h = 0;",
    "const uint lane_sum = simd_sum(abs_h);",
    "uint group_sum = 0;",
    "group_sum += simd_partials[i];",
    "partials[bid.y * grid_groups.x + bid.x] = group_sum;",
)
KERNELS = 2
KERNEL_FLOAT = re.compile(r"\b(?:float|half|double)\b|\batomic\w*")
REFERENCE_LINES = (
    "double score = (double)sad / 256. / (w * h);",
    "MIN(score * s->motion_fps_weight, s->motion_max_val), index);",
    "const unsigned min_idx = s->motion_five_frame_window ? 2 : 1;",
    "const VmafPicture *prev = s->motion_five_frame_window ? &fex->prev_prev_ref : &fex->prev_ref;",
    "if (s->motion_force_zero) {",
    "const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(tables.strip_comments(source).split())


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8") for name in (KERNEL, HOST, REFERENCE)
    }


def _missing(code: str, pieces: tuple[str, ...]) -> list[str]:
    return [piece for piece in pieces if " ".join(piece.split()) not in code]


def _table_failures(sources: dict[str, str]) -> list[str]:
    cpu = tables.table_of_text(sources[REFERENCE], FEATURE / REFERENCE)
    twin = tables.table_of_text(sources[HOST], FEATURE / HOST)
    return [f"{HOST}: option table: {item}" for item in tables.differences(cpu, twin)]


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures: list[str] = []
    if _missing(code, STORED_SCORE):
        failures.append(f"{HOST}: the stored SAD score is not the CPU's weighted, capped value")
    if _missing(code, SAD_SUM) or FLOAT_SAD.search(code):
        failures.append(f"{HOST}: the SAD is not the uint64 sum of the group sums")
    if _missing(code, WINDOW) or OWN_WINDOW.search(code):
        failures.append(f"{HOST}: motion2_v2 / motion3_v2 do not come from the CPU's window")
    if _missing(code, FIVE_FRAME):
        failures.append(f"{HOST}: the five-frame window does not read frame n - 2")
    if _missing(code, FORCE_ZERO):
        failures.append(f"{HOST}: motion_force_zero does not store 0")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    # Both kernels (8 and 16 bpc) carry the reduction.
    if any(code.count(" ".join(piece.split())) != KERNELS for piece in KERNEL_SUM):
        failures.append(f"{KERNEL}: a group SAD is not an exact integer sum")
    if KERNEL_FLOAT.search(code):
        failures.append(f"{KERNEL}: a floating-point value or an atomic is in the SAD")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [f"{REFERENCE} no longer holds `{p}`" for p in _missing(code, REFERENCE_LINES)]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _table_failures(sources)
        + _host_failures(sources[HOST])
        + _kernel_failures(sources[KERNEL])
        + _reference_failures(sources[REFERENCE])
    )


class MetalMotionV2ExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_missing_cpu_option_is_detected(self) -> None:
        # The pre-port table had neither motion_force_zero nor the window.
        failures = self._edited(
            HOST, '.name = "motion_force_zero",', '.name = "motion_force_zero_x",'
        )
        self._assert_detected(failures, "the twin lacks motion_force_zero")

    def test_option_flag_drift_is_detected(self) -> None:
        old = (
            '.name = "motion_five_frame_window",\n'
            '        .help = "use five-frame temporal window",\n'
            '        .alias = "mffw",'
        )
        failures = self._edited(HOST, old, old.replace('"mffw"', '"m5fw"'))
        self._assert_detected(failures, "motion_five_frame_window: cpu")

    def test_raw_sad_store_is_detected(self) -> None:
        # The pre-port collect stored the unweighted, uncapped SAD.
        failures = self._edited(
            HOST,
            "(weighted < s->motion_max_val) ? weighted : s->motion_max_val, index);",
            "score, index);",
        )
        self._assert_detected(failures, "weighted, capped value")

    def test_own_flush_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    if (s->feature_name_dict == NULL) { return 1; }",
            "    if (s->feature_name_dict == NULL || n_frames < 2) { return 1; }",
        )
        self._assert_detected(failures, "do not come from the CPU's window")

    def test_double_block_sum_is_detected(self) -> None:
        failures = self._edited(HOST, "    uint64_t sum = 0U;", "    double sad_d = 0.0;")
        self._assert_detected(failures, "not the uint64 sum")

    def test_three_frame_only_is_detected(self) -> None:
        failures = self._edited(
            HOST, "s->depth = s->motion_five_frame_window ? 2U : 1U;", "s->depth = 1U;"
        )
        self._assert_detected(failures, "does not read frame n - 2")

    def test_force_zero_ignored_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    if (s->motion_force_zero || index < s->depth) {",
            "    if (index < s->depth) {",
        )
        self._assert_detected(failures, "motion_force_zero does not store 0")

    def test_float_kernel_sum_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "        uint group_sum = 0;", "        float group_sum = 0;"
        )
        self._assert_detected(failures, "not an exact integer sum")
        self._assert_detected(failures, "floating-point value")

    def test_cpu_reference_drift_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "MIN(score * s->motion_fps_weight, s->motion_max_val), index);",
            "score, index);",
        )
        self._assert_detected(failures, f"{REFERENCE} no longer holds")


if __name__ == "__main__":
    unittest.main()

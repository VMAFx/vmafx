#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""float_motion_metal computes in the CPU's order (ADR-1498, ADR-1409, ADR-1419).

The CPU float_motion extractor adds the absolute differences of one row, left
to right, into one fp32 accumulator, adds the row sums top to bottom into
another and divides in fp32; every step rounds, so a per-block sum is up to
1.4e-4 off at 1920x1080. The Metal twin used such a sum (simd_sum per SIMD
group, the groups per threadgroup, the threadgroups in double on the host).
Its port takes float_motion_hip's design: the blur stores |cur - prev|
transposed, one thread per row adds the row in order, and the host finishes
through vmaf_float_motion_score_from_row_sads(); the blur, the scale-1 term
and every per-sample value go through core/src/feature/metal/
metal_float_motion_math.h, which test_metal_float_motion_math holds against
the CPU's functions on the host.

Device-free: this reads the sources. Each planted case below puts back one
construct the port removed and checks that the contract names it.
"""

from __future__ import annotations

import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"
MATH = "metal/metal_float_motion_math.h"
KERNEL = "metal/float_motion.metal"
HOST = "metal/float_motion_metal.mm"
CPU_TAPS = "motion_tools.h"
CPU = "float_motion.c"
HOST_ONLY = "#if !defined(__METAL_VERSION__)"

# The arithmetic the header must spell, each the CPU's operation.
MATH_PIECES = (
    "const float scaled = (float)raw * inv_scaler;",
    "return scaled + -128.0f;",
    "const vmaf_mtl_i32 period = 2 * (size - 1);",
    "return (m < size) ? m : period - m;",
    "const float product = t.w[r] * win.v[r * VMAF_MTL_FM_TAPS + c];",
    "accum += product;",
    "const float product = t.w[c] * col[c];",
    "blurred += product;",
    "return ((group * width) + x) * VMAF_MTL_FM_ROW_GROUP + lane;",
    "return (diff < 0.0f) ? -diff : diff;",
    "const float scaled = centre * ratio;",
    "return sum123 + t22;",
    "float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);",
    "score += (float)vmaf_float_motion_score_from_row_sads(",
    "return (double)score;",
)

# What the kernels must call: every value through the header, the row sum in
# the CPU's order from the transposed plane.
KERNEL_PIECES = (
    '#include "metal_float_motion_math.h"',
    "vmaf_mtl_fm_blur(vmaf_mtl_fm_taps(args.filter_size), win)",
    "diff[vmaf_mtl_fm_diff_index(gid.x, gid.y, args.width)] = "
    "vmaf_mtl_fm_abs_diff(blurred, prev_blur[off]);",
    "diff[vmaf_mtl_fm_diff_index(gid.x, gid.y, args.scaled_width)] = "
    "vmaf_mtl_fm_abs_diff(cur, prev);",
    "float accum = 0.0f;",
    "accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];",
    "row_sad[args.first_row + y] = accum;",
)
# The tile load folds both axes into the plane.
KERNEL_TILE_FOLDS = 2
# A reduction across threads, a double, a fused operation, fast math or a tap
# table of the kernel's own.
KERNEL_BANNED = re.compile(
    r"simd_sum|simd_shuffle|quad_sum|atomic|sad_parts|\bdouble\b|\bfma\s*\(|fast::|0\.0544886"
)

HOST_PIECES = (
    '#include "metal_float_motion_math.h"',
    "(vmaf_mtl_u32)s->motion_filter_size",
    "threadsPerThreadgroup:MTLSizeMake(VMAF_MTL_FM_ROW_GROUP, 1, 1)",
    "s->n_planes = FMM_MAX_PLANES;",
    "for (unsigned c = 0u; c < s->n_planes && err == 0; c++) {",
)
# CPU options the twin does not take yet: none, its table is the CPU's.
OPTIONS_NOT_YET: tuple[str, ...] = ()
# motion3 from collect (frame 0 and index - 1) and from the flush tail.
MOTION3_BLEND_EMITS = 2
MOTION3_BLEND = (
    "MIN(motion_blend(score * s->motion_fps_weight, s->motion_blend_factor, "
    "s->motion_blend_offset), s->motion_max_val)"
)
# CPU float_motion.c::motion_clip(), and the debug score through it.
MOTION_CLIP = "return MIN(score * s->motion_fps_weight, s->motion_max_val);"
DEBUG_SCORE = (
    "const double debug_score = (index > 0u) ? fm_metal_motion_clip(s, motion_score) : 0.0;"
)
FRAME_SCORE = (
    "score += vmaf_mtl_fm_plane_score(rows + p->off0, p->w, p->h, scale1_rows, p->sw, p->sh);"
)


def sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8")
        for name in (MATH, KERNEL, HOST, CPU_TAPS, CPU)
    }


def code(source: str) -> str:
    """C / MSL / Objective-C++ source without comments, whitespace squeezed."""
    return " ".join(re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S).split())


def function_body(source: str, name: str) -> str:
    """The body of the first definition of `name` (to its closing brace at column 0)."""
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{]*\)\s*\{{(.*?)\n\}}", source, flags=re.S)
    return match.group(1) if match else ""


def float32_literals(text: str) -> list[int]:
    """The bit patterns of the decimal literals in `text`, rounded to float32."""
    numbers = re.findall(r"\d+\.\d+", text)
    return [struct.unpack("<I", struct.pack("<f", float(n)))[0] for n in numbers]


def tap_failures(src: dict[str, str]) -> list[str]:
    """The header's taps round to motion_tools.h's floats (5-tap, 3-tap)."""
    failures = []
    for cpu_name, mtl_name in (
        ("FILTER_5_s", "vmaf_mtl_fm_filter5"),
        ("FILTER_3_s", "vmaf_mtl_fm_filter3"),
    ):
        cpu = re.search(rf"{cpu_name}\[\d\] = \{{([^}}]*)\}}", src[CPU_TAPS])
        mtl = re.search(rf"{mtl_name}\[VMAF_MTL_FM_TAPS\] = \{{([^}}]*)\}}", src[MATH])
        if not cpu or not mtl:
            failures.append(f"{MATH}: {mtl_name} or {cpu_name} not found")
            continue
        want = float32_literals(cpu.group(1))
        got = [bits for bits in float32_literals(mtl.group(1)) if bits != 0]
        if got != want:
            failures.append(f"{MATH}: {mtl_name} does not round to {cpu_name}'s floats")
    return failures


def math_failures(src: dict[str, str]) -> list[str]:
    text = code(src[MATH])
    failures = [
        f"{MATH}: no longer the CPU's arithmetic ({p})" for p in MATH_PIECES if p not in text
    ]
    shared = code(src[MATH].split(HOST_ONLY, 1)[0])
    if re.search(r"\bdouble\b", shared):
        failures.append(f"{MATH}: a double in the part the kernels compile")
    if re.search(r"FMA|\bfma\s*\(", text):
        failures.append(f"{MATH}: a fused multiply-add where the CPU rounds twice")
    return failures + tap_failures(src)


def kernel_failures(src: dict[str, str]) -> list[str]:
    text = code(src[KERNEL])
    failures = [
        f"{KERNEL}: a kernel no longer goes through {MATH} ({p})"
        for p in KERNEL_PIECES
        if p not in text
    ]
    if text.count("vmaf_mtl_fm_reflect101(") != KERNEL_TILE_FOLDS:
        failures.append(f"{KERNEL}: a tile load does not fold its index into the plane")
    if KERNEL_BANNED.search(text):
        failures.append(f"{KERNEL}: a kernel reduces across threads or leaves the CPU's arithmetic")
    return failures


def host_failures(src: dict[str, str]) -> list[str]:
    text = code(src[HOST])
    failures = [f"{HOST}: the port is incomplete ({p})" for p in HOST_PIECES if p not in text]
    score = code(function_body(src[HOST], "fm_metal_frame_score"))
    if FRAME_SCORE not in score:
        failures.append(f"{HOST}: the frame score is not built from the CPU's row sums")
    if re.search(r"\+= \(double\)|double sad_sum", score):
        failures.append(f"{HOST}: the row sums are added in double")
    plane = code(function_body(src[HOST], "fm_metal_encode_plane"))
    if (
        "fm_metal_encode_scale1(s, cmd, p)" not in plane
        or "p->diff[1], p->off1, p->sw, p->sh" not in plane
    ):
        failures.append(f"{HOST}: motion_add_scale1 does not run the scale-1 SAD")
    return failures


def option_names(source: str) -> list[str]:
    """The names of an extractor's option table, in its order."""
    block = re.search(r"static const VmafOption options\[\] = \{(.*?)\{\s*0\s*\}", source, re.S)
    return re.findall(r'\.name\s*=\s*"(\w+)"', block.group(1)) if block else []


def option_failures(src: dict[str, str]) -> list[str]:
    """The twin's table: the CPU's names in the CPU's order (it spells the feature names)."""
    want = [name for name in option_names(src[CPU]) if name not in OPTIONS_NOT_YET]
    if option_names(src[HOST]) != want:
        return [f"{HOST}: the option table is not the CPU's names in the CPU's order"]
    return []


def motion3_failures(src: dict[str, str]) -> list[str]:
    """ADR-1404: motion3 at the CPU's indices, blended through motion_blend_tools.h."""
    host = src[HOST]
    provided = re.search(r"provided_features\[\] = \{([^}]*)\}", host)
    checks = (
        (provided and '"VMAF_feature_motion3_score"' in provided.group(1), "provides no motion3"),
        (
            MOTION3_BLEND in code(function_body(host, "fm_metal_motion_blend_clip")),
            "motion3 does not blend through motion_blend_tools.h",
        ),
        (
            len(re.findall(r'"VMAF_feature_motion3_score",\s*fm_metal_motion_blend_clip\(', host))
            == MOTION3_BLEND_EMITS,
            "a motion3 score skips motion_blend_clip",
        ),
        (
            "fm_metal_append(s, feature_collector, feature_name, 0.0, 0u)"
            in code(function_body(host, "flush_fex_metal"))
            and 'feature_name[] = "VMAF_feature_motion3_score";' in host,
            "a one-frame run emits no motion3",
        ),
        (
            '"VMAF_feature_motion3_score", 0.0, index'
            in code(function_body(host, "extract_force_zero_metal")),
            "motion_force_zero emits no motion3",
        ),
        (
            "(index > 1u && s->prev_motion_score < motion_score)"
            in code(function_body(host, "fm_metal_emit_motion23")),
            "motion3 at frame 0 does not come from the first SAD",
        ),
    )
    return [f"{HOST}: {message}" for ok, message in checks if not ok]


def clip_failures(src: dict[str, str]) -> list[str]:
    """ADR-1382 / BUG-048: motion and motion2 through motion_clip(), the cap included."""
    host = src[HOST]
    failures = []
    if MOTION_CLIP not in code(function_body(host, "fm_metal_motion_clip")):
        failures.append(f"{HOST}: motion / motion2 skip the motion_max_val cap")
    collect = code(function_body(host, "collect_fex_metal"))
    if DEBUG_SCORE not in collect or '"VMAF_feature_motion_score", debug_score' not in collect:
        failures.append(f"{HOST}: the debug motion score skips motion_clip")
    return failures


def contract_failures(src: dict[str, str]) -> list[str]:
    host = host_failures(src) + option_failures(src) + motion3_failures(src) + clip_failures(src)
    return math_failures(src) + kernel_failures(src) + host


def planted(name: str, old: str, new: str) -> dict[str, str]:
    """The sources with one construct put back; `old` must be there."""
    src = sources()
    if old not in src[name]:
        raise AssertionError(f"{name}: the planted case's anchor is gone: {old!r}")
    src[name] = src[name].replace(old, new, 1)
    return src


class MetalFloatMotionExactContract(unittest.TestCase):
    def assert_detected(self, src: dict[str, str], fragment: str) -> None:
        failures = contract_failures(src)
        self.assertTrue(any(fragment in f for f in failures), failures)

    def test_port_holds(self) -> None:
        self.assertEqual(contract_failures(sources()), [])

    def test_block_reduced_row_sum_is_detected(self) -> None:
        src = planted(
            KERNEL,
            "        accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];",
            "        accum = simd_sum(diff[base + j]);",
        )
        self.assert_detected(src, "accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];")
        self.assert_detected(src, "reduces across threads")

    def test_untransposed_row_sum_is_detected(self) -> None:
        src = planted(
            KERNEL, "accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];", "accum += diff[base + j];"
        )
        self.assert_detected(src, "accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];")

    def test_per_threadgroup_partials_are_detected(self) -> None:
        src = planted(KERNEL, "row_sad[args.first_row + y] = accum;", "sad_parts[y] = accum;")
        self.assert_detected(src, "reduces across threads")

    def test_host_double_sum_is_detected(self) -> None:
        src = planted(
            HOST,
            "score += vmaf_mtl_fm_plane_score(rows + p->off0, p->w, p->h, scale1_rows, "
            "p->sw, p->sh);",
            "score += (double)rows[p->off0];",
        )
        self.assert_detected(src, "not built from the CPU's row sums")
        self.assert_detected(src, "added in double")

    def test_double_plane_mean_is_detected(self) -> None:
        src = planted(
            MATH,
            "    float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);",
            "    double score = vmaf_float_motion_score_from_row_sads(rows, width, height);",
        )
        self.assert_detected(src, "float score = (float)vmaf_float_motion_score_from_row_sads")

    def test_fused_blur_tap_is_detected(self) -> None:
        src = planted(
            MATH,
            "            const float product = t.w[r] * win.v[r * VMAF_MTL_FM_TAPS + c];\n"
            "            accum += product;",
            "            accum = VMAF_MTL_FMA(t.w[r], win.v[r * VMAF_MTL_FM_TAPS + c], accum);",
        )
        self.assert_detected(src, "fused multiply-add")

    def test_fixed_filter_is_detected(self) -> None:
        src = planted(KERNEL, "vmaf_mtl_fm_taps(args.filter_size)", "vmaf_mtl_fm_taps(5u)")
        self.assert_detected(src, "vmaf_mtl_fm_taps(args.filter_size)")

    def test_drifted_tap_literal_is_detected(self) -> None:
        src = planted(
            MATH,
            "0.054488685f, 0.244201342f, 0.402619947f",
            "0.0544887f, 0.244201342f, 0.402619947f",
        )
        self.assert_detected(src, "does not round to FILTER_5_s's floats")

    def test_unfolded_tile_load_is_detected(self) -> None:
        src = planted(
            KERNEL,
            "vmaf_mtl_fm_reflect101(tile_ox + (int)(i % VMAF_MTL_FM_TILE), width)",
            "clamp(tile_ox + (int)(i % VMAF_MTL_FM_TILE), 0, width - 1)",
        )
        self.assert_detected(src, "does not fold its index")

    def test_skipped_scale1_kernel_is_detected(self) -> None:
        src = planted(HOST, "    err = fm_metal_encode_scale1(s, cmd, p);\n", "")
        self.assert_detected(src, "does not run the scale-1 SAD")

    def test_double_in_kernel_part_is_detected(self) -> None:
        src = planted(
            MATH,
            "    const float scaled = (float)raw * inv_scaler;",
            "    const double scaled = (double)raw * inv_scaler;",
        )
        self.assert_detected(src, "a double in the part the kernels compile")

    def test_kernel_tap_table_is_detected(self) -> None:
        src = planted(
            KERNEL,
            "using namespace metal;",
            "using namespace metal;\nconstant float FILT[1] = {0.054488685f};",
        )
        self.assert_detected(src, "leaves the CPU's arithmetic")

    def test_unblended_motion3_is_detected(self) -> None:
        src = planted(
            HOST,
            "fm_metal_motion_blend_clip(s, motion2), index - 1u);",
            "fm_metal_motion_clip(s, motion2), index - 1u);",
        )
        self.assert_detected(src, "a motion3 score skips motion_blend_clip")

    def test_local_blend_is_detected(self) -> None:
        src = planted(
            HOST, "return MIN(motion_blend(score * s->motion_fps_weight,", "return MIN((score *"
        )
        self.assert_detected(src, "does not blend through motion_blend_tools.h")

    def test_missing_one_frame_motion3_is_detected(self) -> None:
        src = planted(
            HOST,
            "fm_metal_append(s, feature_collector, feature_name, 0.0, 0u)",
            "fm_metal_append(s, feature_collector, \"VMAF_feature_motion2_score\", 0.0, 0u)",
        )
        self.assert_detected(src, "a one-frame run emits no motion3")

    def test_force_zero_without_motion3_is_detected(self) -> None:
        src = planted(
            HOST,
            '"VMAF_feature_motion3_score", 0.0, index);',
            '"VMAF_feature_motion2_score", 0.0, index);',
        )
        self.assert_detected(src, "motion_force_zero emits no motion3")

    def test_unprovided_motion3_is_detected(self) -> None:
        src = planted(
            HOST,
            '"VMAF_feature_motion2_score", "VMAF_feature_motion3_score", NULL',
            '"VMAF_feature_motion2_score", NULL',
        )
        self.assert_detected(src, "provides no motion3")

    def test_reordered_option_table_is_detected(self) -> None:
        src = planted(
            HOST, '.name        = "motion_blend_offset",', '.name        = "motion_blend_offsets",'
        )
        self.assert_detected(src, "the option table is not the CPU's names")

    def test_motion3_at_frame_zero_from_min_is_detected(self) -> None:
        src = planted(
            HOST,
            "(index > 1u && s->prev_motion_score < motion_score)",
            "(s->prev_motion_score < motion_score)",
        )
        self.assert_detected(src, "motion3 at frame 0 does not come from the first SAD")

    def test_unclipped_debug_score_is_detected(self) -> None:
        src = planted(
            HOST,
            '"VMAF_feature_motion_score", debug_score,',
            '"VMAF_feature_motion_score", motion_score,',
        )
        self.assert_detected(src, "the debug motion score skips motion_clip")

    def test_uncapped_motion2_is_detected(self) -> None:
        src = planted(
            HOST,
            "return MIN(score * s->motion_fps_weight, s->motion_max_val);",
            "return score * s->motion_fps_weight;",
        )
        self.assert_detected(src, "skip the motion_max_val cap")

    def test_uncapped_motion3_is_detected(self) -> None:
        src = planted(HOST, "               s->motion_max_val);", "               1e300);")
        self.assert_detected(src, "motion3 does not blend through motion_blend_tools.h")

    def test_missing_max_val_option_is_detected(self) -> None:
        src = planted(HOST, '.name        = "motion_max_val",', '.name        = "motion_cap",')
        self.assert_detected(src, "the option table is not the CPU's names")


if __name__ == "__main__":
    unittest.main()

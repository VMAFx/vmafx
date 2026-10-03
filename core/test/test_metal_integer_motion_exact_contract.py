#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin integer_motion_metal to the CPU `motion` extractor's order, scores and table (ADR-1498).

``integer_motion.c`` blurs the frame difference (Netflix a4a1492d): the 5-tap
filter down the column of prev - cur, rounded by bpc, then along the row,
rounded by 16, and the absolute values summed. ``integer_motion_metal`` blurred
each frame and differenced the blurred frames
(T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29), lacked the CPU's option table, and
emitted ``VMAF_integer_feature_motion_y_score`` and its own motion2 instead of
the CPU's SAD score, motion2 and motion3
(T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02).

The kernel now differences two raw planes first (``metal_integer_motion_math.h``,
the design of ADR-1371 / ADR-1372), the host keeps a ring of raw planes (three
with ``motion_five_frame_window``, ADR-1491), ``collect()`` appends the CPU's
SAD score on every frame and ``flush()`` calls the CPU's
``vmaf_motion_window_flush()`` (ADR-1478). The option table and the provided
features are the CPU's.

Device-free: reads the sources only. ``test_metal_integer_motion_math`` runs the
arithmetic against the CPU on the host; ``test_metal_integer_motion_parity`` and
``test_metal_twin_option_parity`` compare on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_integer_motion_math.h"
KERNEL = "metal/integer_motion.metal"
HOST = "metal/integer_motion_metal.mm"
REFERENCE = "integer_motion.c"
FILTER = "integer_motion.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
OPTION_FIELD = re.compile(r"\.(name|alias|type|min|max|flags)\s*=\s*([^,]+?)\s*(?:,|$)")
OPTION_DEFAULT = re.compile(
    r"\.default_val\s*(?:\.\s*([bdis])\s*=\s*([^,}]+)|=\s*\{\s*\.([bdis])\s*=\s*([^}]+)\})"
)

MATH_PIECES = (
    "const vmaf_mtl_i32 c = (idx < -2) ? -2 : ((idx > size + 1) ? size + 1 : idx);",
    "return (2 * size) - c - 2;",
    "return (vmaf_mtl_i32)((sum + (VMAF_MTL_I64(1) << (bpc - 1u))) >> bpc);",
    "const vmaf_mtl_i32 val = (vmaf_mtl_i32)((sum + 32768) >> 16);",
    "return (vmaf_mtl_u32)((val < 0) ? -val : val);",
)
KERNEL_PIECES = (
    '#include "metal_integer_motion_math.h"',
    "diff[i] = im_sample(prev, offset, hbd) - im_sample(cur, offset, hbd);",
    "vert[i] = vmaf_mtl_motion_vertical(diff[top], diff[top + IM_TILE],",
    "return vmaf_mtl_motion_abs_h(vert[b], vert[b + 1u], vert[b + 2u], vert[b + 3u], vert[b + 4u]);",
    "uint total = 0u;",
    "total += scratch[i];",
)
HOST_PIECES = (
    "s->ring = s->motion_five_frame_window ? 3u : 2u;",
    "upload_plane(s, ref_pic, index % s->ring);",
    "return run_sad_kernel(s, (index + 1u) % s->ring, index % s->ring);",
    "uint64_t sad = 0u;",
    "sad += parts[i];",
    "const double score = (double)sad / 256. / (w * h) * s->motion_fps_weight;",
    "return (score < s->motion_max_val) ? score : s->motion_max_val;",
    '"VMAF_integer_feature_motion_sad_score", score, index);',
    '"VMAF_integer_feature_motion_score", score, index);',
    "vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
    ".motion_five_frame_window = s->motion_five_frame_window,",
    ".motion_moving_average = s->motion_moving_average,",
    ".flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL,",
)
REFERENCE_LINES = (
    "int32_t diff = prev[row * prev_stride + j] - cur[row * cur_stride + j];",
    "y_row[j] = (accum + y_round) >> 8;",
    "y_row[j] = (int32_t)((accum + y_round) >> bpc);",
    "int32_t val = (int32_t)((accum + x_round) >> 16);",
    "score = MIN((double)sad / 256. / (w * h) * s->motion_fps_weight, s->motion_max_val);",
    "vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    names = (MATH, KERNEL, HOST, REFERENCE, FILTER)
    return {name: (FEATURE / name).read_text(encoding="utf-8") for name in names}


def _block(code: str, opener: str) -> str:
    """The brace-matched initializer that follows `opener`, or empty."""
    start = code.find(opener)
    if start < 0:
        return ""
    depth = 0
    for index in range(code.index("{", start), len(code)):
        depth += {"{": 1, "}": -1}.get(code[index], 0)
        if depth == 0:
            return code[code.index("{", start) + 1 : index]
    return ""


def _entries(block: str) -> list[str]:
    """Top-level `{...}` entries of an initializer list."""
    entries, depth, start = [], 0, 0
    for index, char in enumerate(block):
        if char == "{":
            depth += 1
            start = index if depth == 1 else start
        elif char == "}":
            depth -= 1
            if depth == 0:
                entries.append(block[start + 1 : index])
    return entries


def _macros(code: str) -> dict[str, str]:
    return dict(re.findall(r"#define (\w+) \(?([\w.]+)\)?", code))


def _value(text: str, macros: dict[str, str]) -> str:
    text = macros.get(text.strip(), text.strip()).strip("()")
    try:
        return repr(float(text))
    except ValueError:
        return text


def _options(source: str) -> list[dict[str, str]]:
    """Each option of `options[]` as {field: normalised value}."""
    code = _flat(source)
    macros = _macros(COMMENT.sub(" ", source))
    table = []
    for entry in _entries(_block(code, "static const VmafOption options[")):
        fields = {k: _value(v, macros) for k, v in OPTION_FIELD.findall(entry + ",")}
        default = OPTION_DEFAULT.search(entry)
        if default:
            kind = default.group(1) or default.group(3)
            fields["default"] = kind + ":" + _value(default.group(2) or default.group(4), macros)
        if "name" in fields:
            table.append(fields)
    return table


def _provided(source: str) -> list[str]:
    return re.findall(r'"(\w+)"', _block(_flat(source), "static const char *provided_features[]"))


def _math_failures(math: str, filter_h: str) -> list[str]:
    code = _flat(math)
    failures = []
    if any(piece not in code for piece in MATH_PIECES):
        failures.append(f"{MATH}: the mirror or a pass is not integer_motion.c's")
    taps = re.findall(r"#define VMAF_MTL_MOTION_TAP\d (\d+)", math)
    cpu = re.search(r"filter\[5\] = \{([^}]*)\}", filter_h)
    if not cpu or [int(t) for t in cpu.group(1).split(",")][:3] != [int(t) for t in taps]:
        failures.append(f"{MATH}: the taps are not integer_motion.h's filter")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures = []
    if any(piece not in code for piece in KERNEL_PIECES):
        failures.append(f"{KERNEL}: the kernel does not difference before it blurs")
    if re.search(r"blurred|simd_sum|\bfloat\b", code):
        failures.append(f"{KERNEL}: a blurred frame or a float sum is back")
    return failures


def _host_failures(host: str, reference: str) -> list[str]:
    code = _flat(host)
    failures = []
    if any(piece not in code for piece in HOST_PIECES):
        failures.append(f"{HOST}: the host is not the CPU's ring, score and window flush")
    if re.search(r"prev_blur_buf|motion_y_score|double sad_sum|prev_motion_score", code):
        failures.append(f"{HOST}: the blurred ping-pong or the twin's own motion2 is back")
    if _options(host) != _options(reference) or not _options(host):
        failures.append(f"{HOST}: the option table is not integer_motion.c's")
    if _provided(host) != _provided(reference):
        failures.append(f"{HOST}: the provided features are not integer_motion.c's")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _math_failures(sources[MATH], sources[FILTER])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST], sources[REFERENCE])
        + _reference_failures(sources[REFERENCE])
    )


class IntegerMotionMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_the_tables_parse(self) -> None:
        sources = _sources()
        self.assertEqual(len(_options(sources[REFERENCE])), 8)
        self.assertEqual(_provided(sources[REFERENCE])[0], "VMAF_integer_feature_motion_sad_score")

    def test_blur_then_diff_is_detected(self) -> None:
        # The pre-port order: each frame staged and blurred on its own.
        failures = self._edited(
            KERNEL,
            "diff[i] = im_sample(prev, offset, hbd) - im_sample(cur, offset, hbd);",
            "diff[i] = im_sample(cur, offset, hbd); /* cur_blurred[] later */",
        )
        self._assert_detected(failures, "does not difference before it blurs")

    def test_float_reduction_is_detected(self) -> None:
        failures = self._edited(KERNEL, "        uint total = 0u;", "        float total = 0.0f;")
        self._assert_detected(failures, "float sum is back")

    def test_unrounded_vertical_pass_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return (vmaf_mtl_i32)((sum + (VMAF_MTL_I64(1) << (bpc - 1u))) >> bpc);",
            "    return (vmaf_mtl_i32)(sum >> bpc);",
        )
        self._assert_detected(failures, "a pass is not integer_motion.c's")

    def test_changed_tap_is_detected(self) -> None:
        failures = self._edited(
            MATH, "#define VMAF_MTL_MOTION_TAP2 26386", "#define VMAF_MTL_MOTION_TAP2 26385"
        )
        self._assert_detected(failures, "taps are not integer_motion.h's filter")

    def test_lost_ring_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    return run_sad_kernel(s, (index + 1u) % s->ring, index % s->ring);",
            "    return run_sad_kernel(s, index % s->ring, index % s->ring);",
        )
        self._assert_detected(failures, "the CPU's ring, score and window flush")

    def test_twin_motion2_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
            "    const int err = 0; (void)window; (void)s->prev_motion_score;",
        )
        self._assert_detected(failures, "window flush")
        self._assert_detected(failures, "own motion2 is back")

    def test_missing_sad_score_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            '"VMAF_integer_feature_motion_sad_score",\n                                                      score, index);',
            '"VMAF_integer_feature_motion_y_score",\n                                                      score, index);',
        )
        self._assert_detected(failures, "the CPU's ring, score and window flush")
        self._assert_detected(failures, "own motion2 is back")

    def test_option_outside_the_cpu_table_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    {nullptr}};",
            '    {.name = "motion_add_uv", .alias = "mau", .type = VMAF_OPT_TYPE_BOOL,\n'
            "     .default_val = {.b = false}, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},\n"
            "    {nullptr}};",
        )
        self._assert_detected(failures, "option table is not integer_motion.c's")

    def test_changed_default_is_detected(self) -> None:
        failures = self._edited(
            HOST, "        .default_val = {.d = 40.0},", "        .default_val = {.d = 20.0},"
        )
        self._assert_detected(failures, "option table is not integer_motion.c's")

    def test_missing_provided_feature_is_detected(self) -> None:
        failures = self._edited(
            HOST, '    "VMAF_integer_feature_motion3_score",\n    NULL\n', "    NULL\n"
        )
        self._assert_detected(failures, "provided features are not integer_motion.c's")

    def test_changed_reference_score_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "score = MIN((double)sad / 256. / (w * h) * s->motion_fps_weight, s->motion_max_val);",
            "score = MIN((double)sad / 256. / (w * h), s->motion_max_val);",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()

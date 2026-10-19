#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the host tail of every integer VIF twin to the CPU's.

``integer_vif.c`` accumulates in int64 and then leaves integer arithmetic:
``vif_store_residuals()`` stores each scale's numerator and denominator in a
``float``, ``write_scores()`` adds those rounded values for the debug outputs,
and the shared emitter divides in single precision
(``single_precision_ratio``). The CUDA, HIP, SYCL and Metal kernels
accumulate in int64 too, so a twin can only return the CPU's values when its
host tail rounds at the same three points.

Two twins missed a point. ``vif_sycl`` kept the sums in ``double`` and was up
to 3.5e-7 from the CPU on every frame (T-SYCL-VIF-DOUBLE-SUMS-2026-10-01).
``integer_vif_metal`` rounded the sums but divided in ``double``: an Apple M4
Pro measured every scale score of the Netflix 576x324 pair up to 3.0e-8 from
the CPU's (issue #2118, T-METAL-INTEGER-VIF-FP32-GAIN-2026-10-03).

Every twin's ``debug`` option also defaults to the CPU's ``false``: ``vif_sycl``
defaulted to ``true`` and published eleven outputs the CPU extractor does not.

Device-free: reads the sources only. The device checks are
``test_sycl_vif_parity``, ``test_cuda_vif_min_dim`` /
``test_integer_vif_cpu_cuda_parity``, ``test_hip_vif_parity`` and
``test_metal_integer_vif_parity``.
"""

from __future__ import annotations

import re
import unittest
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

CPU = "integer_vif.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")

FLAG = ".single_precision_ratio = true,"
RATIO = "output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;"
FLOAT_SCORE = "struct { float num; float den; } scale[4]; } VifScore;"


@dataclass(frozen=True)
class Twin:
    """Where one twin's host tail lives and what it must contain."""

    path: str
    sums: str  # the function that rounds each scale's sums
    sums_pieces: tuple[str, ...]
    roundings: int  # `(float)(` casts in `sums`; 0 when a float field rounds
    score_set: str  # the function that builds the VmafVifScoreSet
    score_pieces: tuple[str, ...]
    debug_default: str  # the debug option's default, as the table spells it
    float_fields: bool = False  # the score fields are float too (the cast above is explicit)


TWINS = (
    Twin(
        path="sycl/integer_vif_sycl.cpp",
        sums="vif_scale_sums",
        sums_pieces=(
            "(const struct vif_accums *accums, float *vif_scale_num, float *vif_scale_den)",
        ),
        roundings=2,
        score_set="vif_score_set",
        score_pieces=(
            "const float *vif_scale_num, const float *vif_scale_den",
            "output.score_num += vif_scale_num[scale];",
            "output.score_den += vif_scale_den[scale];",
        ),
        debug_default=".default_val = {.b = false},",
    ),
    Twin(
        path="cuda/integer_vif_cuda.c",
        sums="vif_reduce_accums",
        sums_pieces=("vif->scale[scale].num =", "vif->scale[scale].den ="),
        roundings=2,
        score_set="write_scores",
        score_pieces=(
            "output.score_num += vif.scale[scale].num;",
            "output.score_den += vif.scale[scale].den;",
        ),
        debug_default=".default_val.b = false,",
        float_fields=True,
    ),
    Twin(
        path="hip/integer_vif_hip.c",
        sums="write_scores_hip",
        sums_pieces=("vif.scale[sc].num =", "vif.scale[sc].den ="),
        roundings=2,
        score_set="write_scores_hip",
        score_pieces=(
            "output.score_num += vif.scale[sc].num;",
            "output.score_den += vif.scale[sc].den;",
        ),
        debug_default=".default_val.b = false,",
    ),
    Twin(
        path="metal/integer_vif_metal.mm",
        sums="scale_num_den",
        sums_pieces=(
            "const float fnum = (float)(",
            "const float fden = (float)(",
            "*num = (double)fnum;",
            "*den = (double)fden;",
        ),
        roundings=2,
        score_set="collect_fex_metal",
        score_pieces=(
            "output.score_num += num[scale];",
            "output.score_den += den[scale];",
        ),
        debug_default=".default_val = {.b = false},",
    ),
)

TWIN_PATHS = tuple(twin.path for twin in TWINS)


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in (CPU, *TWIN_PATHS)}


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _debug_option(code: str) -> str:
    """The option-table entry named "debug": from its name to the next entry."""
    start = code.find('.name = "debug",')
    if start < 0:
        return ""
    end = code.find(".name = ", start + 1)
    return code[start : end if end >= 0 else len(code)]


def _cpu_failures(sources: dict[str, str]) -> list[str]:
    """The CPU properties the twins mirror; a change here needs every twin changed."""
    cpu = _code(sources[CPU])
    failures: list[str] = []
    store = _function_body(cpu, "vif_store_residuals")
    if "(const VifResiduals *acc, float *num, float *den)" not in store:
        failures.append(f"{CPU}: vif_store_residuals() no longer stores the sums in float")
    if FLAG not in _function_body(cpu, "write_scores"):
        failures.append(f"{CPU}: write_scores() no longer asks for a single-precision ratio")
    if ".default_val.b = false," not in _debug_option(cpu):
        failures.append(f"{CPU}: the debug option no longer defaults to false")
    return failures


def _sums_failures(twin: Twin, code: str) -> list[str]:
    """Each scale's numerator and denominator is a float, as vif_store_residuals() stores it."""
    sums = _function_body(code, twin.sums)
    failures = [
        f"{twin.path}: {twin.sums}() must store each scale's sums in float ({piece})"
        for piece in twin.sums_pieces
        if piece not in sums
    ]
    if sums.count("(float)(") != twin.roundings:
        failures.append(
            f"{twin.path}: {twin.sums}() must round each scale's numerator and denominator "
            f"to float once ({twin.roundings} casts)"
        )
    if (twin.roundings == 0 or twin.float_fields) and FLOAT_SCORE not in code:
        failures.append(f"{twin.path}: VifScore must hold each scale's sums as float")
    return failures


def _score_set_failures(twin: Twin, code: str) -> list[str]:
    """The score set is integer_vif.c::write_scores()'s."""
    score_set = _function_body(code, twin.score_set)
    failures: list[str] = []
    if FLAG not in score_set:
        failures.append(
            f"{twin.path}: {twin.score_set}() must ask the emitter for the CPU's "
            "single_precision_ratio"
        )
    for piece in (*twin.score_pieces, RATIO):
        if piece not in score_set:
            failures.append(f"{twin.path}: {twin.score_set}() is not write_scores() ({piece})")
    return failures


def _twin_failures(twin: Twin, sources: dict[str, str]) -> list[str]:
    code = _code(sources[twin.path])
    failures = _sums_failures(twin, code) + _score_set_failures(twin, code)
    if twin.debug_default not in _debug_option(code):
        failures.append(f"{twin.path}: the debug option must default to false, as on the CPU")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    failures = _cpu_failures(sources)
    for twin in TWINS:
        failures += _twin_failures(twin, sources)
    return failures


class VifFloatSumsContractTest(unittest.TestCase):
    def _edited(self, path: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[path])
        sources[path] = sources[path].replace(old, new, 1)
        return _failures(sources)

    def test_live_sources_round_where_the_cpu_does(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_dropped_single_precision_ratio_is_detected_in_every_twin(self) -> None:
        for twin in TWINS:
            with self.subTest(twin=twin.path):
                sources = _sources()
                line = re.search(
                    r"\n[ \t]*\.single_precision_ratio\s*=\s*true,", sources[twin.path]
                )
                self.assertIsNotNone(line, f"{twin.path} has no single_precision_ratio line")
                sources[twin.path] = sources[twin.path].replace(line.group(0), "", 1)
                failures = _failures(sources)
                self.assertTrue(
                    any(
                        f.startswith(twin.path) and "single_precision_ratio" in f for f in failures
                    ),
                    failures,
                )

    def test_double_scale_sums_are_detected(self) -> None:
        failures = self._edited(
            "sycl/integer_vif_sycl.cpp",
            "static inline void vif_scale_sums(const struct vif_accums *accums, "
            "float *vif_scale_num,\n                                  float *vif_scale_den)",
            "static inline void vif_scale_sums(const struct vif_accums *accums, "
            "double *vif_scale_num,\n                                  double *vif_scale_den)",
        )
        self.assertTrue(any("sums in float" in failure for failure in failures), failures)

    def test_double_metal_scale_sums_are_detected(self) -> None:
        failures = self._edited(
            "metal/integer_vif_metal.mm",
            "const float fnum = (float)(",
            "const double fnum = (double)(",
        )
        self.assertTrue(
            any(f.startswith("metal/") and "sums in float" in f for f in failures), failures
        )

    def test_double_cuda_score_fields_are_detected(self) -> None:
        failures = self._edited(
            "cuda/integer_vif_cuda.c",
            "        float num;\n        float den;\n    } scale[4];\n} VifScore;",
            "        double num;\n        double den;\n    } scale[4];\n} VifScore;",
        )
        self.assertTrue(any(f.startswith("cuda/") and "VifScore" in f for f in failures), failures)

    def test_debug_default_true_is_detected(self) -> None:
        failures = self._edited(
            "sycl/integer_vif_sycl.cpp",
            ".default_val = {.b = false},",
            ".default_val = {.b = true},",
        )
        self.assertTrue(
            any("debug option must default" in failure for failure in failures), failures
        )

    def test_unrounded_frame_sum_is_detected(self) -> None:
        failures = self._edited(
            "sycl/integer_vif_sycl.cpp",
            "            output.score_num += vif_scale_num[scale];",
            "            output.score_num += exact_num[scale];",
        )
        self.assertTrue(any("write_scores()" in failure for failure in failures), failures)


if __name__ == "__main__":
    unittest.main()

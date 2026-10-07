#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every extractor that derives motion2 / motion3 through the shared window
also derives them frame by frame (ADR-2090).

integer_motion.c::vmaf_motion_window_advance() appends motion2 / motion3 of
each frame once the SAD scores of frames 0 .. max(i + 1, min_idx) are in;
vmaf_motion_window_flush() appends the rest. Both carry their state in a
VmafMotionWindowState of the extractor. An extractor that calls the flush but
registers no `.advance` callback, or calls the advance without a state, still
compiles and still returns the CPU's values, but its windows complete only at
the flush: the live window scores of #2138 / #2238 arrive at the end of the
stream again. This test reads the sources of every backend (the CPU
extractors, CUDA, SYCL, HIP and Metal twins; Metal has no device here) and
holds each one to:

- the TU that calls vmaf_motion_window_flush() also calls
  vmaf_motion_window_advance() from a function it registers as `.advance`;
- the window both calls build carries `.state = &s->window_state`, a
  VmafMotionWindowState member of the extractor's private data;
- the engine (libvmaf.c) calls advance() after a frame is accepted, after a
  zero-copy SYCL frame, after a read fence and in vmaf_engine_advance(),
  which the VMAFx completion thread (vmafx/window.c) calls before each pass,
  so a window over a VMAF model completes while the feeder stalls.

Each check has a planted-regression case below that must be detected.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE = Path(__file__).resolve().parents[1]
SRC = CORE / "src"

WINDOW_TUS = (
    "feature/integer_motion.c",
    "feature/integer_motion_v2.c",
    "feature/cuda/integer_motion_cuda.c",
    "feature/cuda/integer_motion_v2_cuda.c",
    "feature/sycl/integer_motion_sycl.cpp",
    "feature/sycl/integer_motion_v2_sycl.cpp",
    "feature/hip/integer_motion_hip.c",
    "feature/hip/integer_motion_v2_hip.c",
    "feature/metal/integer_motion_metal.mm",
    "feature/metal/integer_motion_v2_metal.mm",
)
ENGINE = "libvmaf.c"
ENGINE_CALLS = (
    "return err ? err : advance_extractors(vmaf);",
    "return extract_err ? extract_err : advance_extractors(vmaf);",
    "return advance_extractors(vmaf);",
)
WINDOWS = "vmafx/window.c"
WINDOWS_CALL = "const int err = vmaf_engine_advance(context->engine);"
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
ADVANCE_SLOT = re.compile(r"\.advance\s*=\s*(\w+)\s*,")
FUNCTION = r"(?:static\s+)?int\s+{name}\s*\(\s*VmafFeatureExtractor\s*\*\s*fex\s*,"


def _code(text: str) -> str:
    """Source without comments."""
    return COMMENT.sub(" ", text)


def _body(code: str, name: str) -> str:
    """The brace-matched body of function `name`, or empty."""
    match = re.search(FUNCTION.format(name=re.escape(name)), code)
    if not match:
        return ""
    start = code.find("{", match.end())
    depth = 0
    for pos in range(start, len(code)):
        depth += {"{": 1, "}": -1}.get(code[pos], 0)
        if depth == 0:
            return code[start : pos + 1]
    return ""


def _tu_failures(name: str, text: str) -> list[str]:
    code = _code(text)
    failures: list[str] = []
    if "vmaf_motion_window_flush(" not in code:
        failures.append(f"{name}: no longer derives through vmaf_motion_window_flush()")
        return failures
    slot = ADVANCE_SLOT.search(code)
    advance = _body(code, slot.group(1)) if slot else ""
    if "vmaf_motion_window_advance(" not in advance:
        failures.append(f"{name}: no `.advance` callback calls vmaf_motion_window_advance()")
    if code.count(".state = &s->window_state,") != 1:
        failures.append(f"{name}: the window does not carry the extractor's state")
    if not re.search(r"VmafMotionWindowState\s+window_state\s*;", code):
        failures.append(f"{name}: the private data has no VmafMotionWindowState")
    return failures


def _engine_failures(text: str) -> list[str]:
    code = _code(text)
    missing = [call for call in ENGINE_CALLS if call not in code]
    if missing or "err = fex_ctx->fex->advance(fex_ctx->fex, vmaf->feature_collector);" not in code:
        return [f"{ENGINE}: the engine does not advance the extractors: {missing}"]
    return []


def _windows_failures(text: str) -> list[str]:
    code = _code(text)
    evaluate = code[code.find("static void evaluate(") :]
    if WINDOWS_CALL not in code or "advance_engine(set->context);" not in evaluate:
        return [f"{WINDOWS}: the completion thread does not advance the engine before it probes"]
    return []


def _sources() -> dict[str, str]:
    names = (*WINDOW_TUS, ENGINE, WINDOWS)
    return {name: (SRC / name).read_text(encoding="utf-8") for name in names}


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in WINDOW_TUS:
        failures += _tu_failures(name, sources[name])
    return failures + _engine_failures(sources[ENGINE]) + _windows_failures(sources[WINDOWS])


class MotionWindowAdvanceContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_missing_advance_slot_is_detected(self) -> None:
        failures = self._edited(
            "feature/metal/integer_motion_v2_metal.mm", ".advance = advance_fex_metal,", ""
        )
        self._assert_detected(failures, "integer_motion_v2_metal.mm: no `.advance` callback")

    def test_advance_that_only_flushes_is_detected(self) -> None:
        name = "feature/sycl/integer_motion_v2_sycl.cpp"
        source = _sources()[name]
        body = _body(_code(source), "advance_fex_sycl")
        self.assertIn("vmaf_motion_window_advance(", body)
        failures = self._edited(
            name,
            "return vmaf_motion_window_advance(feature_collector, s->feature_name_dict, &window);",
            "return vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);",
        )
        self._assert_detected(failures, "integer_motion_v2_sycl.cpp: no `.advance` callback")

    def test_stateless_window_is_detected(self) -> None:
        failures = self._edited(
            "feature/cuda/integer_motion_cuda.c", "        .state = &s->window_state,\n", ""
        )
        self._assert_detected(failures, "integer_motion_cuda.c: the window does not carry")

    def test_missing_state_member_is_detected(self) -> None:
        failures = self._edited(
            "feature/hip/integer_motion_hip.c",
            "    VmafMotionWindowState window_state;",
            "    unsigned window_state;",
        )
        self._assert_detected(failures, "integer_motion_hip.c: the private data has no")

    def test_engine_without_advance_is_detected(self) -> None:
        failures = self._edited(
            ENGINE,
            "return err ? err : advance_extractors(vmaf);",
            "return err;",
        )
        self._assert_detected(failures, "the engine does not advance the extractors")

    def test_completion_thread_without_advance_is_detected(self) -> None:
        failures = self._edited(WINDOWS, "        advance_engine(set->context);\n", "")
        self._assert_detected(failures, "does not advance the engine before it probes")

    def test_cpu_extractor_without_advance_is_detected(self) -> None:
        failures = self._edited("feature/integer_motion.c", ".advance = advance,", "")
        self._assert_detected(failures, "feature/integer_motion.c: no `.advance` callback")


if __name__ == "__main__":
    unittest.main()

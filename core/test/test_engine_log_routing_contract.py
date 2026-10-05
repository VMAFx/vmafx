#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Engine messages go through vmaf_log() (ADR-1906, full log routing).

A VMAFx context with a log callback receives every message the library raises
for it; vmaf_log() delivers to that context's sink on whatever thread raises
the message. A direct write to stdout or stderr in engine code bypasses the
sink and lands in the process log. This test scans every C, C++, Objective-C
and GPU source under core/src for such writes (printf, puts, perror,
fprintf / fputs / vfprintf to stdout or stderr, std::cout / std::cerr) and
refuses any outside the exception table below. Each exception names one file,
the number of writes it may hold, the reason and when it expires; a count that
grows or shrinks fails, so a removal has to update the table.

The planted-defect cases at the end check the scanner itself.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE_SRC = Path(__file__).resolve().parents[1] / "src"
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".mm", ".cu", ".cuh", ".hip"}

WRITE = re.compile(
    r"(?<![\w.])(?:std::)?(?:printf|puts|perror|vprintf)\s*\("
    r"|(?<![\w.])(?:std::)?(?:fprintf|fputs|vfprintf)\s*\([^;]*\b(?:stdout|stderr)\b"
    r"|std::(?:cout|cerr|clog)\b"
)

# The process log itself and the VMAFx fallback for a context without a log
# callback are the destination, not a bypass.
SINKS = {
    "log.cpp": "the process log: vmaf_log() writes here when no sink is installed",
    "log.c": "the C logger before ADR-0708; not compiled (log.cpp replaced it)",
    "vmafx/error.c": "a failure without an error out-parameter and without a log callback",
}

# path -> (writes, reason, expiry)
EXCEPTIONS = {
    "svm.cpp": (
        10,
        "vendored libsvm: training and cross-validation output the engine never calls, "
        "an out-of-memory line before abort(), and the model-text parser error, which "
        "is raised while loading a model (no context) and also reported to the caller "
        "as the load's failure",
        "RC4: the parser error moves to vmaf_log() with the libsvm link change (WP6)",
    ),
    "feature/vif.c": (
        9,
        "vifdiff(), a stand-alone tool entry point no extractor calls, and the "
        "VIF_OPT_DEBUG_DUMP build-time debug output",
        "RC5 deduplication (#1724) removes vifdiff()",
    ),
    "libvmaf.c": (
        2,
        "vmaf_write_output(): the libvmaf report writer, which no VMAFx call reaches",
        "RC4 WP5: vmafx_report_write() replaces it",
    ),
    "sycl/common.cpp": (
        16,
        "SYCL device listing and profiling print APIs and upload timing traces; no "
        "VMAFx context attaches a SYCL device before WP3",
        "RC4 WP3 SYCL lane",
    ),
    "cuda/cuda_helper.cuh": (
        2,
        "CUDA runtime-error check macros; no VMAFx context attaches a CUDA device before WP3",
        "RC4 WP3 CUDA lane",
    ),
    "metal/common.mm": (
        2,
        "Metal device listing print API; no VMAFx context attaches a Metal device " "before WP3",
        "RC4 WP3 Metal lane",
    ),
}


def _code_only(text: str) -> str:
    """The source without comments and string contents that could look like calls."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def count_writes(text: str) -> int:
    return len(WRITE.findall(_code_only(text)))


def scan() -> dict[str, int]:
    found: dict[str, int] = {}
    for path in sorted(CORE_SRC.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        n = count_writes(path.read_text(encoding="utf-8", errors="replace"))
        if n:
            found[path.relative_to(CORE_SRC).as_posix()] = n
    return found


class EngineLogRoutingContract(unittest.TestCase):
    def test_no_write_bypasses_vmaf_log(self) -> None:
        found = scan()
        unexpected = {
            path: n for path, n in found.items() if path not in SINKS and path not in EXCEPTIONS
        }
        self.assertEqual(
            unexpected,
            {},
            "direct stdout/stderr writes bypass the context's log sink; use vmaf_log()",
        )

    def test_exception_counts_are_exact(self) -> None:
        found = scan()
        for path, (count, _reason, _expiry) in EXCEPTIONS.items():
            with self.subTest(path=path):
                self.assertEqual(found.get(path, 0), count, f"update EXCEPTIONS[{path!r}]")

    def test_every_exception_names_reason_and_expiry(self) -> None:
        for path, (_count, reason, expiry) in EXCEPTIONS.items():
            with self.subTest(path=path):
                self.assertTrue(reason and expiry)

    def test_scanner_finds_planted_writes(self) -> None:
        planted = [
            'printf("error: x\\n");',
            '(void)printf("error: x\\n");',
            'fprintf(stderr, "x");',
            'fputs("x", stdout);',
            "std::cerr << x;",
            'puts("x");',
        ]
        for line in planted:
            with self.subTest(line=line):
                self.assertEqual(count_writes(f"void f(void) {{ {line} }}"), 1)

    def test_scanner_ignores_files_comments_and_logs(self) -> None:
        clean = [
            'fprintf(outfile, "%d", 1);',
            'snprintf(buf, sizeof(buf), "x");',
            'vmaf_log(VMAF_LOG_LEVEL_ERROR, "x");',
            '/* printf("x"); */',
            '// fprintf(stderr, "x");',
        ]
        for line in clean:
            with self.subTest(line=line):
                self.assertEqual(count_writes(f"void f(void) {{ {line} }}"), 0)


if __name__ == "__main__":
    unittest.main()

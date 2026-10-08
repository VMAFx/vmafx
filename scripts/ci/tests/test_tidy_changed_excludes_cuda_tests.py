#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""`Tidy Changed` leaves out every CUDA-only test source.

The job runs clang-tidy on the C and C++ files a push changed, against the CPU
build's compile database. A source that needs the CUDA headers and the
fork-private `vmafx/frame_import_hooks.h` of a CUDA configuration has no entry
there and fails with `clang-diagnostic-error: file not found`; the CUDA lane of
the ratchet measures it instead. `exclude_untidyable()` in `lint-and-format.yml`
lists those sources; `core/test/test_vmafx_import_cuda*.c` (added by #2277) were
missing, and the job failed on master (run 37775116233).

The contract: every `core/test/test_vmafx_import_cuda*.c` and
`core/test/test_cuda_*.c` matches a pattern of `exclude_untidyable()`.
Positive, negative and boundary cases run on synthetic patterns; the last test
reads the workflow and the tree.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
PATTERN = re.compile(r"^\s*-e '([^']+)'", re.M)
FUNCTION = re.compile(r"exclude_untidyable\(\) \{(.*?)\n\s*\}", re.S)


def patterns(workflow: str) -> list[str]:
    body = FUNCTION.search(workflow)
    return PATTERN.findall(body.group(1)) if body else []


def unexcluded(paths: list[str], regexes: list[str]) -> list[str]:
    return [p for p in paths if not any(re.search(r, p) for r in regexes)]


class TidyChangedExcludesCudaTests(unittest.TestCase):
    def test_every_cuda_only_test_source_is_excluded(self) -> None:
        regexes = patterns(WORKFLOW.read_text(encoding="utf-8"))
        self.assertGreater(len(regexes), 10, "exclude_untidyable() was not read")
        sources = sorted(
            str(p.relative_to(ROOT))
            for pattern in ("test_vmafx_import_cuda*.c", "test_cuda_*.c")
            for p in (ROOT / "core" / "test").glob(pattern)
        )
        self.assertTrue(sources, "no CUDA test source found")
        self.assertEqual(unexcluded(sources, regexes), [])

    def test_planted_unlisted_source_is_refused(self) -> None:
        regexes = [r"^core/test/test_cuda_"]
        self.assertEqual(
            unexcluded(["core/test/test_vmafx_import_cuda.c"], regexes),
            ["core/test/test_vmafx_import_cuda.c"],
        )

    def test_listed_source_is_accepted(self) -> None:
        regexes = [r"^core/test/test_vmafx_import_cuda"]
        self.assertEqual(unexcluded(["core/test/test_vmafx_import_cuda_fence.c"], regexes), [])


if __name__ == "__main__":
    unittest.main()

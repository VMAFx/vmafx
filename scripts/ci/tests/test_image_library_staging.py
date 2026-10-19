# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Every image that stages libvmaf stages libvmafx with it (ADR-2094).

Since the library split the vmaf CLI and the compat libvmaf.so.3 both need
libvmafx.so.1. A Dockerfile command that copies or checks the libvmaf chain
(`libvmaf.so*`, `libvmaf.so.*`) without the libvmafx chain ships an image
whose CLI cannot load, or checks only half of what it ships. The check reads
each RUN instruction as one logical line and each `&&` step of it on its own.
Positive (every Dockerfile in the tree), negative and boundary (planted text).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LIBVMAF_CHAIN = re.compile(r"libvmaf\.so(?:\*|\.\*)")


def dockerfiles() -> list[Path]:
    found = sorted((REPO / "docker").glob("Dockerfile*")) + sorted(REPO.glob("Dockerfile*"))
    return [path for path in found if path.is_file()]


def logical_lines(text: str) -> list[str]:
    """Instruction lines with their backslash continuations joined; comments dropped."""
    lines: list[str] = []
    current = ""
    for raw in text.splitlines():
        if raw.lstrip().startswith("#"):
            continue
        if raw.rstrip().endswith("\\"):
            current += raw.rstrip()[:-1] + " "
            continue
        lines.append(current + raw)
        current = ""
    if current:
        lines.append(current)
    return lines


def half_staged_steps(text: str) -> list[str]:
    """The `&&` steps that name the libvmaf chain but not libvmafx."""
    steps = []
    for line in logical_lines(text):
        for step in line.split("&&"):
            if LIBVMAF_CHAIN.search(step) and "libvmafx.so" not in step:
                steps.append(" ".join(step.split()))
    return steps


class ImageLibraryStagingTest(unittest.TestCase):
    def test_every_dockerfile_stages_both_chains(self) -> None:
        files = dockerfiles()
        self.assertIn(REPO / "docker" / "Dockerfile.production-gpu", files)
        self.assertIn(REPO / "docker" / "Dockerfile.tester", files)
        problems = [
            f"{path.relative_to(REPO)}: {step}"
            for path in files
            for step in half_staged_steps(path.read_text(encoding="utf-8"))
        ]
        self.assertEqual(problems, [])

    def test_a_copy_of_the_libvmaf_chain_alone_is_found(self) -> None:
        text = (
            "RUN mkdir -p /dist/lib \\\n"
            "    && find build/src -maxdepth 1 -name 'libvmaf.so*' -not -type d \\\n"
            "        -exec cp -a {} /dist/lib/ \\; \\\n"
            '    && for binary in /dist/bin/vmaf /dist/lib/libvmaf.so.*; do ldd "$binary"; done\n'
        )
        self.assertEqual(len(half_staged_steps(text)), 2)

    def test_both_chains_in_one_step_pass_and_comments_are_ignored(self) -> None:
        text = (
            "# copies libvmaf.so* only in a comment\n"
            "RUN find build/src -maxdepth 1 \\( -name 'libvmaf.so*' -o -name 'libvmafx.so*' \\) \\\n"
            "        -not -type d -exec cp -a {} /dist/lib/ \\; \\\n"
            "    && for binary in /dist/lib/libvmaf.so.* \\\n"
            '        /dist/lib/libvmafx.so.*; do ldd "$binary"; done\n'
        )
        self.assertEqual(half_staged_steps(text), [])


if __name__ == "__main__":
    unittest.main()

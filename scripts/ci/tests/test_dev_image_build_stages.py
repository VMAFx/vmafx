#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""No stage the dev image is made of compiles libvmaf or FFmpeg.

A build tree that a later ``RUN`` deletes stays in the layer that created it:
the Meson tree was 8.25 GB of every dev image and the FFmpeg tree 1.1 GB. Both
builds therefore run in stages of their own (``libvmaf-compile``,
``ffmpeg-compile``) that install into a staging root, and the stages an image is
built FROM copy that root. This test walks the FROM chain of every image target
and refuses a compile step in it.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CONTAINERFILE = Path(__file__).resolve().parents[3] / "dev" / "Containerfile"
IMAGE_TARGETS = ("dev-mcp", "libvmaf-build")
COMPILE_STEPS = {
    "the libvmaf build": re.compile(
        r"\bninja -C core/build(?! install)|\bmeson setup core/build\b"
    ),
    "the FFmpeg build": re.compile(r"\./configure\b[^\n]*(?:\\\n[^\n]*)*--enable-libvmaf"),
}
FROM = re.compile(r"(?im)^FROM\s+(?P<parent>\S+)(?:\s+AS\s+(?P<name>\S+))?\s*$")


def stages(text: str) -> dict[str, tuple[str, str]]:
    """Stage name -> (parent reference, body)."""
    found = list(FROM.finditer(text))
    result: dict[str, tuple[str, str]] = {}
    for index, match in enumerate(found):
        end = found[index + 1].start() if index + 1 < len(found) else len(text)
        name = (match.group("name") or f"stage-{index}").lower()
        result[name] = (match.group("parent").lower(), text[match.end() : end])
    return result


def chain(found: dict[str, tuple[str, str]], target: str) -> list[str]:
    """The target and every stage it is built FROM, nearest first."""
    names: list[str] = []
    current = target
    while current in found and current not in names:
        names.append(current)
        current = found[current][0]
    return names


def compile_steps_in_image(text: str) -> list[str]:
    found = stages(text)
    problems: list[str] = []
    for target in IMAGE_TARGETS:
        if target not in found:
            problems.append(f"{target}: no such stage")
            continue
        for name in chain(found, target):
            body = "\n".join(
                line for line in found[name][1].splitlines() if not line.lstrip().startswith("#")
            )
            problems += [
                f"{target}: stage {name} runs {what}"
                for what, pattern in COMPILE_STEPS.items()
                if pattern.search(body)
            ]
    return problems


class BuildStages(unittest.TestCase):
    def test_no_image_stage_compiles(self) -> None:
        text = CONTAINERFILE.read_text(encoding="utf-8")
        self.assertEqual(compile_steps_in_image(text), [])

    def test_the_compile_steps_still_exist_somewhere(self) -> None:
        text = CONTAINERFILE.read_text(encoding="utf-8")
        for what, pattern in COMPILE_STEPS.items():
            with self.subTest(step=what):
                self.assertRegex(text, pattern)

    def test_staging_roots_are_what_the_image_copies(self) -> None:
        found = stages(CONTAINERFILE.read_text(encoding="utf-8"))
        self.assertIn("COPY --from=libvmaf-compile /stage/libvmaf/ /", found["codec-deps"][1])
        self.assertIn("COPY --from=ffmpeg-compile /stage/ffmpeg/ /", found["libvmaf-build"][1])

    def test_a_build_in_the_image_chain_is_refused(self) -> None:
        planted = """
FROM base AS gpu-sdks
RUN true
FROM gpu-sdks AS libvmaf-build
RUN meson setup core/build core --prefix=/usr/local && ninja -C core/build
RUN ninja -C core/build install && rm -rf core/build
RUN ./configure --prefix=/usr/local \\\\
        --enable-libvmaf && make
FROM libvmaf-build AS dev-mcp
RUN true
"""
        problems = compile_steps_in_image(planted.replace("\\\\\n", "\\\n"))
        self.assertEqual(
            problems,
            [
                "dev-mcp: stage libvmaf-build runs the libvmaf build",
                "dev-mcp: stage libvmaf-build runs the FFmpeg build",
                "libvmaf-build: stage libvmaf-build runs the libvmaf build",
                "libvmaf-build: stage libvmaf-build runs the FFmpeg build",
            ],
        )

    def test_a_copied_from_stage_is_not_in_the_chain(self) -> None:
        split = """
FROM base AS deps
RUN true
FROM deps AS compile
RUN meson setup core/build core && ninja -C core/build
RUN DESTDIR=/stage ninja -C core/build install
FROM deps AS libvmaf-build
COPY --from=compile /stage/ /
FROM libvmaf-build AS dev-mcp
RUN true
"""
        self.assertEqual(compile_steps_in_image(split), [])


if __name__ == "__main__":
    unittest.main()

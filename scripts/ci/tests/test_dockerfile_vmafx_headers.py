#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A Dockerfile stage that installs the libvmaf headers installs the vmafx headers too.

FFmpeg's `--enable-libvmafx` probe and the cgo packages compile `<vmafx/vmafx.h>`.
A stage that copies only `core/include/libvmaf/` into `/usr/local/include` fails that
probe with `libvmafx >= 1.0.0 not found using pkg-config`, which hides the missing
header behind a version message.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
LIBVMAF = re.compile(r"^COPY\s+core/include/libvmaf/\s+/usr/local/include/libvmaf/\s*$", re.M)
VMAFX = re.compile(r"^COPY\s+core/include/vmafx/\s+/usr/local/include/vmafx/\s*$", re.M)
STAGE = re.compile(r"^FROM\s+\S+(?:\s+AS\s+(\S+))?\s*$", re.M | re.I)


def stages_missing_vmafx(text: str) -> list[str]:
    """Names of stages that copy the libvmaf headers but not the vmafx headers."""
    starts = list(STAGE.finditer(text))
    missing: list[str] = []
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
        body = text[match.end() : end]
        if LIBVMAF.search(body) and not VMAFX.search(body):
            missing.append(match.group(1) or f"stage {index + 1}")
    return missing


class DockerfileVmafxHeaders(unittest.TestCase):
    def test_repository_dockerfiles_install_both_header_trees(self) -> None:
        """Positive: every Dockerfile under docker/ pairs the two header copies."""
        dockerfiles = sorted((REPO / "docker").glob("Dockerfile*"))
        self.assertTrue(dockerfiles)
        for path in dockerfiles:
            with self.subTest(dockerfile=path.name):
                self.assertEqual(stages_missing_vmafx(path.read_text(encoding="utf-8")), [])

    def test_libvmaf_only_stage_is_reported(self) -> None:
        """Negative: a stage with only the libvmaf header copy is named."""
        text = (
            "FROM debian AS ffmpeg-deps\n"
            "COPY core/include/libvmaf/ /usr/local/include/libvmaf/\n"
            "FROM debian AS ok\n"
            "COPY core/include/libvmaf/ /usr/local/include/libvmaf/\n"
            "COPY core/include/vmafx/ /usr/local/include/vmafx/\n"
        )
        self.assertEqual(stages_missing_vmafx(text), ["ffmpeg-deps"])

    def test_stage_without_headers_needs_none(self) -> None:
        """Boundary: a stage that installs no header tree, or only vmafx's, is fine."""
        text = "FROM debian\nRUN true\nFROM debian AS v\nCOPY core/include/vmafx/ /usr/local/include/vmafx/\n"
        self.assertEqual(stages_missing_vmafx(text), [])


if __name__ == "__main__":
    unittest.main()

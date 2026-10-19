#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The psnr_hvs twins' prefix scan visits every chunk of 256 blocks.

T-GPU-PSNR-HVS-SCAN-32768-CHUNKS-2026-10-05. The CUDA, HIP and SYCL twins
store 64 terms per 8x8 block, count the kept terms per block, add the counts
of every chunk of 256 blocks (scan reduce), turn the chunk totals into chunk
offsets (scan prefix, one work-item) and compact the terms by those offsets.
The HIP and SYCL prefix stopped at 32768 chunks
(`limit = num_chunks < 32768u ? num_chunks : 32768u`), so above 8,388,608
blocks (16384x8640 in 4:4:4, just past 16K) the offsets of the later chunks
were never written and the compaction wrote their terms out of bounds. The
CUDA twin always scanned every chunk.

Device-free: reads the three kernel sources, checks the cap-derived chunk
count against the 32-bit totals, and reports the master form (planted below).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE = Path(__file__).resolve().parents[1] / "src" / "feature"
SCANS = {
    "cuda": (FEATURE / "cuda" / "integer_psnr_hvs" / "psnr_hvs_score.cu", "hvs_scan_prefix"),
    "hip": (FEATURE / "hip" / "integer_psnr_hvs" / "psnr_hvs_score.hip", "hvs_scan_prefix_hip"),
    "sycl": (FEATURE / "sycl" / "integer_psnr_hvs_sycl.cpp", "launch_scan_prefix"),
}
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
EVERY_CHUNK = re.compile(r"for\s*\(\s*unsigned\s+c\s*=\s*0u?\s*;\s*c\s*<\s*num_chunks\s*;")
CAPPED = re.compile(r"num_chunks\s*<\s*\d+u?\s*\?|\bmin\s*\(\s*num_chunks")

# VMAF_PIC_DIM_MAX (core/src/picture.c) and the twins' block grid.
PIC_DIM_MAX = 32768
BLOCK, STEP, CHUNK, TERMS = 8, 7, 256, 64


def body(text: str, name: str) -> str:
    """The comment-free body of function `name`, or an empty string."""
    clean = COMMENT.sub(" ", text)
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", clean)
    if match is None:
        return ""
    depth, index = 0, match.end() - 1
    for index in range(match.end() - 1, len(clean)):
        depth += {"{": 1, "}": -1}.get(clean[index], 0)
        if depth == 0:
            break
    return clean[match.start() : index + 1]


def problems(backend: str, text: str) -> list[str]:
    """Why the prefix scan of `text` does not visit every chunk."""
    _, name = SCANS[backend]
    scan = body(text, name)
    if not scan:
        return [f"{backend}: {name}() not found"]
    found = []
    if CAPPED.search(scan):
        found.append(f"{backend}: the prefix scan caps the chunk count")
    if not EVERY_CHUNK.search(scan):
        found.append(f"{backend}: the prefix scan does not run to num_chunks")
    return found


# hvs_scan_prefix_hip() as HIP and SYCL had it on master (571565a47).
OLD_FORM = """
__global__ __launch_bounds__(1) void hvs_scan_prefix_hip(const uint32_t *chunk_totals,
                                                         uint32_t *chunk_offsets,
                                                         struct PsnrHvsHipHeader *header,
                                                         unsigned num_chunks)
{
    uint32_t running = 0u;
    const unsigned limit = num_chunks < 32768u ? num_chunks : 32768u;
    for (unsigned c = 0u; c < limit; c++) {
        chunk_offsets[c] = running;
        running += chunk_totals[c];
    }
}
"""


class Bounds(unittest.TestCase):
    def test_the_cap_needs_more_than_32768_chunks(self) -> None:
        side = (PIC_DIM_MAX - BLOCK) // STEP + 1
        blocks = 3 * side * side  # 4:4:4 at the cap
        chunks = -(-blocks // CHUNK)
        self.assertEqual((side, blocks, chunks), (4681, 65735283, 256779))
        self.assertGreater(chunks, 32768)
        # The running offset and the term total are uint32: 64 terms per block fit.
        self.assertLess(TERMS * blocks, 2**32)

    def test_16k_444_fits_and_the_next_width_does_not(self) -> None:
        def chunks(w: int, h: int) -> int:
            blocks = 3 * ((w - BLOCK) // STEP + 1) * ((h - BLOCK) // STEP + 1)
            return -(-blocks // CHUNK)

        self.assertLessEqual(chunks(15360, 8640), 32768)
        self.assertGreater(chunks(16384, 8640), 32768)


class LiveSources(unittest.TestCase):
    def test_every_twin_scans_every_chunk(self) -> None:
        for backend, (path, _) in SCANS.items():
            with self.subTest(backend=backend):
                self.assertEqual(problems(backend, path.read_text(encoding="utf-8")), [])


class PlantedRegressions(unittest.TestCase):
    def test_the_master_form_is_reported(self) -> None:
        found = problems("hip", OLD_FORM)
        self.assertIn("hip: the prefix scan caps the chunk count", found)
        self.assertIn("hip: the prefix scan does not run to num_chunks", found)

    def test_a_cap_in_the_sycl_scan_is_reported(self) -> None:
        path, _ = SCANS["sycl"]
        text = path.read_text(encoding="utf-8")
        mutated = text.replace(
            "for (unsigned c = 0u; c < num_chunks; c++)",
            "for (unsigned c = 0u; c < (num_chunks < 65536u ? num_chunks : 65536u); c++)",
        )
        self.assertNotEqual(mutated, text)
        self.assertIn("sycl: the prefix scan caps the chunk count", problems("sycl", mutated))


if __name__ == "__main__":
    unittest.main()

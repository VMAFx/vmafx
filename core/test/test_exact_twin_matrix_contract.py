#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the exact-twin depth x layout matrix.

`scripts/ci/exact_twin_matrix.py` runs every twin declared exact in
`scripts/ci/exact_twins.d/` at 8, 10, 12 and 16 bits and in 4:2:0, 4:2:2 and
4:4:4 against the CPU, and `docs/development/exact-twin-matrix.md` records the
result per device backend. This test holds, without a device:

- the recorded page has a full, passing row for every exact twin of CUDA,
  SYCL and HIP (a newly declared twin without a recorded run fails here);
- the check on the page reports a missing row, a failing cell, a short row and
  an `n/a` below 16 bits;
- the generated fixtures are the same bytes on every host, have the geometry
  the page describes, and reach 0 and the maximum of every depth;
- the comparison reports a one-ulp difference, an output only one run has and a
  frame-count difference, and takes two nulls or two NaNs as equal;
- `--record` replaces only the block of the backend it ran.

The large grids (8K for every device twin, 16K for the CPU extractors' SIMD
against their scalar code) have their own recorded blocks: the page must
record a full row for every twin in each, worst-case fixture planes are
pinned bytes and reach the extremes they are written for, and a large-picture
refusal is `n/a` only for a feature in SIZE_REFUSED.
"""

from __future__ import annotations

import hashlib
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.ci import exact_twin_matrix as matrix  # noqa: E402
from scripts.ci.cross_backend_calibration import EXACT_TWINS  # noqa: E402

PAGE = ROOT / "docs" / "development" / "exact-twin-matrix.md"

# sha256 (first 16 hex digits) of plane_bytes() for frame 1 of the distorted
# picture: luma (357x353) and 4:2:0 chroma (179x177) at each depth.
PINNED_PLANES = {
    (8, 0): "efc30a93c426fd79",
    (8, 1): "ad0cd2d14101c3ba",
    (10, 0): "7e1a39130143c099",
    (10, 1): "79b78d9b8e86a403",
    (12, 0): "514cbc0654a9254e",
    (12, 1): "aaae1821993c9033",
    (16, 0): "a09bbef53ef1e4e6",
    (16, 1): "b058683e76d66984",
}
MS_SSIM_MIN_SIDE = 176
# sha256 (first 16 hex digits) of worst_case_plane(37, 5, depth, (1, frame, distorted)).
PINNED_WORST_CASE = {
    (8, 0, 0): "7f30fb296ae15bd8",
    (8, 0, 1): "054d60ebd6d91aaa",
    (8, 3, 0): "0925f867431b6e15",
    (16, 0, 0): "561d0fc913b14e6f",
    (16, 0, 1): "a6df8a326347c9f1",
    (16, 3, 0): "814266d22e97e09a",
}


def frame(**metrics: object) -> dict[str, object]:
    """One frame of a CLI receipt."""
    return {"frameNum": 0, "metrics": dict(metrics)}


class RecordedPage(unittest.TestCase):
    def test_every_exact_twin_has_a_full_passing_row(self) -> None:
        problems = matrix.recorded_problems(PAGE.read_text(encoding="utf-8"))
        self.assertEqual(
            problems, [], "run exact_twin_matrix.py --record for: " + "; ".join(problems)
        )

    def test_every_exact_backend_is_measured_somewhere(self) -> None:
        backends = {b for twins in EXACT_TWINS.values() for b in twins}
        self.assertLessEqual(backends - set(matrix.DEVICE_BACKENDS), {"metal"})

    def test_missing_row_is_reported(self) -> None:
        text = PAGE.read_text(encoding="utf-8")
        cut = "\n".join(
            line for line in text.splitlines() if not line.startswith("| hip | `vif` |")
        )
        self.assertIn(
            "vif.hip: declared exact, no recorded matrix row", matrix.recorded_problems(cut)
        )

    def test_bad_cells_are_reported(self) -> None:
        columns = matrix.column_labels(matrix.DEPTHS, matrix.LAYOUTS)
        passing = ["="] * len(columns)
        self.assertEqual(matrix.row_problems(passing, columns), [])
        failing = ["FAIL", *passing[1:]]
        self.assertEqual(matrix.row_problems(failing, columns), ["8/420 is 'FAIL'"])
        early_na = [*passing[:3], "n/a", *passing[4:]]
        self.assertEqual(matrix.row_problems(early_na, columns), ["10/420 is 'n/a'"])
        late_na = [*passing[:-1], "n/a"]
        self.assertEqual(matrix.row_problems(late_na, columns), [])
        self.assertEqual(matrix.row_problems(passing[:-1], columns), ["11 cells, not 12"])

    def test_plan_covers_every_device_twin(self) -> None:
        plan = matrix.exact_features(matrix.DEVICE_BACKENDS, [])
        wanted = {
            (feature, backend)
            for feature, twins in EXACT_TWINS.items()
            for backend in twins
            if backend in matrix.DEVICE_BACKENDS
        }
        self.assertEqual({(f, b) for f, bs in plan.items() for b in bs}, wanted)


class Fixtures(unittest.TestCase):
    def test_geometry_exercises_strides(self) -> None:
        self.assertEqual(matrix.WIDTH % 2, 1)
        self.assertNotEqual(matrix.WIDTH % 16, 0)
        self.assertNotEqual(matrix.WIDTH % 64, 0)
        self.assertEqual(matrix.plane_size(357, 353, "420", 1), (179, 177))
        self.assertEqual(matrix.plane_size(357, 353, "422", 2), (179, 353))
        self.assertEqual(matrix.plane_size(357, 353, "444", 1), (357, 353))
        chroma = matrix.plane_size(matrix.WIDTH, matrix.HEIGHT, "420", 1)
        self.assertGreaterEqual(min(chroma), MS_SSIM_MIN_SIDE)

    def test_planes_are_pinned_bytes(self) -> None:
        for (depth, plane), digest in PINNED_PLANES.items():
            width, height = matrix.plane_size(matrix.WIDTH, matrix.HEIGHT, "420", plane)
            data = matrix.plane_bytes(width, height, depth, (plane, 1, 1, 0x5EED + depth))
            with self.subTest(depth=depth, plane=plane):
                self.assertEqual(hashlib.sha256(data).hexdigest()[:16], digest)

    def test_every_depth_reaches_both_ends(self) -> None:
        for depth in matrix.DEPTHS:
            maximum = (1 << depth) - 1
            data = matrix.plane_bytes(matrix.WIDTH, matrix.HEIGHT, depth, (0, 0, 0, 1))
            size = 1 if depth == matrix.BYTE_DEPTH else 2
            values = {
                int.from_bytes(data[i : i + size], "little") for i in range(0, len(data), size)
            }
            with self.subTest(depth=depth):
                self.assertEqual((min(values), max(values)), (0, maximum))

    def test_distorted_differs_from_reference(self) -> None:
        ref = matrix.plane_bytes(64, 64, 10, (0, 0, 0, 7))
        dis = matrix.plane_bytes(64, 64, 10, (0, 0, 1, 7))
        self.assertNotEqual(ref, dis)


class Comparison(unittest.TestCase):
    def test_equal_runs_pass(self) -> None:
        a = [frame(x=1.5, y=None, z=math.nan)]
        b = [frame(x=1.5, y=None, z=math.nan)]
        self.assertEqual(matrix.frame_differences(a, b), [])

    def test_one_ulp_is_reported(self) -> None:
        a, b = [frame(x=1.5)], [frame(x=math.nextafter(1.5, 2.0))]
        self.assertEqual(len(matrix.frame_differences(a, b)), 1)
        self.assertGreater(matrix.max_difference(a, b), 0.0)

    def test_output_on_one_side_is_reported(self) -> None:
        problems = matrix.frame_differences([frame(x=1.0)], [frame(x=1.0, y=2.0)])
        self.assertEqual(problems, ["frame 0 y: only the device run"])

    def test_frame_count_is_reported(self) -> None:
        problems = matrix.frame_differences([frame(x=1.0)], [])
        self.assertEqual(problems, ["frame count: cpu 1, device 0"])
        self.assertEqual(matrix.frame_differences([], []), ["frame count: cpu 0, device 0"])

    def test_null_against_number_is_reported(self) -> None:
        self.assertEqual(len(matrix.frame_differences([frame(x=None)], [frame(x=0.0)])), 1)


class Record(unittest.TestCase):
    def test_record_replaces_only_its_block(self) -> None:
        cell = matrix.CellResult("psnr", "cuda", 8, "420", matrix.STATUS_PASS)
        with tempfile.TemporaryDirectory() as tmp:
            page = Path(tmp) / "page.md"
            sycl = matrix.record_block("sycl", [dataclass_copy(cell, "sycl")], "kept")
            page.write_text(f"intro\n\n{sycl}\n", encoding="utf-8")
            matrix.record(page, [cell], "first")
            matrix.record(page, [dataclass_copy(cell, "cuda", matrix.STATUS_FAIL)], "second")
            text = page.read_text(encoding="utf-8")
        self.assertIn("kept: 1 of 1 cells equal", text)
        self.assertIn("second: 0 of 1 cells equal", text)
        self.assertNotIn("first", text)
        self.assertEqual(text.count("exact-twin-matrix:cuda:begin"), 1)

    def test_record_needs_a_full_run(self) -> None:
        args = matrix.parse_args(
            ["--vmaf-binary", __file__, "--backends", "cuda", "--depths", "8",
             "--record", "x.md", "--recorded-on", "y"]
        )  # fmt: skip
        self.assertIsNotNone(matrix.usage_error(args))


class LargeGrids(unittest.TestCase):
    def test_worst_case_planes_are_pinned_bytes(self) -> None:
        for (depth, frame, distorted), digest in PINNED_WORST_CASE.items():
            data = matrix.worst_case_plane(37, 5, depth, (1, frame, distorted))
            with self.subTest(depth=depth, frame=frame, distorted=distorted):
                self.assertEqual(hashlib.sha256(data).hexdigest()[:16], digest)

    def test_worst_case_frames_reach_the_extremes(self) -> None:
        for depth in matrix.LARGE_DEPTHS:
            size = 1 if depth == matrix.BYTE_DEPTH else 2
            top = b"\xff" * size

            def plane(frame: int, distorted: int, d: int = depth) -> bytes:
                return matrix.worst_case_plane(64, 4, d, (0, frame, distorted))

            with self.subTest(depth=depth):
                noise, complement = plane(0, 0), plane(0, 1)
                self.assertEqual(
                    bytes(a ^ b for a, b in zip(noise, complement, strict=True)),
                    b"\xff" * len(noise),
                )
                self.assertEqual(plane(1, 0), top * 256)
                self.assertEqual(plane(1, 1), bytes(256 * size))
                self.assertEqual(plane(2, 0), plane(1, 1))
                self.assertEqual(plane(3, 0), plane(3, 1))
                self.assertEqual(set(plane(3, 0)), {0, 255})

    def test_every_grid_needs_a_full_row(self) -> None:
        text = PAGE.read_text(encoding="utf-8")
        begin, end = matrix.markers("cuda", matrix.GRIDS["8k"])
        cut = text.split(begin, 1)[0] + text.split(end, 1)[1]
        self.assertIn(
            "adm.cuda (8k): declared exact, no recorded matrix row", matrix.recorded_problems(cut)
        )
        begin, end = matrix.markers(matrix.CPU_ROW, matrix.GRIDS["16k"])
        cut = text.split(begin, 1)[0] + text.split(end, 1)[1]
        self.assertIn(
            "adm.cpu (16k): declared exact, no recorded matrix row", matrix.recorded_problems(cut)
        )

    def test_large_refusals_are_allowed_for_listed_features_only(self) -> None:
        columns = matrix.column_labels(matrix.LARGE_DEPTHS, matrix.LARGE_LAYOUTS)
        self.assertEqual(matrix.row_problems(["n/a", "n/a"], columns, refused=True), [])
        self.assertEqual(matrix.row_problems(["n/a", "n/a"], columns), ["8/444 is 'n/a'"])
        self.assertEqual(set(matrix.SIZE_REFUSED), {"cambi"})

    def test_a_refused_picture_the_device_accepts_fails(self) -> None:
        cell = matrix.CellResult("cambi", "cuda", 8, "444", matrix.STATUS_NA, note="cpu refuses 8k")

        class AcceptingCli:
            def frames(self, fixture: object, feature: str, backend: str, extra: object = ()):
                return [frame(x=1.0)], 0, ""

        fixture = matrix.Fixture(Path("r"), Path("d"), 8, "444", matrix.GRIDS["8k"])
        result = matrix.refused_cell(AcceptingCli(), fixture, cell)  # type: ignore[arg-type]
        self.assertEqual(result.status, matrix.STATUS_FAIL)

    def test_recording_a_large_grid_keeps_the_small_one(self) -> None:
        cell = matrix.CellResult("psnr", "cuda", 8, "444", matrix.STATUS_PASS)
        with tempfile.TemporaryDirectory() as tmp:
            page = Path(tmp) / "page.md"
            small = matrix.record_block("cuda", [cell], "small")
            page.write_text(f"intro\n\n{small}\n", encoding="utf-8")
            matrix.record(page, [cell], "large", matrix.GRIDS["8k"])
            text = page.read_text(encoding="utf-8")
        self.assertIn("small: 1 of 1 cells equal", text)
        self.assertIn("large: 1 of 1 cells equal", text)
        self.assertIn("exact-twin-matrix-8k:cuda:begin", text)


def dataclass_copy(
    cell: matrix.CellResult, backend: str, status: str = matrix.STATUS_PASS
) -> matrix.CellResult:
    """`cell` on another backend and with another status."""
    return matrix.CellResult(cell.feature, backend, cell.depth, cell.layout, status)


if __name__ == "__main__":
    unittest.main()

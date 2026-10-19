#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bit-depth x chroma-layout matrix of every exact GPU twin, `==` against the CPU.

Every (feature, backend) pair declared exact in ``scripts/ci/exact_twins.d/``
is run on generated fixtures at 8, 10, 12 and 16 bits and in 4:2:0, 4:2:2 and
4:4:4, once with ``--backend cpu`` and once on the device, both at
``--precision max``. A cell passes when both runs have the same frames, the
same output keys and the same bits in every value; there is no tolerance.

The fixtures are written by this script from integer arithmetic only, so they
are the same bytes on every host. They are 357x353 (odd, not a multiple of 16
or 64, so every plane row ends in a partial tile and a device pitch differs
from the width) and four frames long. Each plane holds a moving ramp, blocks
inverted by plane, hashed noise, and runs clipped to 0 and to the depth's
maximum; the distorted frames add their own noise and a shifted window.

A depth the CPU extractor refuses is ``n/a`` (only 16 bits may be refused);
at 8, 10 and 12 bits a CPU failure is an error. Usage::

    exact_twin_matrix.py --vmaf-binary build/tools/vmaf --backends cuda \\
        [--grid standard|8k|16k] [--depths 8 10 12 16] [--layouts 420 422 444] \\
        [--features ...] [--json-out m.json] [--md-out m.md] \\
        [--record docs/development/exact-twin-matrix.md --recorded-on TEXT]

``--grid 8k`` runs every device twin on 8192x4320 4:4:4 pictures at 8 and 16
bits, and ``--grid 16k`` runs the CPU extractor of every exact twin on
15360x8640 4:4:4 pictures with the host's SIMD dispatch against
``--cpumask 0xffffffff`` (scalar code only). Both use worst-case content
(``worst_case_plane()``): full-range noise against its complement, a whole
frame at the maximum difference each way, and a 1-pixel checkerboard
compared with itself. A CPU extractor listed in ``SIZE_REFUSED`` may refuse
a large picture; its cell is ``n/a`` only when the device refuses it too.

``--record`` replaces each run backend's table in the page (between its
``exact-twin-matrix:<backend>`` markers) and needs a full run: every exact twin
of the backend at every depth and layout. ``recorded_problems()`` is the check
the device-free contract test runs on that page: every exact twin of a device
backend must have a recorded row with every cell equal, or ``n/a`` at 16 bits.

Device runs take ``<lock-dir>/<backend lock>`` (default ``~/.cache/vmafx-locks``)
with a 300 s limit inside the lock when the directory exists. Exit 0 when every
cell passes or is ``n/a``, 1 otherwise, 2 on a usage error, 77 when the CLI
refuses a backend (no device).
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import math
import os
import shutil
import sys
import tempfile
from array import array
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.cross_backend_calibration import EXACT_TWINS, metric_delta
from scripts.ci.cross_backend_parity_gate import build_command
from scripts.lib.safe_subprocess import run as run_command

WIDTH = 357
HEIGHT = 353
FRAMES = 4
DEPTHS = (8, 10, 12, 16)
LAYOUTS = ("420", "422", "444")
# A CPU extractor may refuse this depth; at every other depth a refusal is an error.
OPTIONAL_DEPTH = 16
SKIP = 77
EXIT_BACKEND_REFUSED = 100
DEVICE_RUN_LIMIT_S = 300
WAIT_LIMIT_S = 3600
LOCK_FILE = {"cuda": "cuda-4090.lock", "sycl": "sycl-a380.lock", "hip": "hip-gfx1036.lock"}
DEVICE_BACKENDS = tuple(sorted(LOCK_FILE))
MASK32 = 0xFFFFFFFF
BYTE_DEPTH = 8
# The window the distorted picture moves: x0, x1, y0, y1.
MOVED_WINDOW = (61, 102, 37, 66)
NOTE_PROBLEMS = 4
MIN_ROW_PARTS = 3
STATUS_PASS = "="  # noqa: S105 - a table mark, not a credential
STATUS_FAIL = "FAIL"
STATUS_ERROR = "ERROR"
STATUS_NA = "n/a"

# The large grids: worst-case content at 8K (every device twin against the CPU)
# and at 16K (the CPU's SIMD dispatch against its scalar code). See
# docs/development/accumulator-bounds.md.
LARGE_DEPTHS = (8, 16)
LARGE_LAYOUTS = ("444",)
LARGE_FRAMES = 4
LARGE_RUN_LIMIT_S = 1800
CPU_SCALAR_MASK = "0xffffffff"
# CPU extractors that refuse a large picture at init. A large cell of one of
# these is n/a when the device twin refuses the picture too.
SIZE_REFUSED = {
    "cambi": "the window adjusted to the picture exceeds 65, the reciprocal LUT (cambi.h)",
}


@dataclasses.dataclass(frozen=True)
class Grid:
    """One picture geometry and its cells: depths x layouts, recorded between markers."""

    name: str
    width: int
    height: int
    frames: int
    depths: tuple[int, ...]
    layouts: tuple[str, ...]
    marker: str


GRIDS = {
    "standard": Grid("standard", WIDTH, HEIGHT, FRAMES, DEPTHS, LAYOUTS, "exact-twin-matrix"),
    "8k": Grid("8k", 8192, 4320, LARGE_FRAMES, LARGE_DEPTHS, LARGE_LAYOUTS, "exact-twin-matrix-8k"),
    "16k": Grid(
        "16k", 15360, 8640, LARGE_FRAMES, LARGE_DEPTHS, LARGE_LAYOUTS, "exact-twin-matrix-16k"
    ),
}
STANDARD = GRIDS["standard"]
# The 16K grid compares two CPU runs; its rows carry this backend name.
CPU_ROW = "cpu"


# ---------------------------------------------------------------------------
# Fixtures: integer arithmetic only, little-endian 16-bit words above 8 bits.
# ---------------------------------------------------------------------------


def plane_size(width: int, height: int, layout: str, plane: int) -> tuple[int, int]:
    """Plane `plane` of a `layout` picture, chroma ceil-subsampled as vmaf_picture_alloc()."""
    if plane == 0 or layout == "444":
        return width, height
    if layout == "422":
        return (width + 1) >> 1, height
    return (width + 1) >> 1, (height + 1) >> 1


def mix(value: int) -> int:
    """A 32-bit integer hash (the finaliser of MurmurHash3)."""
    value &= MASK32
    value ^= value >> 16
    value = (value * 0x85EBCA6B) & MASK32
    value ^= value >> 13
    value = (value * 0xC2B2AE35) & MASK32
    return value ^ (value >> 16)


def sample(x: int, y: int, key: tuple[int, int, int, int], maximum: int) -> int:
    """One sample: ramp moving 3 px per frame, inverted blocks, noise, clipped runs.

    `key` is (plane, frame, distorted, seed): the distorted picture adds its own
    noise and moves a 41x29 window by 5 px.
    """
    plane, frame, distorted, seed = key
    x0, x1, y0, y1 = MOVED_WINDOW
    shift = 3 * frame + (5 if distorted and x0 <= x < x1 and y0 <= y < y1 else 0)
    ramp = ((x + shift) * 7 + y * 5) * maximum // (7 * (WIDTH + 16) + 5 * HEIGHT)
    if ((x + shift) // 11 + y // 9 + plane) % 3 == 0:
        ramp = maximum - ramp
    noise = mix(x * 0x9E3779B1 ^ y * 0x85EBCA77 ^ (seed << 8) ^ (plane << 4) ^ frame)
    amplitude = max(maximum >> (5 if distorted else 6), 1)
    value = ramp + noise % (2 * amplitude + 1) - amplitude
    if (y // 17 + x // 23 + frame) % 7 == 0:
        value += maximum // 3 if (x + y) % 2 else -maximum // 3
    return min(max(value, 0), maximum)


def plane_bytes(width: int, height: int, depth: int, key: tuple[int, int, int, int]) -> bytes:
    """One plane, row after row; 16-bit little-endian words above 8 bits."""
    maximum = (1 << depth) - 1
    values = array("B" if depth == BYTE_DEPTH else "H")
    for y in range(height):
        values.extend(sample(x, y, key, maximum) for x in range(width))
    if values.itemsize > 1 and sys.byteorder != "little":
        values.byteswap()
    return values.tobytes()


def write_fixture(path: Path, depth: int, layout: str, distorted: bool) -> None:
    """Write FRAMES frames of the WIDTH x HEIGHT fixture of `depth` and `layout`."""
    with path.open("wb") as out:
        for frame in range(FRAMES):
            for plane in range(3):
                width, height = plane_size(WIDTH, HEIGHT, layout, plane)
                key = (plane, frame, int(distorted), 0x5EED + depth)
                out.write(plane_bytes(width, height, depth, key))


# Worst-case content of the large grids, one kind per frame: full-range noise
# against its complement, the reference at the maximum and the distorted
# picture at 0, the other way round, and a 1-pixel checkerboard of 0 and the
# maximum compared with itself. Frames 1 to 2 move every sample from the
# maximum to 0 (the largest motion SAD); frame 3 has the most reference
# detail with no distortion (the largest ADM masking and VIF terms).
WORST_CASE_KINDS = ("noise", "ref-max", "ref-zero", "checkerboard")


def checkerboard_row(width: int, sample_bytes: int, phase: int) -> bytes:
    """One row alternating 0 and the maximum, starting with the maximum when `phase` is odd."""
    pair = b"\x00" * sample_bytes + b"\xff" * sample_bytes
    if phase % 2:
        pair = pair[sample_bytes:] + pair[:sample_bytes]
    return (pair * (width // 2 + 1))[: width * sample_bytes]


def worst_case_plane(width: int, height: int, depth: int, key: tuple[int, int, int]) -> bytes:
    """One plane of worst-case content; `key` is (plane, frame, distorted).

    Every value is 0, the maximum or SHAKE256 noise, so the bytes are the
    same on every host. 16-bit words are little-endian; above 8 bits only 16
    is used, where every byte pattern is a valid sample.
    """
    plane, frame, distorted = key
    sample_bytes = 1 if depth == BYTE_DEPTH else 2
    size = width * height * sample_bytes
    kind = WORST_CASE_KINDS[frame % len(WORST_CASE_KINDS)]
    if kind == "noise":
        seed = f"vmafx-worst-case/{width}x{height}/{depth}/{plane}/{frame}".encode()
        noise = hashlib.shake_256(seed).digest(size)
        return noise.translate(COMPLEMENT) if distorted else noise
    if kind in ("ref-max", "ref-zero"):
        high = (kind == "ref-max") != bool(distorted)
        return b"\xff" * size if high else bytes(size)
    rows = (
        checkerboard_row(width, sample_bytes, plane),
        checkerboard_row(width, sample_bytes, plane + 1),
    )
    return (rows[0] + rows[1]) * (height // 2) + (rows[0] if height % 2 else b"")


# Byte complement: 255 - b, which complements a 16-bit word too.
COMPLEMENT = bytes(range(255, -1, -1))


def write_worst_case_fixture(path: Path, grid: Grid, depth: int, distorted: bool) -> None:
    """Write the worst-case frames of `grid` (4:4:4) at `depth`."""
    with path.open("wb") as out:
        for frame in range(grid.frames):
            for plane in range(3):
                out.write(
                    worst_case_plane(grid.width, grid.height, depth, (plane, frame, int(distorted)))
                )


@dataclasses.dataclass(frozen=True)
class Fixture:
    """The reference and distorted files of one (grid, depth, layout)."""

    ref: Path
    dis: Path
    depth: int
    layout: str
    grid: Grid = STANDARD


def make_fixture(workdir: Path, depth: int, layout: str, grid: Grid = STANDARD) -> Fixture:
    """Generate (once) the pair of one (depth, layout) of `grid` under `workdir`."""
    stem = workdir / f"matrix_{grid.width}x{grid.height}_{layout}p{depth}"
    fixture = Fixture(Path(f"{stem}_ref.yuv"), Path(f"{stem}_dis.yuv"), depth, layout, grid)
    for path, distorted in ((fixture.ref, False), (fixture.dis, True)):
        if path.is_file():
            continue
        if grid is STANDARD:
            write_fixture(path, depth, layout, distorted)
        else:
            write_worst_case_fixture(path, grid, depth, distorted)
    return fixture


# ---------------------------------------------------------------------------
# Comparison of two CLI receipts.
# ---------------------------------------------------------------------------


def same_value(a: Any, b: Any) -> bool:
    """The same bits: equal numbers, both null (a non-finite score), or two NaNs."""
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return type(a) is type(b) and a == b


def frame_differences(cpu: list[dict[str, Any]], dev: list[dict[str, Any]]) -> list[str]:
    """Every output of every frame that the two runs do not share, as text."""
    if len(cpu) != len(dev) or not cpu:
        return [f"frame count: cpu {len(cpu)}, device {len(dev)}"]
    problems: list[str] = []
    for index, (a, b) in enumerate(zip(cpu, dev, strict=True)):
        ma, mb = a.get("metrics", {}), b.get("metrics", {})
        for key in sorted(set(ma) ^ set(mb)):
            problems.append(f"frame {index} {key}: only the {'cpu' if key in ma else 'device'} run")
        for key in sorted(set(ma) & set(mb)):
            if not same_value(ma[key], mb[key]):
                problems.append(f"frame {index} {key}: cpu {ma[key]!r} device {mb[key]!r}")
    return problems


def max_difference(cpu: list[dict[str, Any]], dev: list[dict[str, Any]]) -> float:
    """Largest absolute difference over the outputs both runs have."""
    worst = 0.0
    for a, b in zip(cpu, dev, strict=False):
        ma, mb = a.get("metrics", {}), b.get("metrics", {})
        for key in set(ma) & set(mb):
            worst = max(worst, metric_delta(ma[key], mb[key]))
    return worst


# ---------------------------------------------------------------------------
# Running the CLI.
# ---------------------------------------------------------------------------


def lock_prefix(backend: str, lock_dir: Path) -> tuple[str, ...]:
    """`flock <lock> timeout 300` when the lock directory exists, else `timeout 300`."""
    flock, timeout = shutil.which("flock"), shutil.which("timeout")
    limit = (timeout, str(DEVICE_RUN_LIMIT_S)) if timeout else ()
    if backend in LOCK_FILE and flock and lock_dir.is_dir():
        return (flock, str(lock_dir / LOCK_FILE[backend]), *limit)
    return limit


@dataclasses.dataclass(frozen=True)
class Cli:
    """The vmaf binary, the working directory and the lock directory."""

    binary: Path
    workdir: Path
    lock_dir: Path

    def frames(
        self, fixture: Fixture, feature: str, backend: str, extra: Sequence[str] = ()
    ) -> tuple[list[dict[str, Any]] | None, int, str]:
        """(frames, exit status, message) of one run; frames is None on a failure.

        `extra` is appended to the command line (the 16K grid's scalar run).
        """
        grid = fixture.grid
        tag = "_scalar" if extra else ""
        out = (
            self.workdir
            / f"{feature}_{grid.name}_{fixture.layout}p{fixture.depth}_{backend}{tag}.json"
        )
        geometry = (grid.width, grid.height, fixture.layout, fixture.depth)
        argv = build_command(
            self.binary, fixture.ref, fixture.dis, *geometry, feature, backend, None, out, "max"
        )
        argv += list(extra)
        device = backend != "cpu"
        full = [*lock_prefix(backend, self.lock_dir), *argv] if device else argv
        cpu_limit = DEVICE_RUN_LIMIT_S if grid is STANDARD else LARGE_RUN_LIMIT_S
        proc = run_command(
            full,
            allowed_executables=(full[0],),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=WAIT_LIMIT_S if device else cpu_limit,
            max_output_bytes=4 * 1_048_576,
        )
        if proc.returncode != 0:
            return None, proc.returncode, (proc.stderr or proc.stdout).strip()[-300:]
        payload = json.loads(out.read_text(encoding="utf-8"))
        return list(payload.get("frames") or []), 0, ""


@dataclasses.dataclass
class CellResult:
    """One (feature, backend, depth, layout) cell."""

    feature: str
    backend: str
    depth: int
    layout: str
    status: str
    frames: int = 0
    values: int = 0
    max_abs_diff: float = 0.0
    note: str = ""


class BackendRefused(Exception):
    """The CLI refused a device backend (exit 100): no device on this host."""


def cpu_cell_status(
    fixture: Fixture, code: int, message: str, feature: str = ""
) -> tuple[str, str]:
    """Status and note of a cell whose CPU run failed.

    The CPU may refuse 16 bits, and a large picture when `feature` is in
    SIZE_REFUSED; any other refusal is an error.
    """
    if fixture.grid is not STANDARD and feature in SIZE_REFUSED:
        return STATUS_NA, f"cpu refuses {fixture.grid.name}: {SIZE_REFUSED[feature]}"
    if fixture.depth == OPTIONAL_DEPTH:
        return STATUS_NA, f"cpu refuses {fixture.depth}-bit: {message}"
    return STATUS_ERROR, f"cpu exited {code}: {message}"


def compare_cell(
    base: CellResult, cpu: list[dict[str, Any]], dev: list[dict[str, Any]]
) -> CellResult:
    """Fill `base` from the two runs' frames."""
    problems = frame_differences(cpu, dev)
    values = sum(len(frame.get("metrics", {})) for frame in cpu)
    return dataclasses.replace(
        base,
        status=STATUS_FAIL if problems else STATUS_PASS,
        frames=len(cpu),
        values=values,
        max_abs_diff=max_difference(cpu, dev),
        note="; ".join(problems[:NOTE_PROBLEMS])
        + (f"; {len(problems)} in all" if len(problems) > NOTE_PROBLEMS else ""),
    )


def second_run(
    cli: Cli, fixture: Fixture, feature: str, backend: str
) -> tuple[list[dict[str, Any]] | None, int, str]:
    """The run compared with the CPU: the device, or the CPU's scalar code (row `cpu`)."""
    if backend == CPU_ROW:
        return cli.frames(fixture, feature, "cpu", ("--cpumask", CPU_SCALAR_MASK))
    frames, code, message = cli.frames(fixture, feature, backend)
    if code == EXIT_BACKEND_REFUSED:
        raise BackendRefused(f"--backend {backend}: {message}")
    return frames, code, message


def refused_cell(cli: Cli, fixture: Fixture, cell: CellResult) -> CellResult:
    """A large cell the CPU refused: n/a while the other run refuses the picture too."""
    frames, _, _ = second_run(cli, fixture, cell.feature, cell.backend)
    if frames is None:
        return cell
    return dataclasses.replace(
        cell, status=STATUS_FAIL, note=f"{cell.backend} accepts a picture the cpu refuses"
    )


def run_cell(
    cli: Cli, fixture: Fixture, feature: str, backend: str, cpu: tuple[Any, int, str]
) -> CellResult:
    """One cell, given the CPU run of its (feature, fixture)."""
    base = CellResult(feature, backend, fixture.depth, fixture.layout, STATUS_ERROR)
    cpu_frames, cpu_code, cpu_message = cpu
    if cpu_frames is None:
        status, note = cpu_cell_status(fixture, cpu_code, cpu_message, feature)
        cell = dataclasses.replace(base, status=status, note=note)
        large_na = status == STATUS_NA and fixture.grid is not STANDARD
        return refused_cell(cli, fixture, cell) if large_na else cell
    dev_frames, code, message = second_run(cli, fixture, feature, backend)
    if dev_frames is None:
        return dataclasses.replace(base, note=f"{backend} exited {code}: {message}")
    return compare_cell(base, cpu_frames, dev_frames)


def exact_features(backends: Iterable[str], only: Sequence[str]) -> dict[str, list[str]]:
    """feature -> the selected backends that declare it exact (exact_twins.d)."""
    chosen = set(backends)
    plan: dict[str, list[str]] = {}
    for feature in sorted(EXACT_TWINS):
        listed = sorted(b for b in EXACT_TWINS[feature] if b in chosen)
        if listed and (not only or feature in only):
            plan[feature] = listed
    return plan


def grid_plan(grid: Grid, backends: Iterable[str], only: Sequence[str]) -> dict[str, list[str]]:
    """The plan of `grid`: the device twins, or for 16K the CPU row of every exact feature."""
    if grid.name == "16k":
        return {feature: [CPU_ROW] for feature in exact_features(DEVICE_BACKENDS, only)}
    return exact_features(backends, only)


def run_matrix(
    cli: Cli,
    plan: dict[str, list[str]],
    depths: Sequence[int],
    layouts: Sequence[str],
    grid: Grid = STANDARD,
) -> list[CellResult]:
    """Every cell of `plan` over `depths` x `layouts` of `grid`, printed as it completes."""
    results: list[CellResult] = []
    for depth in depths:
        for layout in layouts:
            fixture = make_fixture(cli.workdir, depth, layout, grid)
            for feature, backends in plan.items():
                cpu = cli.frames(fixture, feature, "cpu")
                for backend in backends:
                    cell = run_cell(cli, fixture, feature, backend, cpu)
                    print_cell(cell)
                    results.append(cell)
    return results


# ---------------------------------------------------------------------------
# Output.
# ---------------------------------------------------------------------------


def print_cell(cell: CellResult) -> None:
    """One line per cell on stdout."""
    print(
        f"{cell.backend:<5} {cell.feature:<18} {cell.depth:>2}-bit {cell.layout}  "
        f"{cell.status:<5} frames={cell.frames} values={cell.values} "
        f"max_abs_diff={cell.max_abs_diff:.3e}" + (f"  {cell.note}" if cell.note else ""),
        flush=True,
    )


def column_labels(depths: Sequence[int], layouts: Sequence[str]) -> list[str]:
    """`8/420`, `8/422`, ... in depth-major order."""
    return [f"{depth}/{layout}" for depth in depths for layout in layouts]


def markdown_table(results: list[CellResult], depths: Sequence[int], layouts: Sequence[str]) -> str:
    """One row per (backend, feature), one column per depth/layout cell."""
    columns = column_labels(depths, layouts)
    cells = {(r.backend, r.feature, f"{r.depth}/{r.layout}"): r.status for r in results}
    rows = sorted({(r.backend, r.feature) for r in results})
    lines = [
        "| backend | feature | " + " | ".join(columns) + " |",
        "|---|---|" + "---|" * len(columns),
    ]
    for backend, feature in rows:
        marks = [cells.get((backend, feature, column), "-") for column in columns]
        lines.append(f"| {backend} | `{feature}` | " + " | ".join(marks) + " |")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# The recorded page: one table per device backend between markers.
# ---------------------------------------------------------------------------


def markers(backend: str, grid: Grid = STANDARD) -> tuple[str, str]:
    """The begin and end markers of the block of `backend` in `grid`."""
    return (f"<!-- {grid.marker}:{backend}:begin -->", f"<!-- {grid.marker}:{backend}:end -->")


def record_block(
    backend: str, results: list[CellResult], recorded_on: str, grid: Grid = STANDARD
) -> str:
    """The marked block of `backend`: the provenance line, then its table."""
    rows = [r for r in results if r.backend == backend]
    equal = sum(r.status == STATUS_PASS for r in rows)
    absent = sum(r.status == STATUS_NA for r in rows)
    counts = f"{equal} of {len(rows)} cells equal, {absent} `n/a`."
    table = markdown_table(rows, grid.depths, grid.layouts)
    begin, end = markers(backend, grid)
    return f"{begin}\n\n{recorded_on}: {counts}\n\n{table}\n{end}"


def record(page: Path, results: list[CellResult], recorded_on: str, grid: Grid = STANDARD) -> None:
    """Replace (or append) the block of every backend `results` covers in `grid`."""
    text = page.read_text(encoding="utf-8")
    for backend in sorted({r.backend for r in results}):
        begin, end = markers(backend, grid)
        block = record_block(backend, results, recorded_on, grid)
        if begin in text and end in text:
            head, rest = text.split(begin, 1)
            text = head + block + rest.split(end, 1)[1]
        else:
            text = text.rstrip("\n") + "\n\n" + block + "\n"
    page.write_text(text, encoding="utf-8")


def recorded_rows(text: str, grid: Grid = STANDARD) -> dict[tuple[str, str], list[str]]:
    """(backend, feature) -> the cell marks of its row, from the blocks of `grid`."""
    rows: dict[tuple[str, str], list[str]] = {}
    for backend in (*DEVICE_BACKENDS, CPU_ROW):
        begin, end = markers(backend, grid)
        if begin not in text or end not in text:
            continue
        for line in text.split(begin, 1)[1].split(end, 1)[0].splitlines():
            parts = [part.strip() for part in line.strip().strip("|").split("|")]
            if len(parts) < MIN_ROW_PARTS or parts[0] != backend or not parts[1].startswith("`"):
                continue
            rows[(parts[0], parts[1].strip("`"))] = parts[2:]
    return rows


def row_problems(marks: list[str], columns: list[str], refused: bool = False) -> list[str]:
    """Why a recorded row is not a full, passing row.

    `n/a` is allowed at 16 bits, and in every cell when `refused` (a large
    grid of a SIZE_REFUSED feature).
    """
    if len(marks) != len(columns):
        return [f"{len(marks)} cells, not {len(columns)}"]
    bad = []
    for column, mark in zip(columns, marks, strict=True):
        optional = mark == STATUS_NA and (refused or column.startswith(f"{OPTIONAL_DEPTH}/"))
        if mark != STATUS_PASS and not optional:
            bad.append(f"{column} is {mark!r}")
    return bad


def grid_pairs(grid: Grid, twins: dict[str, frozenset[str]]) -> list[tuple[str, str]]:
    """(feature, row backend) pairs `grid` must record: every device twin, or for 16K
    the CPU row of every feature with an exact device twin."""
    pairs = [
        (feature, backend)
        for feature in sorted(twins)
        for backend in sorted(set(twins[feature]) & set(DEVICE_BACKENDS))
    ]
    if grid.name == "16k":
        return sorted({(feature, CPU_ROW) for feature, _ in pairs})
    return pairs


def grid_problems(text: str, grid: Grid, twins: dict[str, frozenset[str]]) -> list[str]:
    """The problems of one grid's recorded rows."""
    rows = recorded_rows(text, grid)
    columns = column_labels(grid.depths, grid.layouts)
    where = "" if grid is STANDARD else f" ({grid.name})"
    problems: list[str] = []
    for feature, backend in grid_pairs(grid, twins):
        marks = rows.get((backend, feature))
        if marks is None:
            problems.append(f"{feature}.{backend}{where}: declared exact, no recorded matrix row")
            continue
        refused = grid is not STANDARD and feature in SIZE_REFUSED
        problems += [
            f"{feature}.{backend}{where}: {p}" for p in row_problems(marks, columns, refused)
        ]
    return problems


def recorded_problems(text: str, twins: dict[str, frozenset[str]] = EXACT_TWINS) -> list[str]:
    """Every exact twin the page does not record as equal, in every grid."""
    problems: list[str] = []
    for grid in GRIDS.values():
        problems += grid_problems(text, grid, twins)
    return problems


def write_outputs(args: argparse.Namespace, results: list[CellResult]) -> None:
    """The JSON and Markdown files requested on the command line."""
    grid = GRIDS[args.grid]
    if args.json_out is not None:
        payload = {
            "schema_version": 1,
            "grid": grid.name,
            "geometry": [grid.width, grid.height, grid.frames],
            "cells": [dataclasses.asdict(r) for r in results],
        }
        args.json_out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    if args.md_out is not None:
        args.md_out.write_text(markdown_table(results, args.depths, args.layouts))
    if args.record is not None:
        record(args.record, results, args.recorded_on, grid)


def summary(results: list[CellResult]) -> int:
    """Print the counts; 0 when no cell failed."""
    counts = {s: sum(r.status == s for r in results) for s in (STATUS_PASS, STATUS_NA)}
    failed = [r for r in results if r.status not in (STATUS_PASS, STATUS_NA)]
    print(
        f"exact-twin matrix: {len(results)} cells, {counts[STATUS_PASS]} equal, "
        f"{counts[STATUS_NA]} n/a (cpu refuses the depth or size), {len(failed)} failed"
    )
    for cell in failed:
        print(f"  FAILED {cell.backend} {cell.feature} {cell.depth}/{cell.layout}: {cell.note}")
    return 1 if failed else 0


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vmaf-binary", type=Path, required=True)
    parser.add_argument("--grid", choices=sorted(GRIDS), default=STANDARD.name)
    parser.add_argument("--backends", nargs="+", choices=DEVICE_BACKENDS, default=[])
    parser.add_argument("--depths", nargs="+", type=int, choices=DEPTHS, default=None)
    parser.add_argument("--layouts", nargs="+", choices=LAYOUTS, default=None)
    parser.add_argument("--features", nargs="*", default=[])
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--lock-dir", type=Path, default=None)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--md-out", type=Path, default=None)
    parser.add_argument("--record", type=Path, default=None)
    parser.add_argument("--recorded-on", default="")
    args = parser.parse_args(argv)
    grid = GRIDS[args.grid]
    args.depths = args.depths or list(grid.depths)
    args.layouts = args.layouts or list(grid.layouts)
    return args


def usage_error(args: argparse.Namespace) -> str | None:
    """Why the arguments cannot run, or None."""
    grid = GRIDS[args.grid]
    if not args.vmaf_binary.is_file():
        return f"vmaf binary not found: {args.vmaf_binary}"
    if grid.name != "16k" and not args.backends:
        return f"--grid {grid.name} needs --backends"
    if not set(args.depths) <= set(grid.depths) or not set(args.layouts) <= set(grid.layouts):
        return f"--grid {grid.name} has depths {grid.depths} and layouts {grid.layouts}"
    if args.record is None:
        return None
    full = not args.features and set(args.depths) == set(grid.depths)
    if not full or set(args.layouts) != set(grid.layouts) or not args.recorded_on:
        return "--record needs every depth, layout and feature, and --recorded-on"
    return None


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    error = usage_error(args)
    if error is not None:
        sys.stderr.write(f"{error}\n")
        return 2
    grid = GRIDS[args.grid]
    plan = grid_plan(grid, args.backends, args.features)
    if not plan:
        sys.stderr.write("no exact twin matches --backends / --features\n")
        return 2
    default_locks = Path(os.environ.get("VMAFX_LOCK_DIR", Path.home() / ".cache" / "vmafx-locks"))
    with tempfile.TemporaryDirectory(prefix="exact-matrix-") as tmp:
        workdir = args.workdir or Path(tmp)
        workdir.mkdir(parents=True, exist_ok=True)
        cli = Cli(args.vmaf_binary.resolve(), workdir, args.lock_dir or default_locks)
        try:
            results = run_matrix(cli, plan, args.depths, args.layouts, grid)
        except BackendRefused as refused:
            sys.stderr.write(f"{refused}; skipping ({SKIP})\n")
            return SKIP
    write_outputs(args, results)
    return summary(results)


if __name__ == "__main__":
    sys.exit(main())

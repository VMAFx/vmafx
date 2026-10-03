# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Metal twins against the CPU extractors, on a Mac (the macOS tester bundle).

Every CPU extractor runs on each fixture with `--backend metal` at `--precision max`;
the scores are compared with `==` against the default CPU dispatch. The vmaf JSON
names the backend each extractor ran on, so a twin that fell back to the CPU is not
counted as a Metal result. A host without a usable Metal device is `no_device`:
not exercised, not a failure (vmaf exits 100, VMAF_EXIT_BACKEND_INIT_FAILED).
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .hw_equiv import FixtureRunError, Runner, Scores, compare_scores, run_fixture_meta
from .safe_process import run_bounded

BACKEND_INIT_FAILED = 100
# Per fixture id: the Metal run's scores and the extractors that ran on Metal,
# which the state-row evaluation reads (hw_rows.py, ADR-1496).
MetalRuns = dict[str, dict[str, Any]]


def split_backends(meta: Mapping[str, Any]) -> tuple[list[str], list[str]]:
    """(extractors that ran on Metal, extractors that ran elsewhere), sorted."""
    on_metal: set[str] = set()
    elsewhere: set[str] = set()
    for entry in meta.get("feature_backends", []):
        target = on_metal if entry.get("backend") == "metal" else elsewhere
        target.add(str(entry.get("extractor")))
    return sorted(on_metal), sorted(elsewhere - on_metal)


def metal_cell(
    vmaf: str,
    fixture: Mapping[str, Any],
    cpu: Scores,
    *,
    timeout_seconds: float,
    runner: Runner,
) -> dict[str, Any]:
    """One fixture: Metal run compared with the CPU scores."""
    cell: dict[str, Any] = {"fixture": fixture["id"]}
    try:
        scores, meta = run_fixture_meta(
            vmaf, fixture, None, timeout_seconds=timeout_seconds, runner=runner, backend="metal"
        )
    except FixtureRunError as error:
        cell["error"] = str(error)
        cell["no_device"] = error.returncode == BACKEND_INIT_FAILED
        return cell
    except (TimeoutError, RuntimeError, ValueError) as error:
        cell["error"] = str(error)
        return cell
    on_metal, elsewhere = split_backends(meta)
    if not on_metal:
        cell["error"] = "no extractor ran on Metal (silent CPU fallback)"
        return cell
    cell.update(compare_scores(cpu, scores))
    cell["extractors_on_metal"] = on_metal
    cell["extractors_on_cpu"] = elsewhere
    cell["scores"] = scores
    return cell


def run_metal_equivalence_raw(
    vmaf: str,
    fixtures: Sequence[Mapping[str, Any]],
    cpu_scores: Mapping[str, Scores],
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> tuple[dict[str, Any], MetalRuns]:
    """Metal against CPU on every fixture, and the Metal runs themselves."""
    cells = [
        metal_cell(
            vmaf, fixture, cpu_scores[str(fixture["id"])],
            timeout_seconds=timeout_seconds, runner=runner,
        )
        for fixture in fixtures
        if str(fixture["id"]) in cpu_scores
    ]  # fmt: skip
    runs = {
        str(cell["fixture"]): {
            "scores": cell["scores"],
            "on_metal": cell["extractors_on_metal"],
        }
        for cell in cells
        if "scores" in cell
    }
    section = {"status": status_of_cells(cells), "fixtures": [_public(c) for c in cells]}
    return section, runs


def run_metal_equivalence(
    vmaf: str,
    fixtures: Sequence[Mapping[str, Any]],
    cpu_scores: Mapping[str, Scores],
    *,
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> dict[str, Any]:
    """Metal against CPU on every fixture; `no_device` when no run could start Metal."""
    return run_metal_equivalence_raw(
        vmaf, fixtures, cpu_scores, timeout_seconds=timeout_seconds, runner=runner
    )[0]


def _public(cell: Mapping[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in cell.items() if key not in ("no_device", "scores")}


def status_of_cells(cells: Sequence[Mapping[str, Any]]) -> str:
    """`no_device` (all runs), `error`, `differing`, `identical`; no cells is `error`."""
    if not cells:
        return "error"
    if all(cell.get("no_device") for cell in cells):
        return "no_device"
    if any("error" in cell for cell in cells):
        return "error"
    return "differing" if any(c["differing_values"] for c in cells) else "identical"

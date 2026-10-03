#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every registered CUDA, SYCL, HIP and Metal twin is a cell of the parity gate (ADR-1460).

The cross-backend parity gate compares a GPU twin with its CPU extractor for
the features in ``FEATURE_METRICS``. A twin whose feature is not in that table
is registered, selectable with ``--backend`` and guarded by nothing but its own
unit test: ``speed_temporal_cuda``, ``speed_temporal_hip`` and
``speed_temporal_sycl`` were in that position.

This test reads the extractor registry
(``core/src/feature/feature_extractor.cpp``) and the name each registered
extractor carries, and requires every twin of a backend the gate runs to be
the extractor of some gate feature. A twin of a backend the gate does not run
is counted and has to be on the list below with its state row, so that a new
backend cannot be left out without a record. Metal was on that list until
ADR-1496 gave the gate a ``metal`` backend, which the macOS tester bundle runs
on an Apple device.

Device-free: reads the sources only.
"""

from __future__ import annotations

import ast
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
REGISTRY = FEATURE_ROOT / "feature_extractor.cpp"
GATE = ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py"
SINGLE_FEATURE_GATE = ROOT / "scripts" / "ci" / "cross_backend_vif_diff.py"

GPU_BACKENDS = ("cuda", "sycl", "hip", "metal")
# Backends with registered twins that the gate cannot run, and where that is
# recorded. Empty since ADR-1496 (`metal`).
UNGATED_BACKENDS: dict[str, str] = {}

SOURCE_SUFFIXES = (".c", ".cpp", ".mm")
DEFINITION = re.compile(
    r"VmafFeatureExtractor\s+(vmaf_fex_\w+)\s*=\s*\{.*?\.name\s*=\s*\"([^\"]+)\"", re.S
)
REGISTERED = re.compile(r"&(vmaf_fex_\w+)")


def _literal(path: Path, name: str) -> object:
    """A module-level literal of the gate, read without importing it."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        target = node.target if isinstance(node, ast.AnnAssign) else None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name and node.value is not None:
            return ast.literal_eval(node.value)
    raise AssertionError(f"{path.name}: no {name} literal")


def _extractor_names() -> dict[str, str]:
    """Registry symbol -> the name the extractor is selected by."""
    names: dict[str, str] = {}
    for path in sorted(FEATURE_ROOT.rglob("*")):
        if path.suffix in SOURCE_SUFFIXES:
            names.update(DEFINITION.findall(path.read_text(encoding="utf-8", errors="replace")))
    return names


def registered_twins() -> dict[str, set[str]]:
    """Backend -> names of the twins feature_extractor_list[] registers."""
    names = _extractor_names()
    listing = REGISTRY.read_text(encoding="utf-8")
    body = listing[listing.index("feature_extractor_list[] = {") :]
    twins: dict[str, set[str]] = {backend: set() for backend in GPU_BACKENDS}
    for symbol in REGISTERED.findall(body[: body.index("};")]):
        name = names.get(symbol, "")
        for backend in GPU_BACKENDS:
            if name.endswith(f"_{backend}"):
                twins[backend].add(name)
    return twins


def gated_extractors() -> dict[str, set[str]]:
    """Backend -> extractor names the gate runs, as feature_extractor_name() forms them."""
    metrics = _literal(GATE, "FEATURE_METRICS")
    aliases = _literal(GATE, "FEATURE_ALIASES")
    renamed = _literal(GATE, "BACKEND_EXTRACTOR_ALIASES")
    suffixes = _literal(GATE, "BACKEND_SUFFIX")
    gated: dict[str, set[str]] = {}
    for backend, suffix in suffixes.items():
        if backend == "cpu":
            continue
        gated[backend] = set()
        for feature in metrics:
            base = aliases.get(feature, (feature, ""))[0]
            gated[backend].add(renamed.get((base, backend), f"{base}{suffix}"))
    return gated


def uncovered(twins: dict[str, set[str]], gated: dict[str, set[str]]) -> list[str]:
    """Registered twins of a gated backend that are no gate feature's extractor."""
    return sorted(
        name for backend, names in twins.items() if backend in gated for name in names - gated[backend]
    )


class ParityGateCoversRegisteredTwins(unittest.TestCase):
    def test_registry_is_parsed(self) -> None:
        twins = registered_twins()
        self.assertIn("adm_cuda", twins["cuda"])
        self.assertIn("speed_temporal_hip", twins["hip"])
        self.assertIn("vif_sycl", twins["sycl"])
        for backend in ("cuda", "sycl", "hip"):
            self.assertGreaterEqual(len(twins[backend]), 19, backend)
        self.assertIn("integer_adm_metal", twins["metal"])
        self.assertGreaterEqual(len(twins["metal"]), 17)

    def test_every_twin_of_a_gated_backend_is_a_gate_cell(self) -> None:
        self.assertEqual(uncovered(registered_twins(), gated_extractors()), [])

    def test_a_twin_without_a_gate_feature_is_detected(self) -> None:
        # The gate before ADR-1460: no `speed_temporal` feature.
        gated = gated_extractors()
        for names in gated.values():
            names.discard(next(name for name in names if name.startswith("speed_temporal")))
        self.assertEqual(
            uncovered(registered_twins(), gated),
            ["speed_temporal_cuda", "speed_temporal_hip", "speed_temporal_sycl"],
        )

    def test_metal_twins_are_gate_cells(self) -> None:
        # ADR-1496: before it, the gate had no `metal` entry and none of the 17
        # registered Metal twins was a cell.
        gated = gated_extractors()
        self.assertIn("metal", gated)
        self.assertTrue(registered_twins()["metal"] <= gated["metal"])
        del gated["metal"]
        twins = registered_twins()
        self.assertEqual({b for b, n in twins.items() if n and b not in gated}, {"metal"})

    def test_backends_the_gate_cannot_run_are_on_record(self) -> None:
        twins = registered_twins()
        gated = gated_extractors()
        ungated = {backend for backend, names in twins.items() if names and backend not in gated}
        self.assertEqual(ungated, set(UNGATED_BACKENDS))
        state = (ROOT / "docs" / "state.md").read_text(encoding="utf-8")
        for backend, row in UNGATED_BACKENDS.items():
            self.assertTrue(
                f"**{row}**" in state, f"{backend}: {row} is not a row of docs/state.md"
            )

    def test_both_gates_run_the_same_cells(self) -> None:
        # The single-feature gate has its own copy of the tables; it lacked
        # `ssim` and compared three PSNR planes where the matrix gate
        # compared one.
        for table in ("FEATURE_METRICS", "FEATURE_ALIASES", "BACKEND_EXTRACTOR_ALIASES"):
            with self.subTest(table=table):
                self.assertEqual(_literal(GATE, table), _literal(SINGLE_FEATURE_GATE, table))

    def test_psnr_cell_compares_every_plane(self) -> None:
        self.assertEqual(
            _literal(GATE, "FEATURE_METRICS")["psnr"], ("psnr_y", "psnr_cb", "psnr_cr")
        )


if __name__ == "__main__":
    unittest.main()

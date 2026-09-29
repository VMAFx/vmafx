#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The cross-backend gates must look metrics up by the name vmaf writes (ADR-1364).

``vmaf --json`` writes a feature under its alias from core/src/feature/alias.c
(``Cambi_feature_cambi_score`` is written as ``cambi``). The parity gate and
the single-feature diff read the JSON with the raw name, so their ``cambi``
cell raised ``KeyError`` instead of comparing anything. Found while running
every SYCL twin on Windows; the lookup is platform-neutral.
"""

from __future__ import annotations

import ast
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALIAS_C = ROOT / "core" / "src" / "feature" / "alias.c"
GATES = (
    ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py",
    ROOT / "scripts" / "ci" / "cross_backend_vif_diff.py",
)


def aliased_raw_names() -> dict[str, str]:
    """Map every raw feature name in alias.c to the alias vmaf writes instead."""
    source = ALIAS_C.read_text(encoding="utf-8")
    pairs = re.findall(r'\.name\s*=\s*"([^"]+)",\s*\.alias\s*=\s*"([^"]+)"', source)
    return dict(pairs)


def feature_metrics(path: Path) -> dict[str, tuple[str, ...]]:
    """Read the FEATURE_METRICS literal without importing the gate."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        target = node.target if isinstance(node, ast.AnnAssign) else None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == "FEATURE_METRICS":
            value = node.value
            if value is None:
                break
            return ast.literal_eval(value)
    raise AssertionError(f"{path.name}: no FEATURE_METRICS literal")


class ParityGateMetricNamesTest(unittest.TestCase):
    def test_alias_table_is_parsed(self) -> None:
        aliases = aliased_raw_names()
        self.assertEqual(aliases.get("Cambi_feature_cambi_score"), "cambi")
        self.assertEqual(aliases.get("VMAF_integer_feature_vif_scale0_score"), "integer_vif_scale0")

    def test_gates_use_the_names_vmaf_writes(self) -> None:
        aliases = aliased_raw_names()
        for gate in GATES:
            for feature, metrics in feature_metrics(gate).items():
                for metric in metrics:
                    with self.subTest(gate=gate.name, feature=feature, metric=metric):
                        self.assertNotIn(
                            metric,
                            aliases,
                            f"vmaf writes {metric!r} as {aliases.get(metric)!r}",
                        )

    def test_cambi_cells_read_the_cambi_key(self) -> None:
        for gate in GATES:
            with self.subTest(gate=gate.name):
                self.assertEqual(feature_metrics(gate)["cambi"], ("cambi",))


if __name__ == "__main__":
    unittest.main()

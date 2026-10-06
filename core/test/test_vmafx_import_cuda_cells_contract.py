#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the CUDA import bit-exactness cells (ADR-2023).

`core/test/test_vmafx_import_cuda_bitexact.c` compares imported CUDA frames
with host-uploaded ones for every cell of `core/test/vmafx_cuda_cells.h`. The
exit evidence of the CUDA lane (ADR-1829) covers every CUDA twin declared
exact, so the table must name exactly the cells of
`scripts/ci/exact_twins.d/*.cuda`, each as the extractor and options the
parity gate registers for it (`FEATURE_ALIASES` of
`scripts/ci/cross_backend_parity_gate.py`). A twin declared exact later and
missing here fails this test; so does a table row naming a cell that is not
declared, or an alias with other options. The check is run on planted
defects too, each of which it must refuse.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.ci.cross_backend_calibration import EXACT_TWINS  # noqa: E402
from scripts.ci.cross_backend_parity_gate import FEATURE_ALIASES  # noqa: E402

CELLS_H = ROOT / "core" / "test" / "vmafx_cuda_cells.h"
ROW = re.compile(r'\{\s*"([a-z0-9_]+)",\s*"([a-z0-9_]+)",\s*(NULL|"[^"]*")\s*,\s*(\d+)u\s*\}')


def table_rows(text: str) -> dict[str, tuple[str, str | None]]:
    """Cell -> (extractor, options) of the `vc_cells` table in `text`."""
    body = text.split("static const VcCell vc_cells[] = {", 1)[1].split("};", 1)[0]
    rows: dict[str, tuple[str, str | None]] = {}
    for name, extractor, options, _ in ROW.findall(body):
        rows[name] = (extractor, None if options == "NULL" else options.strip('"'))
    return rows


def problems(rows: dict[str, tuple[str, str | None]], exact: set[str]) -> list[str]:
    """Every way the table disagrees with the declared exact CUDA twins."""
    found = [f"missing cell {cell}" for cell in sorted(exact - rows.keys())]
    found += [f"undeclared cell {cell}" for cell in sorted(rows.keys() - exact)]
    for cell, (extractor, options) in sorted(rows.items()):
        want = FEATURE_ALIASES.get(cell, (cell, None))
        if (extractor, options) != want:
            found.append(f"cell {cell}: {extractor} {options}, the gate registers {want}")
    return found


class CellsContract(unittest.TestCase):
    def setUp(self) -> None:
        self.text = CELLS_H.read_text(encoding="utf-8")
        self.rows = table_rows(self.text)
        self.exact = {feature for feature, backends in EXACT_TWINS.items() if "cuda" in backends}

    def test_table_is_every_exact_cuda_twin(self) -> None:
        self.assertGreater(len(self.exact), 0)
        self.assertEqual(problems(self.rows, self.exact), [])

    def test_refuses_a_missing_cell(self) -> None:
        rows = dict(self.rows)
        rows.pop("vif")
        self.assertIn("missing cell vif", problems(rows, self.exact))

    def test_refuses_an_undeclared_cell(self) -> None:
        rows = dict(self.rows)
        rows["ciede"] = ("ciede", None)
        self.assertIn("undeclared cell ciede", problems(rows, self.exact))

    def test_refuses_other_alias_options(self) -> None:
        rows = dict(self.rows)
        rows["motion_debug"] = ("motion", None)
        self.assertTrue(any(p.startswith("cell motion_debug") for p in problems(rows, self.exact)))

    def test_parses_every_row(self) -> None:
        declared = self.text.count('{"', self.text.index("vc_cells[]"))
        self.assertEqual(len(self.rows), declared)


if __name__ == "__main__":
    unittest.main()

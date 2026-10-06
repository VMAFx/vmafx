#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free contract of the SYCL import bit-exactness cells (ADR-2091).

`core/test/test_vmafx_import_sycl_bitexact.c` compares imported SYCL frames
with host-uploaded ones for every cell of `core/test/vmafx_sycl_cells.h`. The
exit evidence of the SYCL lane (ADR-1829) covers every SYCL twin declared
exact, so the table must name exactly the cells of
`scripts/ci/exact_twins.d/*.sycl`, each as the extractor and options the
parity gate registers for it. The rules are the CUDA lane's
(`test_vmafx_import_cuda_cells_contract.py`, whose checks this reuses); the
check is run on planted defects too, each of which it must refuse.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))

from scripts.ci.cross_backend_calibration import EXACT_TWINS  # noqa: E402
from test_vmafx_import_cuda_cells_contract import problems, table_rows  # noqa: E402

CELLS_H = HERE / "vmafx_sycl_cells.h"


class SyclCellsContract(unittest.TestCase):
    def setUp(self) -> None:
        self.text = CELLS_H.read_text(encoding="utf-8")
        self.rows = table_rows(self.text, "vs_cells")
        self.exact = {feature for feature, backends in EXACT_TWINS.items() if "sycl" in backends}

    def test_table_is_every_exact_sycl_twin(self) -> None:
        self.assertGreater(len(self.exact), 0)
        self.assertEqual(problems(self.rows, self.exact), [])

    def test_refuses_a_missing_cell(self) -> None:
        rows = dict(self.rows)
        rows.pop("ssimulacra2")
        self.assertIn("missing cell ssimulacra2", problems(rows, self.exact))

    def test_refuses_an_undeclared_cell(self) -> None:
        rows = dict(self.rows)
        rows["ciede"] = ("ciede", None)
        self.assertIn("undeclared cell ciede", problems(rows, self.exact))

    def test_refuses_other_alias_options(self) -> None:
        rows = dict(self.rows)
        rows["float_ms_ssim_lcs"] = ("float_ms_ssim", None)
        self.assertTrue(
            any(p.startswith("cell float_ms_ssim_lcs") for p in problems(rows, self.exact))
        )

    def test_parses_every_row(self) -> None:
        declared = self.text.count('{"', self.text.index("vs_cells[]"))
        self.assertEqual(len(self.rows), declared)


if __name__ == "__main__":
    unittest.main()

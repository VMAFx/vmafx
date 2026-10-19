# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""rust_twin_diff.view_distance_variants(): the second-viewing-distance cells
(ADR-2795) derived from model option sets."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.rust_twin_diff import (
    REPO,
    model_option_sets,
    view_distance_variants,
)

BASE = {"adm_dlm_weight": 0.7, "adm_ref_display_height": 1080}


class ViewDistanceVariants(unittest.TestCase):
    def test_pair_differing_only_in_distance_gives_one_cell(self) -> None:
        sets = [{**BASE, "adm_norm_view_dist": 3.0}, {**BASE, "adm_norm_view_dist": 5.0}]
        self.assertEqual(
            view_distance_variants(sets),
            [{**BASE, "adm_norm_view_dist": 3.0, "adm_norm_view_dist_extra": 5.0}],
        )

    def test_other_differences_give_none(self) -> None:
        sets = [
            {**BASE, "adm_norm_view_dist": 3.0},
            {**BASE, "adm_norm_view_dist": 5.0, "adm_ref_display_height": 2160},
            {"adm_dlm_weight": 0.7},
        ]
        self.assertEqual(view_distance_variants(sets), [])

    def test_equal_distance_and_empty_input_give_none(self) -> None:
        same = {**BASE, "adm_norm_view_dist": 3.0}
        self.assertEqual(view_distance_variants([same, dict(same)]), [])
        self.assertEqual(view_distance_variants([]), [])

    def test_the_shipped_models_give_two_adm_cells(self) -> None:
        cells = view_distance_variants(model_option_sets(REPO)["adm"])
        self.assertEqual(
            sorted((c["adm_norm_view_dist"], c["adm_norm_view_dist_extra"]) for c in cells),
            [(1.5, 3.0), (3.0, 5.0)],
        )


if __name__ == "__main__":
    unittest.main()

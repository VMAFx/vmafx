#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary tests for scripts/ci/depot_minutes.py (ADR-2168)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import depot_minutes as dm


def _job(label: str, seconds: int) -> dict[str, Any]:
    return {
        "labels": [label],
        "started_at": "2026-10-06T10:00:00Z",
        "completed_at": f"2026-10-06T{10 + seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}Z",
    }


class MultiplierTable(unittest.TestCase):
    def test_documented_linux_sizes(self) -> None:
        # depot.dev/docs/github-actions/runner-types, checked 2026-10-07.
        table = {
            "depot-ubuntu-24.04": 1,
            "depot-ubuntu-24.04-4": 2,
            "depot-ubuntu-24.04-8": 4,
            "depot-ubuntu-24.04-16": 8,
            "depot-ubuntu-24.04-64": 32,
            "depot-ubuntu-22.04-arm-8": 4,
            "depot-ubuntu-24.04-arm": 1,
        }
        for label, want in table.items():
            self.assertEqual(dm.multiplier(label), want, label)

    def test_documented_windows_sizes(self) -> None:
        self.assertEqual(dm.multiplier("depot-windows-2025"), 2)
        self.assertEqual(dm.multiplier("depot-windows-2025-8"), 8)

    def test_unknown_label_is_refused(self) -> None:
        for label in ("depot-macos-26", "ubuntu-latest", "depot-ubuntu-24.04-x"):
            with self.assertRaises(ValueError):
                dm.multiplier(label)

    def test_planted_wrong_multiplier_is_caught(self) -> None:
        # A table that read -8 as 8x (vCPU count) would double the true 4x.
        jobs = [_job("depot-ubuntu-24.04-8", 3600)]
        self.assertEqual(dm.base_minutes(jobs), 240)
        self.assertNotEqual(dm.base_minutes(jobs), 480)


class Sums(unittest.TestCase):
    def test_github_hosted_jobs_do_not_count(self) -> None:
        self.assertEqual(dm.base_minutes([_job("ubuntu-latest", 3600)]), 0)

    def test_sizes_add_up(self) -> None:
        jobs = [_job("depot-ubuntu-24.04", 1800), _job("depot-ubuntu-24.04-4", 1800)]
        self.assertEqual(dm.base_minutes(jobs), 30 + 60)

    def test_rounds_up_the_month_total(self) -> None:
        self.assertEqual(dm.base_minutes([_job("depot-ubuntu-24.04", 61)]), 2)

    def test_unfinished_job_counts_zero(self) -> None:
        job = {"labels": ["depot-ubuntu-24.04"], "started_at": None, "completed_at": None}
        self.assertEqual(dm.base_minutes([job]), 0)

    def test_empty(self) -> None:
        self.assertEqual(dm.base_minutes([]), 0)


class Threshold(unittest.TestCase):
    def _main(self, minutes: int) -> int:
        real = dm.fetch_jobs
        dm.fetch_jobs = lambda *_a, **_k: [_job("depot-ubuntu-24.04", minutes * 60)]
        try:
            return dm.main(["--month", "2026-10", "--limit", "100"])
        finally:
            dm.fetch_jobs = real

    def test_below_at_and_above_threshold(self) -> None:
        self.assertEqual(self._main(89), 0)
        self.assertEqual(self._main(90), 1)
        self.assertEqual(self._main(150), 1)

    def test_unreadable_api_fails_closed(self) -> None:
        real = dm.fetch_jobs

        def boom(*_a: object, **_k: object) -> list[dict[str, Any]]:
            raise RuntimeError("403")

        dm.fetch_jobs = boom
        try:
            self.assertEqual(dm.main(["--month", "2026-10"]), 2)
        finally:
            dm.fetch_jobs = real


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The hosted clang-tidy lanes keep one contract (ADR-2796, Q-315).

Every lane the ratchet knows is a required, path-routed check: the cpu lane is
`Tidy Ratchet`, the device and cross lanes are the legs of `Tidy Lane (<lane>)`,
and the macOS lane is `Tidy Metal`. A nightly sweep measures them all, unrouted.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github/workflows"
ACTION = ROOT / ".github/actions/tidy-lane/action.yml"
IMPACT = ROOT / ".github/ci-impact.json"
LANES = ("cuda", "hip", "sycl", "arm64", "clang")
LISTS = 2  # `required` and `strictMustReport`
DIGEST = re.compile(r"^ghcr\.io/vmafx/vmafx-dev-mcp@sha256:[0-9a-f]{64}$")


def load(path: Path) -> dict[str, object]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return data


def lint_problems(lint: str) -> list[str]:
    found: list[str] = []
    leg = yaml.safe_load(lint)["jobs"].get("clang-tidy-lanes", {})
    matrix = leg.get("strategy", {})
    if leg.get("name") != "Tidy Lane (${{ matrix.lane }})":
        found.append("lint: the lane job is not named 'Tidy Lane (${{ matrix.lane }})'")
    if sorted(matrix.get("matrix", {}).get("lane", [])) != sorted(LANES):
        found.append("lint: the matrix does not list exactly the device and cross lanes")
    if matrix.get("fail-fast") is not False:
        found.append("lint: a failing lane must not cancel the others (fail-fast: false)")
    steps = leg.get("steps", [])
    measure = [s for s in steps if s.get("uses") == "./.github/actions/tidy-lane"]
    if not measure:
        found.append("lint: the lane job does not use the tidy-lane action")
    if not all("tidy_{0}" in s.get("if", "") for s in measure):
        found.append("lint: the lane is not routed by its tidy_<lane> selector")
    if not any("NOT measured" in s.get("run", "") for s in steps):
        found.append("lint: a skipped lane does not say it was not measured")
    return found


def aggregator_problems(aggregator: str) -> list[str]:
    found: list[str] = []
    for label in (*(f"Tidy Lane ({lane})" for lane in LANES), "Tidy Metal"):
        if aggregator.count(f"'{label}'") != LISTS:
            found.append(f"aggregator: '{label}' must be in required and strictMustReport")
    return found


def nightly_problems(nightly: str) -> list[str]:
    found: list[str] = []
    workflow = yaml.safe_load(nightly)
    sweep = workflow["jobs"].get("clang-tidy-lanes", {})
    if sorted(sweep.get("strategy", {}).get("matrix", {}).get("lane", [])) != sorted(LANES):
        found.append("nightly: the sweep does not cover every lane")
    if "schedule" not in workflow.get(True, {}):
        found.append("nightly: no schedule")
    drift = workflow["jobs"].get("tidy-drift", {})
    if set(drift.get("needs", [])) != {"clang-tidy-full", "clang-tidy-lanes"}:
        found.append("nightly: the drift job must wait for the cpu scan and the lane sweep")
    return found


def metal_problems(metal: str) -> list[str]:
    found: list[str] = []
    workflow = yaml.safe_load(metal)
    if workflow["jobs"].get("gate", {}).get("name") != "Tidy Metal":
        found.append("metal: the required context 'Tidy Metal' is not the gate job")
    if "schedule" not in workflow.get(True, {}):
        found.append("metal: no schedule")
    return found


def action_problems(action: str) -> list[str]:
    image = yaml.safe_load(action)["inputs"]["image"].get("default", "")
    if DIGEST.match(image):
        return []
    return ["action: the dev container is not pinned by a sha256 digest"]


def problems(lint: str, nightly: str, aggregator: str, metal: str, action: str) -> list[str]:
    """Every way the five texts break the contract; empty when they keep it."""
    return [
        *lint_problems(lint),
        *aggregator_problems(aggregator),
        *nightly_problems(nightly),
        *metal_problems(metal),
        *action_problems(action),
    ]


class HostedTidyLanes(unittest.TestCase):
    def texts(self) -> tuple[str, str, str, str, str]:
        return (
            (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8"),
            (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8"),
            (WORKFLOWS / "required-aggregator.yml").read_text(encoding="utf-8"),
            (WORKFLOWS / "tidy-metal.yml").read_text(encoding="utf-8"),
            ACTION.read_text(encoding="utf-8"),
        )

    def test_the_repository_keeps_the_contract(self) -> None:
        self.assertEqual(problems(*self.texts()), [])

    def test_every_lane_has_a_selector_and_a_baseline(self) -> None:
        selectors = json.loads(IMPACT.read_text(encoding="utf-8"))["selectors"]
        for lane in (*LANES, "metal"):
            with self.subTest(lane=lane):
                self.assertTrue(selectors[f"tidy_{lane}"]["own_paths_only"])
                self.assertTrue((ROOT / f"scripts/ci/tidy-baseline-{lane}.json").is_file())

    def test_mutations_are_caught(self) -> None:
        # (position in texts(), mutated text, word the problem must contain)
        lint, nightly, aggregator, metal, action = self.texts()
        mutations = {
            "a lane dropped from the aggregator": (
                2,
                aggregator.replace("'Tidy Lane (hip)',", "", 1),
                "aggregator",
            ),
            "the matrix loses a lane": (
                0,
                lint.replace("lane: [cuda, hip, sycl, arm64, clang]", "lane: [cuda, hip]", 1),
                "matrix",
            ),
            "the image floats": (
                4,
                re.sub(r"@sha256:[0-9a-f]{64}", ":master", action, count=1),
                "digest",
            ),
            "the sweep loses a lane": (
                1,
                nightly.replace("lane: [cuda, hip, sycl, arm64, clang]", "lane: [cuda]", 1),
                "sweep",
            ),
            "the gate job is renamed": (
                3,
                metal.replace("    name: Tidy Metal\n", "    name: Tidy Metal gate\n", 1),
                "Tidy Metal",
            ),
        }
        for label, (position, text, word) in mutations.items():
            with self.subTest(label):
                texts = [lint, nightly, aggregator, metal, action]
                texts[position] = text
                self.assertTrue(
                    any(word in found for found in problems(*texts)), f"{label}: not detected"
                )


if __name__ == "__main__":
    unittest.main()

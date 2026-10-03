#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The macOS tester bundle measures every open Metal row, with exact tests (ADR-1496).

`tools/rc1-tester/image/metal-rows.json` names, per open Metal row of
`docs/state.md`, the measurements of the bundle that close it: cases of the
Metal parity tests, Metal scores on the fixtures, cells of the parity gate's
Metal run. This test keeps that map honest without a device:

- every `RC3 (Metal only ...)` row of docs/state.md is in the map, and every
  row of the map is a row of docs/state.md;
- every named case is a case of the named test, run through metal_run_case()
  so it prints the `@case` line the report reads;
- every Metal parity test of core/test/meson.build is in the bundle's unit list;
- the Metal parity tests compare with `==`: no places-style tolerance, and the
  only bound is the ciede twin's LIBM_TWINS one;
- the gate features are exactly the gate features that have a Metal twin, and
  the fixtures a feature is left out of exist.

Device-free: reads the sources only.
"""

from __future__ import annotations

import ast
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROWS = ROOT / "tools" / "rc1-tester" / "image" / "metal-rows.json"
FIXTURES = ROOT / "tools" / "rc1-tester" / "image" / "fixtures.json"
UNIT_LIST = ROOT / "tools" / "rc1-tester" / "image" / "unit-tests-macos.txt"
STATE = ROOT / "docs" / "state.md"
MESON = ROOT / "core" / "test" / "meson.build"
TESTS = ROOT / "core" / "test"
GATE = ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py"
METAL_SOURCES = ROOT / "core" / "src" / "feature" / "metal"

METAL_ONLY_ROW = re.compile(r"^\| \*\*(T-[A-Z0-9-]+)\*\* \| \*\*RC3 \(Metal only", re.M)
STATE_ROW = re.compile(r"^\| \*\*(T-[A-Z0-9-]+)\*\* \|", re.M)
MESON_LIST = re.compile(r"metal_parity_tests = \[(.*?)\]", re.S)
# A places-style or loose tolerance in a Metal parity test.
LOOSE_TOLERANCE = re.compile(r"PARITY_TOL|PLACES|\b[15]e-[2-6]\b")
# The one bound a Metal parity test may set: LIBM_TWINS["ciede"].
ALLOWED_BOUND = re.compile(r"#define CIEDE_TWIN_TOL 1e-9\b")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def code_of(source: str) -> str:
    """The source without comments: a history note may name the old tolerance."""
    return COMMENT.sub("", source)


def row_map() -> dict:
    return json.loads(ROWS.read_text(encoding="utf-8"))


def meson_metal_tests() -> list[str]:
    match = MESON_LIST.search(MESON.read_text(encoding="utf-8"))
    assert match, "core/test/meson.build: no metal_parity_tests list"
    return [f"test_metal_{name}_parity" for name in re.findall(r"'(\w+)'", match.group(1))]


def gate_literal(name: str) -> object:
    tree = ast.parse(GATE.read_text(encoding="utf-8"))
    for node in tree.body:
        target = node.target if isinstance(node, ast.AnnAssign) else None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name and node.value is not None:
            return ast.literal_eval(node.value)
    raise AssertionError(f"{GATE.name}: no {name} literal")


def registered_metal_names() -> set[str]:
    names = set()
    for path in METAL_SOURCES.glob("*.mm"):
        names |= set(re.findall(r'\.name\s*=\s*"(\w+_metal)"', path.read_text(encoding="utf-8")))
    return names


def metal_gate_features() -> set[str]:
    """Gate features whose Metal extractor, as the gate names it, is registered."""
    metrics = gate_literal("FEATURE_METRICS")
    aliases = gate_literal("FEATURE_ALIASES")
    renamed = gate_literal("BACKEND_EXTRACTOR_ALIASES")
    registered = registered_metal_names()
    found = set()
    for feature in metrics:
        base = aliases.get(feature, (feature, ""))[0]
        if renamed.get((base, "metal"), f"{base}_metal") in registered:
            found.add(feature)
    return found


class MetalReportRowsContract(unittest.TestCase):
    def test_every_open_metal_only_row_is_measured(self) -> None:
        mapped = {row["id"] for row in row_map()["rows"]}
        metal_only = set(METAL_ONLY_ROW.findall(STATE.read_text(encoding="utf-8")))
        self.assertTrue(metal_only, "docs/state.md has no RC3 (Metal only) row: pattern stale?")
        self.assertEqual(sorted(metal_only - mapped), [])

    def test_every_mapped_row_is_a_state_row(self) -> None:
        rows = set(STATE_ROW.findall(STATE.read_text(encoding="utf-8")))
        for row in row_map()["rows"]:
            with self.subTest(row=row["id"]):
                self.assertIn(row["id"], rows)
                self.assertTrue(row.get("cases") or row.get("metrics") or row.get("gate"))

    def test_every_case_is_run_with_a_verdict_line(self) -> None:
        tests = set(meson_metal_tests())
        for row in row_map()["rows"]:
            for spec in row.get("cases", []):
                with self.subTest(row=row["id"], case=spec["case"]):
                    self.assertIn(spec["test"], tests)
                    source = (TESTS / f"{spec['test']}.c").read_text(encoding="utf-8")
                    # A case is a function, or one of the two a TWIN_CASE(x, ...) of
                    # the option-table test defines (test_twin_x, test_twin_x_provides).
                    twin = re.match(r"^test_twin_(\w+?)(?:_provides)?$", spec["case"])
                    defined = rf"static char \*{spec['case']}\(void\)"
                    if twin:
                        defined += rf"|TWIN_CASE\({twin.group(1)},"
                    self.assertRegex(source, defined)
                    self.assertRegex(source, rf"metal_run_case\({spec['case']}\);")

    def test_metal_metrics_name_registered_twins(self) -> None:
        registered = registered_metal_names()
        for row in row_map()["rows"]:
            for spec in row.get("metrics", []):
                with self.subTest(row=row["id"], extractor=spec["extractor"]):
                    self.assertIn(spec["extractor"], registered)
                    self.assertIn(float(spec["bound"]), (0.0, 1e-9))

    def test_bundle_runs_every_metal_parity_test(self) -> None:
        listed = {
            line.strip()
            for line in UNIT_LIST.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        }
        self.assertEqual(sorted(set(meson_metal_tests()) - listed), [])

    def test_metal_parity_tests_compare_exactly(self) -> None:
        for test in meson_metal_tests():
            with self.subTest(test=test):
                source = (TESTS / f"{test}.c").read_text(encoding="utf-8")
                self.assertIn('#include "metal_twin.h"', source)
                self.assertIn("return metal_first_failure;", source)
                self.assertNotIn("mu_run_test(", source)
                stripped = ALLOWED_BOUND.sub("", code_of(source))
                self.assertIsNone(LOOSE_TOLERANCE.search(stripped), "a tolerance in a Metal test")

    def test_gate_features_are_the_metal_twins(self) -> None:
        gate = row_map()["gate"]
        self.assertEqual(sorted(gate["features"]), sorted(metal_gate_features()))
        fixtures = {f["id"] for f in json.loads(FIXTURES.read_text(encoding="utf-8"))["fixtures"]}
        for entry in gate["skip"]:
            with self.subTest(fixture=entry["fixture"]):
                self.assertIn(entry["fixture"], fixtures)
                self.assertTrue(set(entry["features"]) <= set(gate["features"]))
                self.assertRegex(entry["reason"], r"T-[A-Z0-9-]+")

    def test_a_missing_row_is_detected(self) -> None:
        state = "| **T-METAL-EXAMPLE-2026-10-03** | **RC3 (Metal only; needs an Apple device). x"
        self.assertEqual(METAL_ONLY_ROW.findall(state), ["T-METAL-EXAMPLE-2026-10-03"])
        self.assertIsNotNone(LOOSE_TOLERANCE.search("#define PARITY_TOL 1e-4"))
        self.assertIsNone(LOOSE_TOLERANCE.search(code_of("/* was places=4, 1e-4 */ x == y")))
        self.assertIsNone(
            LOOSE_TOLERANCE.search(ALLOWED_BOUND.sub("", "#define CIEDE_TWIN_TOL 1e-9"))
        )


if __name__ == "__main__":
    unittest.main()

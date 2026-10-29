# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The encoder registry loader and its generated surfaces (ADR-3388).

Positive: the committed api/vmafx-encoders.toml loads, and the Go copy, the
Python copy and the docs page are what the emitter writes from it. Negative:
every rule refuses its planted defect (an unknown family, an implemented row
without an adapter table, an adapter on a gap row, a window outside the
absolute range, a default outside the window, a probe preset outside the
presets, a non-boolean flag, a duplicate id, a variant of an unknown encoder,
a bad check date). Boundary: a window equal to the absolute range and a
default on either edge load; an empty registry loads and emits empty tables.
"""

from __future__ import annotations

import copy
import tomllib
import unittest
from typing import Any

from support import ROOT
from vmafx_api import emit_encoders, encoders
from vmafx_api.model import DefinitionError


def live() -> dict[str, Any]:
    with (ROOT / encoders.DEFINITION).open("rb") as handle:
        return tomllib.load(handle)


def first_implemented(doc: dict[str, Any]) -> dict[str, Any]:
    return next(e for e in doc["encoders"] if e["status"] == "implemented")


def gap_row() -> dict[str, Any]:
    return {
        "id": "libgap",
        "family": "vvc",
        "paradigm": "classic",
        "class": "delivery",
        "status": "planned",
        "interface": "cli",
        "licence_class": "permissive",
        "maintenance": "active",
        "upstream": "https://example.invalid/gap",
        "checked": "2026-10-29",
    }


class Positive(unittest.TestCase):
    def test_committed_definition_loads(self) -> None:
        registry = encoders.parse(live())
        self.assertTrue(any(e.status == "implemented" for e in registry.encoders))

    def test_committed_outputs_are_current(self) -> None:
        registry = encoders.load(ROOT / encoders.DEFINITION)
        for path, text in emit_encoders.files(registry).items():
            with self.subTest(path=path):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8"), text)

    def test_go_and_python_copies_identical(self) -> None:
        go, python = (ROOT / p for p in emit_encoders.OUTPUTS)
        self.assertEqual(go.read_bytes(), python.read_bytes())


class Negative(unittest.TestCase):
    def refused(self, doc: dict[str, Any], needle: str) -> None:
        with self.assertRaises(DefinitionError) as caught:
            encoders.parse(doc)
        self.assertIn(needle, str(caught.exception))

    def mutate(self, needle: str, change: Any) -> None:
        doc = copy.deepcopy(live())
        change(doc)
        self.refused(doc, needle)

    def test_unknown_family(self) -> None:
        self.mutate("`family`", lambda d: first_implemented(d).update(family="nope"))

    def test_implemented_without_adapter(self) -> None:
        self.mutate("`adapter` table", lambda d: first_implemented(d).pop("adapter"))

    def test_adapter_on_gap_row(self) -> None:
        row = gap_row() | {"adapter": {}}
        self.mutate("`adapter` table", lambda d: d["encoders"].append(row))

    def test_window_outside_absolute(self) -> None:
        def change(d: dict[str, Any]) -> None:
            adapter = first_implemented(d)["adapter"]
            adapter["quality_range"] = [
                adapter["absolute_range"][0] - 1,
                adapter["quality_range"][1],
            ]

        self.mutate("outside absolute_range", change)

    def test_default_outside_window(self) -> None:
        def change(d: dict[str, Any]) -> None:
            adapter = first_implemented(d)["adapter"]
            adapter["quality_default"] = adapter["quality_range"][1] + 1

        self.mutate("quality_default", change)

    def test_probe_preset_not_listed(self) -> None:
        self.mutate(
            "probe_preset",
            lambda d: first_implemented(d)["adapter"].update(probe_preset="nonexistent"),
        )

    def test_flag_not_bool(self) -> None:
        self.mutate(
            "supports_two_pass",
            lambda d: first_implemented(d)["adapter"].update(supports_two_pass=1),
        )

    def test_duplicate_id(self) -> None:
        self.mutate("appears twice", lambda d: d["encoders"].append(first_implemented(d)))

    def test_variant_of_unknown_encoder(self) -> None:
        variant = {
            "adapter": "libnope",
            "variant": "fork",
            "kind": "runtime-fork",
            "status": "planned",
            "licence_class": "permissive",
            "upstream": "",
            "checked": "2026-10-29",
        }
        self.mutate(
            "has no [[encoders]] row", lambda d: d.setdefault("variants", []).append(variant)
        )

    def test_bad_checked_date(self) -> None:
        self.mutate("YYYY-MM-DD", lambda d: first_implemented(d).update(checked="29.10.2026"))

    def test_wrong_format_version(self) -> None:
        self.mutate("format_version", lambda d: d.update(format_version=2))


class Boundary(unittest.TestCase):
    def test_window_equal_to_absolute_and_edge_defaults(self) -> None:
        for edge in (0, 1):
            doc = copy.deepcopy(live())
            adapter = first_implemented(doc)["adapter"]
            adapter["quality_range"] = list(adapter["absolute_range"])
            adapter["quality_default"] = adapter["absolute_range"][edge]
            adapter["probe_quality"] = adapter["absolute_range"][edge]
            with self.subTest(edge=edge):
                encoders.parse(doc)

    def test_empty_registry(self) -> None:
        registry = encoders.parse({"format_version": encoders.FORMAT_VERSION})
        self.assertEqual(registry.encoders, ())
        page = emit_encoders.docs_page(registry)
        self.assertIn("## Variants", page)


if __name__ == "__main__":
    unittest.main()

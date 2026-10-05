#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Append-only check of the definition features on the fixture (HISS-14, ADR-1852).

Planted breaks the checker refuses without an ABI minor bump: growth of a
struct another struct embeds by value, a changed array length, a renumbered
flag bit, a changed callback signature, a removed option and a changed proto
field number.
"""

from __future__ import annotations

import copy
import unittest
from collections.abc import Callable
from typing import Any

from support import bumped, entry, fixture
from vmafx_api import abi_check
from vmafx_api.loader import parse

Edit = Callable[[dict[str, Any]], None]


def _grow_fence(doc: dict[str, Any]) -> None:
    fields = entry(doc["structs"], "VmafxFence")["fields"]
    fields.append({"name": "extra", "type": "u32", "since": "0.2"})


def _longer_array(doc: dict[str, Any]) -> None:
    entry(doc["structs"], "VmafxFrameImport")["fields"][3]["count"] = 4


def _flag_bit(doc: dict[str, Any]) -> None:
    entry(doc["flags"], "VmafxImportFlags")["bits"][1]["bit"] = 4


def _callback(doc: dict[str, Any]) -> None:
    params = entry(doc["callbacks"], "VmafxWindowCallback")["params"]
    params.insert(2, {"name": "status", "type": "status", "pass": "in"})


def _option_removed(doc: dict[str, Any]) -> None:
    entry(doc["option_groups"], "pool")["options"].pop()


def _proto_field(doc: dict[str, Any]) -> None:
    entry(doc["option_groups"], "threads")["options"][0]["proto"] = {"field": 8}


def _spelling_removed(doc: dict[str, Any]) -> None:
    entry(doc["option_groups"], "threads")["options"][0]["aliases"] = []


CASES: tuple[tuple[Edit, str], ...] = (
    (_grow_fence, "VmafxFence: grew while VmafxFrameImport embeds it by value"),
    (_longer_array, "VmafxFrameImport: field removed, reordered, renamed or retyped"),
    (_flag_bit, "VMAFX_IMPORT_ALLOW_PLANAR_ONLY renumbered 8 -> 16"),
    (_callback, "callback VmafxWindowCallback: parameters or result changed"),
    (_option_removed, "option pool.subsample removed"),
    (_proto_field, "option threads.threads: type or proto field changed"),
    (_spelling_removed, "option threads.threads: a surface spelling was removed"),
)


class FeatureBreakTest(unittest.TestCase):
    def test_planted_breaks_need_a_minor_bump(self) -> None:
        base = fixture()
        old = parse(base)
        for edit, finding in CASES:
            with self.subTest(finding=finding):
                doc = bumped(copy.deepcopy(base), "0.2.1")
                edit(doc)
                found = abi_check.compare(old, parse(doc))
                self.assertTrue(any(finding in f for f in found), found)
                result = abi_check.report(old, parse(bumped(doc, "0.3.0")))
                self.assertTrue(any(finding in a for a in result.accepted), result.accepted)

    def test_option_and_bit_additions_are_append_only(self) -> None:
        base = fixture()
        doc = bumped(copy.deepcopy(base), "0.3.0")
        entry(doc["flags"], "VmafxImportFlags")["bits"].append(
            {"name": "VMAFX_IMPORT_TEST_ONLY", "bit": 5, "doc": "Test.", "since": "0.3"}
        )
        option = {"name": "extra", "type": "bool", "default": False, "doc": "Extra."}
        entry(doc["option_groups"], "pool")["options"].append(
            {**option, "cli": ["--extra"], "ffmpeg": "extra"}
        )
        result = abi_check.report(parse(base), parse(doc))
        self.assertEqual(result.findings, [])
        self.assertIn("constant VMAFX_IMPORT_TEST_ONLY", result.added)
        self.assertIn("option pool.extra", result.added)


if __name__ == "__main__":
    unittest.main()

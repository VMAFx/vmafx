#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every security/vex/*.openvex.json is a well-formed OpenVEX v0.2.0 document.

The checks follow the OpenVEX specification v0.2.0
(https://github.com/openvex/spec/blob/main/OPENVEX-SPEC.md): document fields,
the four statuses, the five not_affected justifications, and the statement
text each status requires. Planted defects show that every rule rejects.
"""

from __future__ import annotations

import copy
import json
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
VEX_DIR = ROOT / "security" / "vex"
CONTEXT = "https://openvex.dev/ns/v0.2.0"
STATUSES = {"not_affected", "affected", "fixed", "under_investigation"}
JUSTIFICATIONS = {
    "component_not_present",
    "vulnerable_code_not_present",
    "vulnerable_code_not_in_execute_path",
    "vulnerable_code_cannot_be_controlled_by_adversary",
    "inline_mitigations_already_exist",
}


def statement_problems(index: int, statement: dict[str, Any]) -> list[str]:
    where = f"statements[{index}]"
    problems = []
    if not str(statement.get("vulnerability", {}).get("name", "")).strip():
        problems.append(f"{where}: vulnerability.name is missing")
    products = statement.get("products") or []
    if not products or not all(str(p.get("@id", "")).startswith("pkg:") for p in products):
        problems.append(f"{where}: every product needs a purl @id")
    status = statement.get("status")
    if status not in STATUSES:
        problems.append(f"{where}: status {status!r} is not an OpenVEX status")
    if status == "not_affected":
        if statement.get("justification") not in JUSTIFICATIONS:
            problems.append(f"{where}: not_affected needs one of the five justifications")
        if not str(statement.get("impact_statement", "")).strip():
            problems.append(f"{where}: not_affected needs an impact_statement with the evidence")
    if status == "affected" and not str(statement.get("action_statement", "")).strip():
        problems.append(f"{where}: affected needs an action_statement")
    return problems


def document_problems(document: dict[str, Any]) -> list[str]:
    problems = []
    if document.get("@context") != CONTEXT:
        problems.append("@context is not OpenVEX v0.2.0")
    for field in ("@id", "author", "timestamp"):
        if not str(document.get(field, "")).strip():
            problems.append(f"{field} is missing")
    if not isinstance(document.get("version"), int) or document["version"] < 1:
        problems.append("version must be a positive integer")
    statements = document.get("statements") or []
    if not statements:
        problems.append("no statements")
    names = [s.get("vulnerability", {}).get("name") for s in statements]
    if len(names) != len(set(names)):
        problems.append("a vulnerability has two statements")
    for index, statement in enumerate(statements):
        problems += statement_problems(index, statement)
    return problems


class OpenVexDocuments(unittest.TestCase):
    def documents(self) -> list[Path]:
        found = sorted(VEX_DIR.glob("*.openvex.json"))
        self.assertTrue(found, "security/vex holds no OpenVEX document")
        return found

    def test_tracked_documents_are_well_formed(self) -> None:
        for path in self.documents():
            with self.subTest(path=path.name):
                self.assertEqual(document_problems(json.loads(path.read_text("utf-8"))), [])

    def test_every_rule_rejects_a_planted_defect(self) -> None:
        base = json.loads(self.documents()[0].read_text("utf-8"))
        defects: dict[str, Callable[[dict[str, Any]], object]] = {
            "context": lambda d: d.update({"@context": "https://openvex.dev/ns/v0.1.0"}),
            "author": lambda d: d.pop("author"),
            "version": lambda d: d.update({"version": 0}),
            "status": lambda d: d["statements"][0].update({"status": "safe"}),
            "justification": lambda d: d["statements"][0].update({"justification": "trust_me"}),
            "impact": lambda d: d["statements"][0].pop("impact_statement"),
            "purl": lambda d: d["statements"][0]["products"][0].update({"@id": "vmaf-train"}),
            "duplicate": lambda d: d["statements"].append(copy.deepcopy(d["statements"][0])),
            "affected": lambda d: d["statements"][0].update({"status": "affected"}),
        }
        for name, plant in defects.items():
            with self.subTest(defect=name):
                document = copy.deepcopy(base)
                plant(document)
                self.assertNotEqual(document_problems(document), [])


if __name__ == "__main__":
    unittest.main()

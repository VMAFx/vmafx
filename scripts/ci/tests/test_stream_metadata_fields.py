#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shape checks for the stream metadata field list (ADR-3311).

``api/stream-metadata-fields.json`` names the per-frame stream metadata fields
of VMAFx and states which keys of the Pelorus telemetry field registry VMAFx
does not carry. Pelorus' ``scripts/check-telemetry-field-parity.py`` compares
the two files; this test holds the VMAFx side to the shape that script reads.

Positive: the checked-in file passes every check.
Negative: a mutated copy per check (schema, duplicate key, unknown pelorus
class, missing column, unknown JSON type, a key both carried and not carried,
malformed seed reference) fails with that check's message.
Boundary: an empty ``not_carried`` list and a ``vmafx_only`` field pass.
"""

from __future__ import annotations

import copy
import json
import re
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
FIELD_LIST = ROOT / "api" / "stream-metadata-fields.json"
SCHEMA = "vmafx/stream-metadata-fields/1"
PELORUS_CLASSES = frozenset({"shared", "vmafx_only"})
JSON_TYPES = frozenset({"integer", "number", "string", "boolean", "array"})
COLUMNS = ("key", "json", "unit", "range", "pelorus")
KEY_RE = re.compile(r"^[a-z][a-z0-9_]*$")
SEED_RE = re.compile(r"^VMAFx/pelorus@[0-9a-f]{40}:libpelorus/schema/telemetry-fields\.json$")


def check_field(index: int, field: Any) -> list[str]:
    """The findings for one row of ``fields``."""
    if not isinstance(field, dict):
        return [f"fields[{index}] is not an object"]
    errors = [f"fields[{index}] lacks {col}" for col in COLUMNS if col not in field]
    key = field.get("key")
    if not isinstance(key, str) or not KEY_RE.match(key):
        errors.append(f"fields[{index}] key {key!r} is not snake_case")
    if field.get("json") not in JSON_TYPES:
        errors.append(f"fields[{index}] json {field.get('json')!r} is not a JSON type")
    if field.get("pelorus") not in PELORUS_CLASSES:
        errors.append(f"fields[{index}] pelorus {field.get('pelorus')!r} is not a class")
    for col in ("unit", "range"):
        if col in field and (not isinstance(field[col], str) or not field[col]):
            errors.append(f"fields[{index}] {col} is empty")
    return errors


def check_keys(doc: dict[str, Any]) -> list[str]:
    """Unique carried keys, unique not-carried keys, and no key in both."""
    errors: list[str] = []
    keys = [
        f["key"]
        for f in doc.get("fields", [])
        if isinstance(f, dict) and isinstance(f.get("key"), str)
    ]
    errors += [f"duplicate key {k}" for k in sorted({k for k in keys if keys.count(k) > 1})]
    absent = doc.get("not_carried")
    if not isinstance(absent, list) or not all(isinstance(k, str) for k in absent):
        return [*errors, "not_carried is not a list of strings"]
    errors += [
        f"duplicate not_carried {k}" for k in sorted({k for k in absent if absent.count(k) > 1})
    ]
    errors += [f"key {k} is carried and not carried" for k in sorted(set(keys) & set(absent))]
    return errors


def check(doc: Any) -> list[str]:
    """Every finding for a parsed field list; empty when it is well formed."""
    if not isinstance(doc, dict):
        return ["the field list is not an object"]
    errors: list[str] = []
    if doc.get("schema") != SCHEMA:
        errors.append(f"schema {doc.get('schema')!r} is not {SCHEMA}")
    if not isinstance(doc.get("seeded_from"), str) or not SEED_RE.match(doc["seeded_from"]):
        errors.append(f"seeded_from {doc.get('seeded_from')!r} is not a pinned registry path")
    fields = doc.get("fields")
    if not isinstance(fields, list) or not fields:
        return [*errors, "fields is not a non-empty list"]
    for index, field in enumerate(fields):
        errors += check_field(index, field)
    return errors + check_keys(doc)


def load() -> dict[str, Any]:
    with FIELD_LIST.open(encoding="utf-8") as handle:
        doc = json.load(handle, object_pairs_hook=refuse_duplicates)
    if not isinstance(doc, dict):
        raise ValueError(f"{FIELD_LIST} is not a JSON object")
    return doc


def refuse_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    seen: dict[str, Any] = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError(f"duplicate JSON key {key}")
        seen[key] = value
    return seen


class FieldListTest(unittest.TestCase):
    def setUp(self) -> None:
        self.doc = load()

    def mutated(self, change: Any) -> list[str]:
        doc = copy.deepcopy(self.doc)
        change(doc)
        return check(doc)

    def assert_finding(self, change: Any, needle: str) -> None:
        errors = self.mutated(change)
        self.assertTrue(any(needle in e for e in errors), errors)

    # --- positive -------------------------------------------------------

    def test_checked_in_file_is_well_formed(self) -> None:
        self.assertEqual(check(self.doc), [])

    def test_duplicate_json_key_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            json.loads('{"schema": 1, "schema": 2}', object_pairs_hook=refuse_duplicates)

    # --- boundary -------------------------------------------------------

    def test_empty_not_carried_passes(self) -> None:
        self.assertEqual(self.mutated(lambda d: d.update(not_carried=[])), [])

    def test_vmafx_only_field_passes(self) -> None:
        row = {
            "key": "frames_since_key",
            "json": "integer",
            "unit": "frame",
            "range": "display distance from the last key frame",
            "pelorus": "vmafx_only",
        }
        self.assertEqual(self.mutated(lambda d: d["fields"].append(row)), [])

    # --- planted negatives ----------------------------------------------

    def test_wrong_schema_fails(self) -> None:
        self.assert_finding(lambda d: d.update(schema="vmafx/stream-metadata-fields/2"), "schema")

    def test_unpinned_seed_fails(self) -> None:
        bad = "VMAFx/pelorus@master:libpelorus/schema/telemetry-fields.json"
        self.assert_finding(lambda d: d.update(seeded_from=bad), "seeded_from")

    def test_empty_fields_fails(self) -> None:
        self.assert_finding(lambda d: d.update(fields=[]), "fields is not a non-empty list")

    def test_duplicate_key_fails(self) -> None:
        self.assert_finding(
            lambda d: d["fields"].append(copy.deepcopy(d["fields"][0])), "duplicate key"
        )

    def test_missing_column_fails(self) -> None:
        self.assert_finding(lambda d: d["fields"][0].pop("unit"), "lacks unit")

    def test_unknown_json_type_fails(self) -> None:
        self.assert_finding(lambda d: d["fields"][0].update(json="int"), "is not a JSON type")

    def test_unknown_pelorus_class_fails(self) -> None:
        self.assert_finding(
            lambda d: d["fields"][0].update(pelorus="encoder_only"), "is not a class"
        )

    def test_non_snake_case_key_fails(self) -> None:
        self.assert_finding(lambda d: d["fields"][0].update(key="DisplayIndex"), "snake_case")

    def test_carried_and_not_carried_fails(self) -> None:
        self.assert_finding(
            lambda d: d["not_carried"].append(d["fields"][0]["key"]), "is carried and not carried"
        )

    def test_duplicate_not_carried_fails(self) -> None:
        self.assert_finding(
            lambda d: d["not_carried"].append(d["not_carried"][0]), "duplicate not_carried"
        )


if __name__ == "__main__":
    unittest.main()

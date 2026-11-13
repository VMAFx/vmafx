# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Acceptance fixtures for api/run-result-timeline.schema.json (issue #2452).

The schema is the skeleton the RC5 run-result timeline grows from; these
fixtures pin the shape the issue's acceptance names:

- the boundary document - every section present, all series empty - validates;
- the positive document, which carries entries in every section, validates;
- the sections the schema leaves open follow the OpenAPI contracts they name;
- a planted event without a time key is refused by that one error alone, and
  the known version validates once the key is restored;
- an unknown schema version is refused by that one error alone, and version 1
  validates once restored.

The byte-equal write-read-write round trip needs the writer/reader C ABI and
lands with it in a later PR of the same issue.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import Any

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parents[3]
SCHEMA_PATH = ROOT / "api" / "run-result-timeline.schema.json"
FIXTURE_DIR = ROOT / "api" / "testdata" / "run-result-timeline"
OPENAPI_PATH = ROOT / "api" / "openapi" / "vmafx-server-v1.yaml"


def openapi_validator(name: str) -> jsonschema.Draft4Validator:
    """A validator for one OpenAPI 3.0 component; its $refs resolve in the same document."""
    components = yaml.safe_load(OPENAPI_PATH.read_text(encoding="utf-8"))["components"]
    return jsonschema.Draft4Validator({**components["schemas"][name], "components": components})


class RunResultTimelineSchemaTests(unittest.TestCase):
    """The shape rules the #2452 skeleton must hold."""

    @classmethod
    def setUpClass(cls) -> None:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        cls.validator: Any = jsonschema.Draft202012Validator(schema)

    def load(self, name: str) -> dict:
        return json.loads((FIXTURE_DIR / name).read_text(encoding="utf-8"))

    def test_valid_minimal_document(self) -> None:
        """Boundary: every section present, all series empty."""
        self.validator.validate(self.load("valid-minimal.json"))

    def test_valid_full_document(self) -> None:
        """Positive: provenance, scoring configuration, frames, windows, events."""
        self.validator.validate(self.load("valid-full.json"))

    def test_full_document_sections_follow_the_openapi_contract(self) -> None:
        """The sections the schema leaves open follow the contract they name."""
        document = self.load("valid-full.json")
        for section, component in (
            ("provenance", "Provenance"),
            ("scoring_configuration", "ScoreOptions"),
        ):
            validator = openapi_validator(component)
            with self.subTest(section=section):
                errors = [
                    f"{e.json_path}: {e.message}" for e in validator.iter_errors(document[section])
                ]
                self.assertEqual(errors, [])
                self.assertEqual(
                    set(document[section]) - set(validator.schema["properties"]), set()
                )

    def test_full_document_keeps_the_rules_the_schema_cannot_state(self) -> None:
        """Frames and events ascend by frame index; every window ends at or after its start."""
        document = self.load("valid-full.json")
        for section in ("frames", "events"):
            with self.subTest(section=section):
                keys = [entry["time"]["frame_index"] for entry in document[section]]
                self.assertEqual(keys, sorted(keys))
        for window in document["windows"]:
            with self.subTest(window=window["statistic"]):
                self.assertEqual(set(window["start"]), set(window["end"]))
                self.assertLessEqual(window["start"]["frame_index"], window["end"]["frame_index"])

    def test_event_without_time_key_refused_by_name(self) -> None:
        """A planted event without a time key is refused, naming the event."""
        document = self.load("invalid-event-without-time.json")
        errors = [
            (e.json_path, e.validator, e.message) for e in self.validator.iter_errors(document)
        ]
        self.assertEqual(errors, [("$.events[0]", "required", "'time' is a required property")])
        document["events"][0]["time"] = {"frame_index": 0}
        self.validator.validate(document)

    def test_unknown_schema_version_refused_by_name(self) -> None:
        """An unknown schema version is refused, naming the member."""
        document = self.load("invalid-unknown-schema-version.json")
        errors = [(e.json_path, e.validator) for e in self.validator.iter_errors(document)]
        self.assertEqual(errors, [("$.schema_version", "const")])
        document["schema_version"] = 1
        self.validator.validate(document)


if __name__ == "__main__":
    unittest.main()

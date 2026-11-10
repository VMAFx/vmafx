# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Acceptance fixtures for api/run-result-timeline.schema.json (issue #2452).

The schema is the skeleton the RC5 run-result timeline grows from; these
fixtures pin the shape the issue's acceptance names:

- the boundary document - every section present, all series empty - validates;
- the positive document, which carries entries in every section, validates;
- a planted event without a time key is refused by name (the refusal points at
  the offending event);
- an unknown schema version is refused by name (the refusal points at
  schema_version).

The byte-equal write-read-write round trip needs the writer/reader C ABI and
lands with it in a later PR of the same issue.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import Any

try:
    import jsonschema

    _HAS_JSONSCHEMA = True
except ImportError:  # the tooling-tests lock installs it; stand-alone runs may not
    _HAS_JSONSCHEMA = False

ROOT = Path(__file__).resolve().parents[3]
SCHEMA_PATH = ROOT / "api" / "run-result-timeline.schema.json"
FIXTURE_DIR = ROOT / "api" / "testdata" / "run-result-timeline"


@unittest.skipUnless(_HAS_JSONSCHEMA, "jsonschema is not installed")
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

    def test_event_without_time_key_refused_by_name(self) -> None:
        """A planted event without a time key is refused, naming the event."""
        errors = list(self.validator.iter_errors(self.load("invalid-event-without-time.json")))
        self.assertTrue(errors, "the event without a time key must be refused")
        named = [
            error
            for error in errors
            if error.json_path.startswith("$.events") and "time" in error.message
        ]
        self.assertTrue(
            named,
            "the refusal must name the offending event and its missing time key; "
            f"got: {[error.json_path for error in errors]}",
        )

    def test_unknown_schema_version_refused_by_name(self) -> None:
        """An unknown schema version is refused, naming the member."""
        errors = list(self.validator.iter_errors(self.load("invalid-unknown-schema-version.json")))
        self.assertTrue(errors, "the unknown schema version must be refused")
        named = [error for error in errors if "schema_version" in error.json_path]
        self.assertTrue(
            named,
            "the refusal must name schema_version; "
            f"got: {[error.json_path for error in errors]}",
        )


if __name__ == "__main__":
    unittest.main()

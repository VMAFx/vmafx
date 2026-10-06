#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Proto-only repeated members of a struct's message (RC4 WP5, ADR-2073).

`proto_repeated` on VmafxProvenance makes the proto `Provenance` message carry
the context's models, features and annotations, which the C struct reaches
through indexed functions. Each member is `repeated <message of another
struct> name = number` with a number from 100, so the field-order numbers of
the scalar fields never meet them. Every rule refuses its planted defect; the
live output compiles with protoc and the OpenAPI schema refers to the item
message.
"""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path
from typing import Any

from support import document, entry, run, tool
from vmafx_api import emit_openapi, emit_proto
from vmafx_api.loader import parse
from vmafx_api.model import DefinitionError


def provenance(doc: dict[str, Any]) -> dict[str, Any]:
    found: dict[str, Any] = entry(doc["structs"], "VmafxProvenance")
    return found


def refused(change: Any, text: str) -> None:
    doc = copy.deepcopy(document())
    change(doc)
    case = unittest.TestCase()
    with case.assertRaisesRegex(DefinitionError, text):
        emit_proto.proto_text(parse(doc))


class ProtoRepeatedTest(unittest.TestCase):
    def test_live_messages(self) -> None:
        text = emit_proto.proto_text(parse(document()))
        self.assertIn("  repeated ModelProvenance models = 100;", text)
        self.assertIn("  repeated FeatureProvenance features = 101;", text)
        self.assertIn("  repeated Annotation annotations = 102;", text)
        self.assertIn("message ModelProvenance {", text)

    def test_unknown_struct_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            provenance(doc)["proto_repeated"][0]["struct"] = "VmafxNoSuch"

        refused(change, "not a struct with a proto message")

    def test_struct_without_message_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            del entry(doc["structs"], "VmafxAnnotation")["proto"]

        refused(change, "not a struct with a proto message")

    def test_low_number_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            provenance(doc)["proto_repeated"][0]["number"] = 50

        refused(change, "number 50 is outside")

    def test_duplicate_number_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            provenance(doc)["proto_repeated"][1]["number"] = 100

        refused(change, "number 100 twice")

    def test_name_of_a_field_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            provenance(doc)["proto_repeated"][0]["name"] = "version"

        refused(change, "name version twice")

    def test_repeated_without_message_is_refused(self) -> None:
        def change(doc: dict[str, Any]) -> None:
            entry(doc["structs"], "VmafxScore")["proto_repeated"] = [
                {"name": "models", "struct": "VmafxModelProvenance", "number": 100}
            ]

        refused(change, "proto_repeated without a proto message")

    def test_openapi_refers_to_the_item_message(self) -> None:
        schemas = emit_openapi.schemas(parse(document()))
        models = schemas["Provenance"]["properties"]["models"]
        self.assertEqual(models["type"], "array")
        self.assertEqual(models["items"], {"$ref": "#/components/schemas/ModelProvenance"})
        self.assertIn("ModelProvenance", schemas)

    def test_protoc_compiles_the_live_messages(self) -> None:
        protoc = tool("protoc")
        if protoc is None:
            self.skipTest("protoc not on PATH")
        with tempfile.TemporaryDirectory() as tmp:
            text = emit_proto.proto_text(parse(document()))
            (Path(tmp) / "vmafx_api.proto").write_text(text)
            result = run([protoc, f"-I{tmp}", "--descriptor_set_out=/dev/null", "vmafx_api.proto"])
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

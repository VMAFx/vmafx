#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The provenance record in the vmaf CLI's reports (#2142, ADR-2073, RC4 WP5).

Usage: test_vmaf_provenance_report.py <vmaf binary> <repository root>

- JSON: the `provenance` object has one member per field of VmafxProvenance
  in core/api/vmafx.toml and its `proto_repeated` arrays, each item one member
  per field of its struct, in the proto JSON mapping (64-bit integers as
  strings); the model digest equals the SHA-256 of the model file the built-in
  model is made from; the record digest equals an independent RFC 8785
  canonicalisation of the record (Python's sorted, compact JSON, which is the
  same text for ASCII keys, integers and strings); the score digest equals one
  recomputed from the lossless per-frame scores.
- XML: one `<provenance>` element with one attribute per field and a
  `<model>`, `<feature>` or `<annotation>` child per item.
- CSV: the columns of the CSV writer, no provenance; the sidecar holds the
  record when asked.

Failing first: on the WP8 base the JSON record has six members and XML none.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, ClassVar
from xml.etree import ElementTree

import tomllib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from provenance_fixture import MODEL, score

ARGC = 3  # program, vmaf binary, repository root
TIMING_AND_SELF = ("digest", "elapsed_ns")


def struct_fields(definition: dict[str, Any], name: str) -> list[dict[str, Any]]:
    entry = next(s for s in definition["structs"] if s["name"] == name)
    fields: list[dict[str, Any]] = entry["fields"]
    return fields


def check_types(
    case: unittest.TestCase, record: dict[str, Any], fields: list[dict[str, Any]]
) -> None:
    for field in fields:
        value = record[field["name"]]
        if field.get("enum") or field["type"] == "cstr":
            case.assertIsInstance(value, str, field["name"])
        elif field["type"] in ("u64", "i64"):
            case.assertIsInstance(value, str, field["name"])
            case.assertTrue(value.lstrip("-").isdigit(), field["name"])
        else:
            case.assertIsInstance(value, int, field["name"])


def canonical(record: dict[str, Any]) -> bytes:
    body = {k: v for k, v in record.items() if k not in TIMING_AND_SELF}
    return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def scores_digest(report: dict[str, Any]) -> str:
    lines = []
    for frame in report["frames"]:
        for name, value in frame["metrics"].items():
            bits = struct.unpack("<Q", struct.pack("<d", value))[0]
            lines.append((name, frame["frameNum"], bits))
    lines.sort(key=lambda line: (line[0].encode(), line[1]))
    text = "".join(f"{name} {index} {bits:016x}\n" for name, index, bits in lines)
    return "sha256:" + hashlib.sha256(text.encode()).hexdigest()


class ProvenanceReportTest(unittest.TestCase):
    definition: ClassVar[dict[str, Any]] = {}
    vmaf: ClassVar[str] = ""
    root: ClassVar[Path] = Path()

    @classmethod
    def setUpClass(cls) -> None:
        with (cls.root / "core" / "api" / "vmafx.toml").open("rb") as handle:
            cls.definition = tomllib.load(handle)

    def run_vmaf(self, directory: Path, name: str, *extra: str) -> Path:
        output = directory / name
        result = score(self.vmaf, directory, output, *extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        return output

    def test_json_record_follows_the_definition(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = json.loads(
                self.run_vmaf(Path(tmp), "r.json", "--json", "--precision", "max").read_text()
            )
        record = report["provenance"]
        fields = list(struct_fields(self.definition, "VmafxProvenance"))
        prov = next(s for s in self.definition["structs"] if s["name"] == "VmafxProvenance")
        repeated = {m["name"]: m["struct"] for m in prov["proto_repeated"]}
        self.assertEqual(set(record), {f["name"] for f in fields} | set(repeated))
        check_types(self, record, fields)
        for member, struct_name in repeated.items():
            item_fields = struct_fields(self.definition, struct_name)
            self.assertTrue(record[member], member)
            for item in record[member]:
                self.assertEqual(set(item), {f["name"] for f in item_fields}, member)
                check_types(self, item, item_fields)
        self.assertEqual(record["models"][0]["version"], MODEL)
        model_file = self.root / "model" / f"{MODEL}.json"
        self.assertEqual(
            record["models"][0]["sha256"], hashlib.sha256(model_file.read_bytes()).hexdigest()
        )
        self.assertEqual(
            record["digest"], "sha256:" + hashlib.sha256(canonical(record)).hexdigest()
        )
        self.assertEqual(record["scores_digest"], scores_digest(report))
        self.assertEqual(report["score_format"], "%.17g")
        self.assertEqual(report["backend_used"], "cpu")
        self.assertTrue(report["feature_backends"])

    def test_xml_element(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            written = self.run_vmaf(Path(tmp), "r.xml", "--xml")
            # The report the binary under test just wrote, not outside input.
            root = ElementTree.parse(written).getroot()  # noqa: S314
        element = root.find("provenance")
        self.assertIsNotNone(element)
        assert element is not None
        fields = struct_fields(self.definition, "VmafxProvenance")
        self.assertEqual(list(element.attrib), [f["name"] for f in fields])
        children = {child.tag for child in element}
        self.assertEqual(children, {"model", "feature", "annotation"})
        model = element.find("model")
        assert model is not None
        self.assertEqual(model.attrib["version"], MODEL)
        # The harness still finds what it reads.
        self.assertIsNotNone(root.find("frames"))
        self.assertIsNotNone(root.find("pooled_metrics"))

    def test_csv_unchanged_and_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            plain = self.run_vmaf(directory, "plain.csv", "--csv").read_text()
            self.assertFalse((directory / "plain.csv.provenance.json").exists())
            self.run_vmaf(directory, "side.csv", "--csv", "--provenance-sidecar")
            side = (directory / "side.csv").read_text()
            sidecar = json.loads((directory / "side.csv.provenance.json").read_text())
        self.assertTrue(plain.startswith("Frame,"))
        self.assertNotIn("provenance", plain)
        self.assertEqual(plain, side, "the sidecar changed the CSV")
        self.assertEqual(sidecar["models"][0]["version"], MODEL)
        self.assertEqual(
            sidecar["digest"], "sha256:" + hashlib.sha256(canonical(sidecar)).hexdigest()
        )


def main() -> int:
    if len(sys.argv) != ARGC:
        print(__doc__)
        return 2
    ProvenanceReportTest.vmaf = sys.argv[1]
    ProvenanceReportTest.root = Path(sys.argv[2])
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProvenanceReportTest)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The platform definition and its protobuf emitter (ADR-2350 D13)."""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path
from typing import Any

from support import ROOT, document, run, tool
from vmafx_api import cli, emit_platform_proto, emit_proto
from vmafx_api.loader import load
from vmafx_api.model import DefinitionError
from vmafx_api.platform import (
    DEFINITION,
    FIELD_MAX,
    External,
    Platform,
    external_messages,
    parse,
)

CORE = load(ROOT / "core" / "api" / "vmafx.toml")
EXTERNAL = external_messages(
    CORE, emit_proto.IMPORT_PATH, emit_proto.PACKAGE, emit_proto.GO_PACKAGE
)


def real() -> dict[str, Any]:
    data: dict[str, Any] = document(ROOT / DEFINITION)
    return data


def _mini_files() -> list[dict[str, Any]]:
    return [
        {
            "name": name,
            "path": f"proto/demo/{name}/v1/{name}.proto",
            "package": f"demo.{name}.v1",
            "go_package": f"example.com/{name};{name}v1",
            "doc": f"File {name}.",
        }
        for name in ("a", "b")
    ]


def _mini_messages() -> list[dict[str, Any]]:
    thing = [
        {"name": "id", "type": "string", "number": 1, "doc": "Id."},
        {"name": "colour", "type": "Colour", "number": 2},
        {"name": "scores", "type": "map<string, double>", "number": 3},
        {"name": "tags", "type": "string", "number": 4, "repeated": True},
        {"name": "x", "type": "int32", "number": 5, "oneof": "pick"},
        {"name": "y", "type": "string", "number": 6, "oneof": "pick"},
    ]
    box = [{"name": "thing", "type": "Thing", "number": 1}]
    return [
        {"name": "Thing", "file": "a", "doc": "A thing.", "fields": thing},
        {"name": "Box", "file": "b", "doc": "Holds a thing.", "fields": box},
    ]


def _mini_services() -> list[dict[str, Any]]:
    rpcs = [
        {"name": "Get", "request": "Box", "response": "Thing", "doc": "Get it."},
        {
            "name": "Watch",
            "request": "Box",
            "response": "Box",
            "doc": "Watch it.",
            "client_stream": True,
            "server_stream": True,
        },
    ]
    return [{"name": "Boxes", "file": "b", "doc": "Box service.", "rpcs": rpcs}]


def mini() -> dict[str, Any]:
    """A two-file definition: a service, an enum, a map, a oneof and an import."""
    values = [{"name": "COLOUR_UNSPECIFIED", "number": 0}, {"name": "COLOUR_RED", "number": 1}]
    return {
        "platform": {"version": 1},
        "files": _mini_files(),
        "enums": [{"name": "Colour", "file": "a", "doc": "A colour.", "values": values}],
        "messages": _mini_messages(),
        "services": _mini_services(),
    }


def platform(data: dict[str, Any], external: tuple[External, ...] = ()) -> Platform:
    return parse(data, external)


class RealDefinitionTest(unittest.TestCase):
    def test_parses_and_renders_both_services(self) -> None:
        files = emit_platform_proto.files(platform(real(), EXTERNAL))
        self.assertEqual(
            sorted(files),
            ["proto/vmafx/controller/v1/controller.proto", "proto/vmafx/v1/vmafx.proto"],
        )
        scoring = files["proto/vmafx/v1/vmafx.proto"]
        self.assertIn('import "vmafx/v1/vmafx_api.proto";', scoring)
        self.assertIn(
            "rpc ScoreStream(stream ScoreStreamRequest) returns (stream ScoreStreamResponse);",
            scoring,
        )
        controller = files["proto/vmafx/controller/v1/controller.proto"]
        self.assertIn("rpc StreamJobs(StreamJobsRequest) returns (stream Job);", controller)
        self.assertIn("  map<string, double> features = 6;", controller)

    def test_committed_files_match(self) -> None:
        files = cli.render(CORE, None, cli.platform_of(CORE, ROOT))
        for path in ("proto/vmafx/v1/vmafx.proto", "proto/vmafx/controller/v1/controller.proto"):
            with self.subTest(path=path):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8"), files[path])

    def test_protoc_compiles_the_generated_files(self) -> None:
        protoc = tool("protoc")
        if protoc is None:
            self.skipTest("protoc not on PATH")
        files = cli.render(CORE, None, cli.platform_of(CORE, ROOT))
        with tempfile.TemporaryDirectory() as tmp:
            for path, text in files.items():
                if path.startswith("proto/") and path.endswith(".proto"):
                    target = Path(tmp) / path.removeprefix("proto/")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(text)
            result = run(
                [
                    protoc,
                    f"-I{tmp}",
                    "--descriptor_set_out=/dev/null",
                    "vmafx/v1/vmafx.proto",
                    "vmafx/controller/v1/controller.proto",
                ]
            )
        self.assertEqual(result.returncode, 0, result.stderr)


class EmitterTest(unittest.TestCase):
    def test_oneof_map_stream_and_cross_package_reference(self) -> None:
        files = emit_platform_proto.files(platform(mini()))
        a, b = files["proto/demo/a/v1/a.proto"], files["proto/demo/b/v1/b.proto"]
        self.assertIn("  oneof pick {\n    int32 x = 5;\n    string y = 6;\n  }", a)
        self.assertIn("  map<string, double> scores = 3;", a)
        self.assertIn("  repeated string tags = 4;", a)
        self.assertIn('import "demo/a/v1/a.proto";', b)
        self.assertIn("  demo.a.v1.Thing thing = 1;", b)
        self.assertIn("rpc Get(Box) returns (demo.a.v1.Thing);", b)
        self.assertIn("rpc Watch(stream Box) returns (stream Box);", b)
        self.assertNotIn("import", a.split("option go_package")[0].split("package demo.a.v1;")[1])

    def test_comment_keeps_line_breaks_and_wraps(self) -> None:
        lines = emit_platform_proto.comment("One.\n\n  * two " + "x" * 120, "  ")
        self.assertEqual(lines[:2], ["  // One.", "  //"])
        self.assertTrue(lines[2].startswith("  //   * two"))
        self.assertTrue(all(len(line) <= emit_platform_proto.WIDTH for line in lines))


class RefusalTest(unittest.TestCase):
    def refused(self, change: Any, pattern: str) -> None:
        data = copy.deepcopy(mini())
        change(data)
        with self.assertRaisesRegex(DefinitionError, pattern):
            platform(data)

    def test_duplicate_field_number(self) -> None:
        self.refused(lambda d: d["messages"][0]["fields"][1].update(number=1), "field number: 1")

    def test_unknown_type(self) -> None:
        self.refused(lambda d: d["messages"][0]["fields"][0].update(type="Nope"), "type Nope")

    def test_bad_map_key(self) -> None:
        self.refused(
            lambda d: d["messages"][0]["fields"][2].update(type="map<double, string>"), "map key"
        )

    def test_repeated_oneof_member(self) -> None:
        self.refused(lambda d: d["messages"][0]["fields"][4].update(repeated=True), "oneof member")

    def test_enum_without_zero(self) -> None:
        self.refused(lambda d: d["enums"][0]["values"][0].update(number=3), "number 0")

    def test_path_outside_package_directory(self) -> None:
        self.refused(lambda d: d["files"][0].update(path="proto/a.proto"), "must lie directly")

    def test_unknown_file(self) -> None:
        self.refused(lambda d: d["messages"][1].update(file="zzz"), "not in \\[\\[files\\]\\]")

    def test_rpc_with_a_scalar(self) -> None:
        self.refused(
            lambda d: d["services"][0]["rpcs"][0].update(request="string"), "not a message"
        )

    def test_duplicate_name(self) -> None:
        self.refused(lambda d: d["messages"][1].update(name="Thing"), "Thing appears twice")

    def test_format_version(self) -> None:
        self.refused(lambda d: d["platform"].update(version=2), "version must be 1")

    def test_go_package_of_a_shared_package(self) -> None:
        shared = (External("Ext", "demo/a/v1/ext.proto", "demo.a.v1", "example.com/other;x"),)
        with self.assertRaisesRegex(DefinitionError, "go_package must match"):
            platform(mini(), shared)


class BoundaryTest(unittest.TestCase):
    def test_field_numbers(self) -> None:
        for number, ok in ((1, True), (FIELD_MAX, True), (0, False), (FIELD_MAX + 1, False)):
            with self.subTest(number=number):
                data = copy.deepcopy(mini())
                data["messages"][0]["fields"][0]["number"] = number
                if ok:
                    platform(data)
                else:
                    with self.assertRaisesRegex(DefinitionError, "not a valid field number"):
                        platform(data)

    def test_reserved_range(self) -> None:
        for number, ok in ((18_999, True), (19_000, False), (19_999, False), (20_000, True)):
            with self.subTest(number=number):
                data = copy.deepcopy(mini())
                data["messages"][0]["fields"][0]["number"] = number
                if ok:
                    platform(data)
                else:
                    with self.assertRaises(DefinitionError):
                        platform(data)

    def test_empty_message(self) -> None:
        data = copy.deepcopy(mini())
        data["messages"][1]["fields"] = []
        text = emit_platform_proto.files(platform(data))["proto/demo/b/v1/b.proto"]
        self.assertIn("message Box {}", text)


if __name__ == "__main__":
    unittest.main()

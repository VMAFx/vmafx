# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The CRD compatibility check and the controller-gen runner (ADR-2350 D13).

crd_compat: an identical CRD and every widening pass; every planted narrowing
is reported with its path. crd_generate: with controller-gen replaced by the
committed tree, --check passes, and fails naming the file on a hand edit, a
missing or an extra file; --write replaces only the generated files; a
compatibility finding fails the check; no Go or no base exits 77 in check
mode. The real controller-gen run is the Meson test test_crd_generated_current.
"""

from __future__ import annotations

import copy
import io
import sys
import tempfile
import unittest
from collections.abc import Callable
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any
from unittest import mock

import yaml  # type: ignore[import-untyped]
from support import ROOT
from vmafx_api import crd_compat, gitref

sys.path.insert(0, str(ROOT / "scripts" / "codegen"))
import crd_generate

Schema = dict[str, Any]


def _spec_schema() -> Schema:
    return {
        "type": "object",
        "required": ["size"],
        "properties": {
            "size": {"type": "integer", "format": "int32", "minimum": 0, "default": 1},
            "mode": {"type": "string", "enum": ["a", "b"]},
            "name": {"type": "string", "maxLength": 10},
            "note": {"type": "string"},
            "labels": {"type": "object", "additionalProperties": {"type": "string"}},
            "tags": {"type": "array", "maxItems": 8, "items": {"type": "string", "minLength": 1}},
            "free": {"type": "object", "x-kubernetes-preserve-unknown-fields": True},
        },
    }


def crd() -> Schema:
    version = {
        "name": "v1",
        "served": True,
        "storage": True,
        "subresources": {"status": {}},
        "schema": {"openAPIV3Schema": {"type": "object", "properties": {"spec": _spec_schema()}}},
    }
    names = {
        "kind": "Widget",
        "listKind": "WidgetList",
        "plural": "widgets",
        "singular": "widget",
        "shortNames": ["wd"],
    }
    return {
        "metadata": {"name": "widgets.demo.dev"},
        "spec": {"names": names, "scope": "Namespaced", "versions": [version]},
    }


def spec(doc: Schema) -> Schema:
    found: Schema = doc["spec"]["versions"][0]["schema"]["openAPIV3Schema"]["properties"]["spec"]
    return found


def prop(doc: Schema, name: str) -> Schema:
    found: Schema = spec(doc)["properties"][name]
    return found


Mutation = Callable[[Schema], object]

NARROWINGS: list[tuple[str, Mutation, str]] = [
    ("removed property", lambda d: spec(d)["properties"].pop("mode"), ".spec.mode: removed"),
    ("newly required", lambda d: spec(d)["required"].append("note"), ".spec.note: newly required"),
    ("enum value lost", lambda d: prop(d, "mode").update(enum=["a"]), "enum narrowed (lost ['b'])"),
    ("enum added", lambda d: prop(d, "note").update(enum=["x"]), "enum narrowed (lost any value)"),
    ("minimum raised", lambda d: prop(d, "size").update(minimum=1), "minimum 0 -> 1"),
    ("maxLength lowered", lambda d: prop(d, "name").update(maxLength=5), "maxLength 10 -> 5"),
    ("new maximum", lambda d: prop(d, "size").update(maximum=9), "maximum none -> 9"),
    ("maxItems lowered", lambda d: prop(d, "tags").update(maxItems=4), "maxItems 8 -> 4"),
    ("default changed", lambda d: prop(d, "size").update(default=2), "default 1 -> 2"),
    ("default dropped", lambda d: prop(d, "size").pop("default"), "default 1 -> 'none'"),
    ("type changed", lambda d: prop(d, "note").update(type="integer"), "type string -> integer"),
    ("format added", lambda d: prop(d, "note").update(format="uri"), "format none -> uri"),
    ("format changed", lambda d: prop(d, "size").update(format="int64"), "format int32 -> int64"),
    ("pattern added", lambda d: prop(d, "note").update(pattern="^a"), "pattern none -> ^a"),
    ("items bound raised", lambda d: prop(d, "tags")["items"].update(minLength=2),
     ".spec.tags[]: minLength 1 -> 2"),
    ("map value type", lambda d: prop(d, "labels")["additionalProperties"].update(type="integer"),
     ".spec.labels{}: type string -> integer"),
    ("unknown fields", lambda d: prop(d, "free").pop("x-kubernetes-preserve-unknown-fields"),
     ".spec.free: unknown fields no longer preserved"),
    ("version removed", lambda d: d["spec"]["versions"][0].update(name="v2"), "v1: version removed"),
    ("not served", lambda d: d["spec"]["versions"][0].update(served=False), "no longer served"),
    ("status lost", lambda d: d["spec"]["versions"][0].update(subresources={}),
     "status subresource removed"),
    ("short name lost", lambda d: d["spec"]["names"].update(shortNames=[]), "short name wd removed"),
    ("scope", lambda d: d["spec"].update(scope="Cluster"), "scope Namespaced -> Cluster"),
    ("kind", lambda d: d["spec"]["names"].update(kind="Gadget"), "names.kind Widget -> Gadget"),
]  # fmt: skip

WIDENINGS: list[tuple[str, Mutation]] = [
    ("optional property added", lambda d: spec(d)["properties"].update(extra={"type": "string"})),
    ("enum value added", lambda d: prop(d, "mode").update(enum=["a", "b", "c"])),
    ("minimum lowered", lambda d: prop(d, "size").update(minimum=-1)),
    ("maxLength dropped", lambda d: prop(d, "name").pop("maxLength")),
    ("format dropped", lambda d: prop(d, "size").pop("format")),
    ("required dropped", lambda d: spec(d).update(required=[])),
    ("description added", lambda d: prop(d, "note").update(description="A note.")),
    ("short name added", lambda d: d["spec"]["names"].update(shortNames=["wd", "wdg"])),
    ("version added", lambda d: d["spec"]["versions"].append(dict(d["spec"]["versions"][0], name="v2"))),
]  # fmt: skip


def mutated(mutation: Mutation) -> Schema:
    doc = crd()
    mutation(doc)
    return doc


class CrdCompatTest(unittest.TestCase):
    def test_identical_and_widened_crds_pass(self) -> None:
        self.assertEqual(crd_compat.findings([crd()], [crd()]), [])
        for label, mutation in WIDENINGS:
            with self.subTest(label):
                self.assertEqual(crd_compat.findings([crd()], [mutated(mutation)]), [])

    def test_every_planted_narrowing_is_reported(self) -> None:
        for label, mutation, want in NARROWINGS:
            with self.subTest(label):
                found = crd_compat.findings([crd()], [mutated(mutation)])
                self.assertTrue(any(want in f for f in found), found)

    def test_removed_crd_is_reported(self) -> None:
        self.assertEqual(crd_compat.findings([crd()], []), ["widgets.demo.dev: CRD removed"])

    def test_new_crd_is_free(self) -> None:
        self.assertEqual(crd_compat.findings([], [crd()]), [])

    def test_nesting_is_bounded(self) -> None:
        deep: Schema = {"type": "string"}
        for _ in range(crd_compat.MAX_DEPTH + 2):
            deep = {"type": "object", "properties": {"x": deep}}
        found = crd_compat.schema_findings(deep, copy.deepcopy(deep))
        self.assertTrue(any("nested deeper than" in f for f in found), found)
        shallow = deep["properties"]["x"]["properties"]["x"]["properties"]["x"]
        self.assertEqual(crd_compat.schema_findings(shallow, copy.deepcopy(shallow)), [])

    def test_walk_is_bounded(self) -> None:
        with mock.patch.object(crd_compat, "MAX_NODES", 3):
            found = crd_compat.findings([crd()], [crd()])
        self.assertTrue(any("more than 3 schema nodes" in f for f in found), found)

    def test_committed_chart_crds_are_compatible_with_themselves(self) -> None:
        texts = [
            p.read_text(encoding="utf-8") for p in sorted((ROOT / crd_generate.CRDS).glob("*.yaml"))
        ]
        docs = crd_generate.crds_of(texts)
        self.assertEqual(len(docs), 4)
        self.assertEqual(crd_compat.findings(docs, copy.deepcopy(docs)), [])


def quiet_main(*argv: str) -> tuple[int, str]:
    out = io.StringIO()
    with redirect_stdout(out):
        code = crd_generate.main(list(argv))
    return code, out.getvalue()


class CrdGenerateTest(unittest.TestCase):
    """crd_generate with controller-gen replaced by a fixed result."""

    def generated(self, change: Callable[[dict[str, str]], object] = lambda f: None) -> Any:
        files = crd_generate.committed(ROOT)
        change(files)
        return mock.patch.object(crd_generate, "generate", return_value=files)

    def test_committed_tree_passes(self) -> None:
        with self.generated():
            self.assertEqual(quiet_main("--check", "--root", str(ROOT)), (0, ""))

    def test_hand_edit_missing_and_extra_files_fail(self) -> None:
        crds = crd_generate.CRDS.as_posix()
        role = (crd_generate.RBAC / "role.yaml").as_posix()

        def edit(files: dict[str, str]) -> None:
            files[f"{crds}/vmafx.dev_vmafxjobs.yaml"] += "# hand edit\n"
            files[f"{crds}/vmafx.dev_vmafxwidgets.yaml"] = "kind: CustomResourceDefinition\n"
            del files[role]

        with self.generated(edit):
            code, out = quiet_main("--check", "--root", str(ROOT))
        self.assertEqual(code, 1)
        for path in (
            f"{crds}/vmafx.dev_vmafxjobs.yaml",
            f"{crds}/vmafx.dev_vmafxwidgets.yaml",
            role,
        ):
            self.assertIn(path, out)
        self.assertIn("never edit generated files", out)

    def test_compat_finding_fails_the_check(self) -> None:
        crds = crd_generate.CRDS.as_posix()
        old = {
            p: t + "" for p, t in crd_generate.committed(ROOT).items() if p.startswith(crds + "/")
        }
        job = yaml.safe_load(old[f"{crds}/vmafx.dev_vmafxjobs.yaml"].split("---", 1)[-1])
        props = job["spec"]["versions"][0]["schema"]["openAPIV3Schema"]["properties"]["spec"]
        props["properties"]["retired"] = {"type": "string"}
        old[f"{crds}/vmafx.dev_vmafxjobs.yaml"] = yaml.safe_dump(job)
        with self.generated(), mock.patch.object(gitref, "files_at", return_value=old):
            code, out = quiet_main("--check", "--compat-against", "BASE", "--root", str(ROOT))
        self.assertEqual(code, 1)
        self.assertIn("compat with BASE: vmafxjobs.vmafx.dev/v1 .spec.retired: removed", out)
        self.assertIn("only grows", out)
        self.assertNotIn("never edit generated files", out)

    def test_unavailable_tool_or_base_skips_check_and_fails_write(self) -> None:
        missing = mock.patch.object(
            crd_generate, "generate", side_effect=crd_generate.Unavailable("go is not installed")
        )
        with missing:
            code, out = quiet_main("--check", "--root", str(ROOT))
            self.assertEqual(code, crd_generate.SKIP)
            self.assertIn("SKIP: go is not installed", out)
            with self.assertRaises(SystemExit):
                quiet_main("--write", "--root", str(ROOT))
        no_base = mock.patch.object(
            gitref, "merge_base", side_effect=gitref.Unavailable("unknown ref")
        )
        with no_base, self.generated():
            code, _ = quiet_main(
                "--check", "--compat-against-merge-base", "nope", "--root", str(ROOT)
            )
        self.assertEqual(code, crd_generate.SKIP)

    def test_write_replaces_only_generated_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keep = root / "api/vmafx/v1/deepcopy_test.go"
            stale = root / crd_generate.CRDS / "vmafx.dev_retired.yaml"
            for path in (keep, stale, root / "api/vmafx/v1/zz_generated.deepcopy.go"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("old\n", encoding="utf-8")
            want = {
                "api/vmafx/v1/zz_generated.deepcopy.go": "new\n",
                f"{crd_generate.CRDS.as_posix()}/vmafx.dev_widgets.yaml": "new\n",
            }
            with mock.patch.object(crd_generate, "generate", return_value=want):
                self.assertEqual(quiet_main("--write", "--root", str(root))[0], 0)
            self.assertEqual(keep.read_text(encoding="utf-8"), "old\n")
            self.assertFalse(stale.exists())
            for path, text in want.items():
                self.assertEqual((root / path).read_text(encoding="utf-8"), text)


class PackagePatternTest(unittest.TestCase):
    """controller-gen gets import path patterns: controller-tools turns a
    filesystem root ("./x/...") into ".<separator>...", which `go list` on
    Windows reads as one package, and the role lost the controllers' markers."""

    def test_generate_passes_import_path_patterns(self) -> None:
        calls: list[tuple[str, ...]] = []
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(crd_generate, "controller_gen", lambda _root, *a: calls.append(a)),
        ):
            crd_generate.generate(ROOT, Path(tmp))
        patterns = [
            arg[len("paths=") :] for call in calls for arg in call if arg.startswith("paths=")
        ]
        self.assertEqual(len(patterns), 3)
        for pattern in patterns:
            self.assertTrue(pattern.startswith("github.com/VMAFx/vmafx/"), pattern)
            self.assertTrue(pattern.endswith("/..."), pattern)
        self.assertIn("github.com/VMAFx/vmafx/cmd/vmafx-operator/...", patterns)

    def test_pattern_follows_the_module_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "go.mod").write_text(
                "// x\nmodule example.org/m\n\ngo 1.27\n", encoding="utf-8"
            )
            self.assertEqual(crd_generate.package_pattern(root, "a/..."), "example.org/m/a/...")
            (root / "go.mod").write_text("go 1.27\n", encoding="utf-8")
            with self.assertRaises(SystemExit):
                crd_generate.package_pattern(root, "a/...")


if __name__ == "__main__":
    unittest.main()

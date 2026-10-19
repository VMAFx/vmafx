# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Kubernetes subset of the chart's values schema (ADR-2350 D13).

Offline: the committed subset names the pinned release and digests, holds
exactly the types the definition reaches and resolves every reference in
itself. The transform drops descriptions and extensions and rewrites
references; the closure refuses a missing type. With the download replaced:
a digest that differs is refused, a failed download skips --check (77), a
stale subset fails it, --write rewrites it.
"""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any
from unittest import mock

from support import ROOT
from vmafx_api.chart import schema_refs

sys.path.insert(0, str(ROOT / "scripts" / "codegen"))
import k8s_openapi
from sqlc_generate import build_config


def committed() -> dict[str, Any]:
    document: dict[str, Any] = json.loads((ROOT / k8s_openapi.SUBSET).read_text())
    return document


def quiet(*argv: str) -> int:
    with redirect_stdout(io.StringIO()):
        code: int = k8s_openapi.main(list(argv))
    return code


class CommittedSubsetTest(unittest.TestCase):
    def test_names_the_pinned_release(self) -> None:
        document = committed()
        version, digests = k8s_openapi.pins(ROOT)
        self.assertEqual(document["kubernetes"], version)
        self.assertEqual([s["sha256"] for s in document["sources"]], list(digests.values()))
        for source in document["sources"]:
            self.assertIn(f"/v{version}/", source["url"])

    def test_holds_exactly_the_reached_types(self) -> None:
        schemas = committed()["schemas"]
        reached = k8s_openapi.subset(k8s_openapi.roots(ROOT), schemas)
        self.assertEqual(sorted(reached), sorted(schemas))
        for name, schema in schemas.items():
            for ref in schema_refs(schema):
                self.assertIn(ref[len(k8s_openapi.DEFS) :], schemas, name)
            self.assertNotIn('"description"', json.dumps(schema), name)

    def test_notice_names_the_release_and_carries_the_licence(self) -> None:
        notice = (ROOT / k8s_openapi.NOTICE).read_text(encoding="utf-8")
        self.assertEqual(notice, k8s_openapi.notice(ROOT))
        self.assertIn(f"kubernetes/kubernetes v{committed()['kubernetes']}", notice)
        self.assertIn("Copyright The Kubernetes Authors.", notice)
        self.assertTrue(
            notice.endswith((ROOT / k8s_openapi.APACHE_TEXT).read_text(encoding="utf-8"))
        )

    def test_minimum_supported_minor_is_the_documented_one(self) -> None:
        guide = (ROOT / "docs/development/k8s-deployment.md").read_text(encoding="utf-8")
        version = build_config(ROOT / "build-config.env")["K8S_SCHEMA_VERSION"]
        minor = ".".join(version.split(".")[:2])
        self.assertIn(f"A Kubernetes cluster ({minor}+)", guide)


SPEC = {
    "A": {
        "description": "a",
        "x-kubernetes-map-type": "atomic",
        "properties": {"b": {"allOf": [{"$ref": "#/components/schemas/B"}], "description": "x"}},
    },
    "B": {"type": "object", "properties": {"c": {"$ref": "#/components/schemas/C"}}},
    "C": {"type": "string"},
    "D": {"type": "integer"},
}


class TransformTest(unittest.TestCase):
    def test_cleaned_drops_documentation_and_rewrites_references(self) -> None:
        self.assertEqual(
            k8s_openapi.cleaned(SPEC["A"]),
            {"properties": {"b": {"allOf": [{"$ref": "#/$defs/B"}]}}},
        )

    def test_subset_is_the_closure(self) -> None:
        self.assertEqual(list(k8s_openapi.subset(["A"], SPEC)), ["A", "B", "C"])
        self.assertEqual(list(k8s_openapi.subset(["C", "D"], SPEC)), ["C", "D"])
        with self.assertRaises(SystemExit):
            k8s_openapi.subset(["Z"], SPEC)


def response(data: bytes) -> mock.MagicMock:
    handle = mock.MagicMock()
    handle.__enter__.return_value.read.return_value = data
    return handle


class DownloadTest(unittest.TestCase):
    payload = json.dumps({"components": {"schemas": {"C": {"type": "string"}}}}).encode()

    def test_digest_is_checked(self) -> None:
        good = hashlib.sha256(self.payload).hexdigest()
        with mock.patch("urllib.request.urlopen", return_value=response(self.payload)):
            self.assertEqual(
                k8s_openapi.download("1.26.15", "CORE_V1", good), {"C": {"type": "string"}}
            )
            with self.assertRaises(SystemExit):
                k8s_openapi.download("1.26.15", "CORE_V1", "0" * 64)

    def test_failed_download_skips_the_check(self) -> None:
        with mock.patch("urllib.request.urlopen", side_effect=OSError("offline")):
            self.assertEqual(quiet("--check", "--root", str(ROOT)), k8s_openapi.SKIP)
            with self.assertRaises(SystemExit):
                quiet("--write", "--root", str(ROOT))

    def test_conflicting_definitions_are_refused(self) -> None:
        answers = iter([{"C": {"type": "string"}}, {"C": {"type": "integer"}}, {}])
        with mock.patch.object(k8s_openapi, "download", side_effect=lambda *a: next(answers)):
            with self.assertRaises(SystemExit):
                k8s_openapi.merged("1.26.15", dict.fromkeys(k8s_openapi.FILES, ""))

    def test_stale_subset_fails_and_write_repairs_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / k8s_openapi.SUBSET).parent.mkdir(parents=True)
            (root / k8s_openapi.SUBSET).write_text("{}\n")
            fresh = {k8s_openapi.SUBSET: '{"fresh": 1}\n'}
            with mock.patch.object(k8s_openapi, "outputs", return_value=fresh):
                self.assertEqual(quiet("--check", "--root", tmp), 1)
                self.assertEqual(quiet("--write", "--root", tmp), 0)
                self.assertEqual(quiet("--check", "--root", tmp), 0)
            self.assertEqual((root / k8s_openapi.SUBSET).read_text(), '{"fresh": 1}\n')


if __name__ == "__main__":
    unittest.main()

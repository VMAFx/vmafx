# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/release/distribution.py.

Every case loads the real packaging/manifest.toml and changes one thing in memory. The
negative cases are the disagreements the check exists for: a channel that names an
artifact no pipeline builds, a licence that differs from the licensing record or the
package manifest, a producer job that does not exist. The boundary cases generate the
Homebrew formula and the conda recipe for an artifact with no GPU backend and for one
with every backend, and the winget manifests are validated against the vendored schemas.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import jsonschema
import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.release import distribution  # noqa: E402

DIGESTS = {"source": "a" * 64, "winget": "b" * 64}
SCHEMAS = ROOT / "packaging/schemas/winget-1.12.0"


def manifest() -> dict[str, Any]:
    return copy.deepcopy(distribution.load(ROOT))


class CheckTests(unittest.TestCase):
    def assert_problem(self, data: dict[str, Any], text: str) -> None:
        problems = distribution.check(ROOT, data)
        self.assertTrue(any(text in p for p in problems), problems)

    def test_tree_manifest_passes(self) -> None:
        self.assertEqual(distribution.check(ROOT, manifest()), [])

    def test_channel_naming_unbuilt_artifact_fails(self) -> None:
        data = manifest()
        data["channels"]["crates-io"]["artifacts"] = ["crate-vmafx"]
        self.assert_problem(data, "artifact 'crate-vmafx' is not built (#2320)")

    def test_channel_naming_unknown_artifact_fails(self) -> None:
        data = manifest()
        data["channels"]["github-release"]["artifacts"].append("native-linux-riscv64")
        self.assert_problem(data, "'native-linux-riscv64' is not in the manifest")

    def test_licence_missing_a_record_licence_fails(self) -> None:
        data = manifest()
        artifact = data["artifacts"]["native-linux-x86_64"]
        artifact["licence"] = artifact["licence"].replace(" AND MIT", "")
        self.assert_problem(data, "native-linux-x86_64")

    def test_licence_beyond_the_package_field_fails(self) -> None:
        data = manifest()
        data["artifacts"]["vmaf-mcp-wheel"]["licence"] += " AND MIT"
        self.assert_problem(data, "vmaf-mcp-wheel")

    def test_licence_without_text_fails(self) -> None:
        data = manifest()
        data["artifacts"]["models"]["licence"] += " AND LicenseRef-No-Text"
        self.assert_problem(data, "models: no LICENSES/LicenseRef-No-Text.txt")

    def test_missing_producer_job_fails(self) -> None:
        data = manifest()
        data["artifacts"]["tester-windows-x64"]["producer"]["job"] = "Build the zip (OS/2)"
        self.assert_problem(data, "defines no job 'Build the zip (OS/2)'")

    def test_missing_producer_workflow_fails(self) -> None:
        data = manifest()
        data["artifacts"]["models"]["producer"]["workflow"] = "gone.yml"
        self.assert_problem(data, "producer workflow 'gone.yml' does not exist")

    def test_producer_and_tracking_together_fail(self) -> None:
        data = manifest()
        data["artifacts"]["models"]["tracking"] = "#1"
        self.assert_problem(data, "exactly one of producer and tracking")

    def test_unknown_backend_fails(self) -> None:
        data = manifest()
        data["artifacts"]["source"]["backends"] = ["cpu", "vulkan"]
        self.assert_problem(data, "unknown backends ['vulkan']")

    def test_planned_channel_without_issue_fails(self) -> None:
        data = manifest()
        del data["channels"]["homebrew"]["tracking"]
        self.assert_problem(data, "channel homebrew: a planned channel")

    def test_main_exit_codes(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(distribution.main(["--root", str(ROOT), "check"]), 0)
            with tempfile.TemporaryDirectory() as empty:
                self.assertEqual(distribution.main(["--root", empty, "check"]), 2)


class GenerateTests(unittest.TestCase):
    def generate(self, data: dict[str, Any], version: str = "1.0.0") -> dict[str, str]:
        return distribution.generate(data, version, DIGESTS)

    def test_no_gpu_backend_boundary(self) -> None:
        files = self.generate(manifest())
        formula, recipe = files["homebrew/vmafx.rb"], files["conda-forge/recipe.yaml"]
        for flag in ("-Denable_cuda=false", "-Denable_sycl=false", "-Denable_hip=false",
                     "-Denable_metal=disabled"):  # fmt: skip
            self.assertIn(f'"{flag}"', formula)
            self.assertIn(flag, recipe)

    def test_every_backend_boundary(self) -> None:
        data = manifest()
        data["artifacts"]["source"]["backends"] = list(distribution.BACKENDS)
        files = self.generate(data)
        for flag in ("-Denable_cuda=true", "-Denable_sycl=true", "-Denable_hip=true",
                     "-Denable_metal=enabled"):  # fmt: skip
            self.assertIn(f'"{flag}"', files["homebrew/vmafx.rb"])
            self.assertIn(flag, files["conda-forge/recipe.yaml"])

    def test_licence_record_reaches_every_recipe(self) -> None:
        files = self.generate(manifest())
        recipe = yaml.safe_load(files["conda-forge/recipe.yaml"])
        licence = manifest()["artifacts"]["source"]["licence"]
        self.assertEqual(recipe["about"]["license"], licence)
        self.assertIn("LICENSES/LicenseRef-LIVE-BRISQUE.txt", recipe["about"]["license_file"])
        self.assertIn("license :cannot_represent", files["homebrew/vmafx.rb"])

    def test_winget_manifests_match_the_vendored_schemas(self) -> None:
        files = self.generate(manifest(), "1.0.0-rc.4")
        kinds = {"VMAFx.VMAFX.yaml": "version",
                 "VMAFx.VMAFX.locale.en-US.yaml": "defaultLocale",
                 "VMAFx.VMAFX.installer.yaml": "installer"}  # fmt: skip
        for name, kind in kinds.items():
            schema = json.loads((SCHEMAS / f"manifest.{kind}.1.12.0.json").read_text())
            document = yaml.safe_load(files[f"winget/{name}"])
            jsonschema.validate(document, schema)

    def test_winget_schema_refuses_a_broken_manifest(self) -> None:
        files = self.generate(manifest())
        schema = json.loads((SCHEMAS / "manifest.installer.1.12.0.json").read_text())
        document = yaml.safe_load(files["winget/VMAFx.VMAFX.installer.yaml"])
        document["Installers"][0]["InstallerSha256"] = "not a digest"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(document, schema)

    def test_index_lists_versioned_files(self) -> None:
        files = self.generate(manifest(), "1.0.0-rc.4")
        pypi = json.loads(files["index/pypi.json"])
        names = [name for entry in pypi["artifacts"] for name in entry["files"]]
        self.assertIn("vmaf_mcp-1.0.0rc4-py3-none-any.whl", names)
        planned = json.loads(files["index/winget.json"])
        self.assertEqual((planned["state"], planned["tracking"]), ("planned", "#2319"))

    def test_bad_version_and_digest_are_refused(self) -> None:
        with self.assertRaises(distribution.DistributionError):
            self.generate(manifest(), "v1.0")
        with self.assertRaises(distribution.DistributionError):
            distribution.generate(manifest(), "1.0.0", {"source": "a" * 63, "winget": "b" * 64})


if __name__ == "__main__":
    unittest.main()

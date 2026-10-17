#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""ADR-1591, ADR-1594: every archive and image the project publishes is written at
the strongest compression its documented consumers open.

Images: every docker/build-push-action step of a publishing workflow that pushes
exports through `outputs:` ending in the workflow's IMAGE_COMPRESSION (zstd at
BuildKit's strongest level, force-compression, OCI media types); the `push:`
shorthand carries no compression and is refused. A `load:` stays local. A workflow
that pushes an image and is not in PUBLISHING fails. Archives: the macOS bundle is
tar + xz at level 9, the git-archive source tarballs are gzip at level 9. The
Windows zip, the report bundle and the release tarballs have behavioural tests next
to their builders (tools/rc1-tester/tests, scripts/release/tests).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Any, Iterator

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
COMPOSITE = ROOT / ".github" / "actions" / "image-licence-artifacts" / "action.yml"
CANONICAL = "compression=zstd,compression-level=22,force-compression=true,oci-mediatypes=true"
ENV_REF = "${{ env.IMAGE_COMPRESSION }}"
INPUT_REF = "${{ inputs.compression }}"
PUBLISHING = (
    "docker-publish-tester.yml",
    "docker-publish-production.yml",
    "docker-publish-operator-node.yml",
    "dev-container-publish.yml",
    "published-rc-licence-companions.yml",
)
COMPOSITE_USE = "./.github/actions/image-licence-artifacts"
PUSHED_TYPES = ("image", "registry")


def load(path: Path) -> dict[str, Any]:
    document = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise TypeError(f"{path}: not a YAML mapping")
    return document


def steps_using(document: dict[str, Any], prefix: str) -> Iterator[dict[str, Any]]:
    """Every step of a workflow or composite action whose `uses:` starts with prefix."""
    jobs = document.get("jobs") or {"composite": document.get("runs", {})}
    for job in jobs.values():
        for step in job.get("steps", []):
            if str(step.get("uses", "")).startswith(prefix):
                yield step


def build_push_steps(document: dict[str, Any]) -> Iterator[dict[str, Any]]:
    return steps_using(document, "docker/build-push-action@")


def output_type(outputs: str) -> str:
    match = re.match(r"type=([a-z]+)", outputs)
    return match.group(1) if match else ""


def step_problems(step: dict[str, Any], reference: str) -> list[str]:
    """What keeps one build-push-action step from pushing at the canonical level. A
    `load:` (the local test image) is not published."""
    inputs = step.get("with", {})
    name = step.get("name", "<unnamed>")
    problems = [f"{name}: uses the push: shorthand"] if "push" in inputs else []
    outputs = str(inputs.get("outputs", ""))
    if output_type(outputs) in PUSHED_TYPES and not outputs.endswith("," + reference):
        problems.append(f"{name}: outputs {outputs!r} does not end with {reference}")
    if not outputs and not problems and inputs.get("load") not in (True, "true"):
        problems.append(f"{name}: no outputs")
    return problems


def pushes_an_image(step: dict[str, Any]) -> bool:
    """A build-push-action push, or a call of the action that pushes source images."""
    inputs = step.get("with", {})
    return (
        inputs.get("push") in (True, "true")
        or "push=true" in str(inputs.get("outputs", ""))
        or step.get("uses") == COMPOSITE_USE
    )


class ImageCompression(unittest.TestCase):
    def test_every_publishing_workflow_declares_the_canonical_level(self) -> None:
        for name in PUBLISHING:
            with self.subTest(workflow=name):
                self.assertEqual(load(WORKFLOWS / name)["env"]["IMAGE_COMPRESSION"], CANONICAL)

    def test_the_canonical_level_is_zstd_on_every_layer_with_oci_types(self) -> None:
        """BuildKit maps levels 9 to 22 to its best zstd; force-compression converts the
        layers that are gzip already (base image, GitHub Actions cache); Docker cannot
        pull zstd layers under Docker media types (docker/cli#5011)."""
        options = dict(item.split("=", 1) for item in CANONICAL.split(","))
        self.assertEqual(options, {"compression": "zstd", "compression-level": "22",
                                   "force-compression": "true", "oci-mediatypes": "true"})  # fmt: skip

    def test_every_image_export_carries_it(self) -> None:
        for name in PUBLISHING:
            document = load(WORKFLOWS / name)
            steps = list(build_push_steps(document))
            self.assertTrue(steps or list(steps_using(document, COMPOSITE_USE)), name)
            for step in steps:
                with self.subTest(workflow=name, step=step.get("name")):
                    self.assertEqual(step_problems(step, ENV_REF), [])

    def test_the_source_image_action_takes_it_from_the_caller(self) -> None:
        action = load(COMPOSITE)
        self.assertIs(action["inputs"]["compression"]["required"], True)
        for step in build_push_steps(action):
            self.assertEqual(step_problems(step, INPUT_REF), [])
        for name in PUBLISHING:
            for step in steps_using(load(WORKFLOWS / name), COMPOSITE_USE):
                with self.subTest(workflow=name, step=step.get("name")):
                    self.assertEqual(step.get("with", {}).get("compression"), ENV_REF)

    def test_no_other_workflow_pushes_an_image(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.yml")):
            if any(pushes_an_image(step) for step in build_push_steps(load(path))):
                self.assertIn(path.name, PUBLISHING, f"{path.name} pushes an image; see ADR-1594")

    def test_the_checks_refuse_planted_defects(self) -> None:
        shorthand = {"name": "s", "with": {"push": True, "tags": "x"}}
        no_level = {"name": "n", "with": {"outputs": "type=image,push=true"}}
        gzip_level = {"name": "g", "with": {"outputs": "type=image,push=true,compression=gzip"}}
        loaded = {"name": "l", "with": {"load": True}}
        local = {"name": "o", "with": {"outputs": "type=local,dest=out"}}
        self.assertEqual(len(step_problems(shorthand, ENV_REF)), 1)
        self.assertEqual(len(step_problems(no_level, ENV_REF)), 1)
        self.assertEqual(len(step_problems(gzip_level, ENV_REF)), 1)
        self.assertEqual(step_problems(loaded, ENV_REF), [])
        self.assertEqual(step_problems(local, ENV_REF), [])
        self.assertTrue(pushes_an_image(shorthand) and pushes_an_image(no_level))
        self.assertTrue(pushes_an_image({"uses": COMPOSITE_USE, "with": {}}))
        self.assertFalse(pushes_an_image(local))


class ArchiveCompression(unittest.TestCase):
    def test_the_macos_bundle_is_tar_xz_at_level_9(self) -> None:
        script = (ROOT / "scripts/ci/build-macos-tester-bundle.sh").read_text(encoding="utf-8")
        self.assertIn('--options xz:compression-level=9 \\\n  -cJf "$name.tar.xz" "$name"', script)
        self.assertNotIn("-czf", script)
        workflow = (WORKFLOWS / "macos-tester-bundle.yml").read_text(encoding="utf-8")
        self.assertNotIn("out/*.tar.gz", workflow)
        self.assertNotIn("bundle/*.tar.gz", workflow)
        # Provenance subject, its consumer-recipe check (ADR-3316), SBOM subject, signing.
        self.assertEqual(workflow.count("bundle/*.tar.xz"), 4)

    def test_the_git_archive_source_tarballs_are_gzip_at_level_9(self) -> None:
        node = (ROOT / "docker/Dockerfile.node").read_text(encoding="utf-8")
        licensing = (ROOT / "tools/rc1-tester/image/licensing.py").read_text(encoding="utf-8")
        self.assertIn("git archive --format=tar.gz -9 ", node)
        self.assertIn('["archive", "--format=tar.gz", "-9", ', licensing)
        for text in (node, licensing):
            self.assertNotRegex(text, r"archive\W+--format=tar\.gz\W+(?!-9)--prefix")

    def test_the_windows_zip_encoder_has_one_pin_and_is_installed(self) -> None:
        """ADR-1594: zopfli writes the zips; the build's lock, the tests' lock, the tester
        package's dev pin and its pre-commit hook name one version, and the build installs
        the lock."""
        versions = {
            path: re.findall(r"^zopfli==(\S+)", (ROOT / path).read_text(encoding="utf-8"), re.M)
            for path in ("requirements/locks/windows-tester-zip.in",
                         "requirements/locks/windows-tester-zip.txt",
                         "tools/rc1-tester/requirements-dev-lock.txt")
        }  # fmt: skip
        dev_pin = re.findall(
            r'"zopfli==([^"]+)"', (ROOT / "tools/rc1-tester/pyproject.toml").read_text()
        )
        hook = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        dev_pin += re.findall(r"zopfli==([^\],\s]+)", hook)
        self.assertEqual(len(dev_pin), 2, "the rc1-tester dev extra and its pre-commit hook")
        self.assertEqual(
            {v for found in versions.values() for v in found} | set(dev_pin), {dev_pin[0]}
        )
        self.assertTrue(all(len(found) == 1 for found in versions.values()), versions)
        workflow = (WORKFLOWS / "windows-tester-bundle.yml").read_text(encoding="utf-8")
        self.assertIn(
            "pip install --require-hashes -r requirements/locks/windows-tester-zip.txt", workflow
        )
        self.assertIn("            requirements/locks/windows-tester-zip.txt\n", workflow)

    def test_the_release_tarballs_are_gzip_at_level_9(self) -> None:
        script = (ROOT / "scripts/release/build-native-release-artifacts.sh").read_text(
            encoding="utf-8"
        )
        self.assertEqual(script.count("| gzip -9n >artifacts/"), 2)
        self.assertNotRegex(script, r"\| gzip -n ")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every nv-codec-headers pin names the release build-config.env declares.

libvmaf's CUDA build compiles the loader of FFmpeg's nv-codec-headers in
(``ffnvcodec/dynlink_loader.h``). ``NV_CODEC_HEADERS_TAG`` and
``NV_CODEC_HEADERS_COMMIT`` in ``build-config.env`` are the authority. The sites
below repeat one of the two values, and this test holds them to it:

* the root ``Dockerfile`` and ``dev/Containerfile`` fetch the tag;
* ``docker/Dockerfile.tester`` and ``docker/Dockerfile.production-gpu`` fetch the
  commit and compare it; the Windows tester bundle reads the tester image's value;
* ``core/src/meson.build`` names the release in its configure errors;
* ``tools/rc1-tester/image/licensing.json`` records the source of the builds made
  from now on. The records of the published release candidates keep the commit
  those images were built against.

A production image pinned one release behind the dev image stopped the CUDA
release build in ``core/src/cuda/import_frame.c``, which calls a loader member
the older header does not declare.

Known gap: the hosted CUDA build legs (``build.yml``, ``libvmaf-build-matrix.yml``)
clone the default branch of nv-codec-headers and name no release, so this test
has no literal to compare there (docs/state.md,
T-NV-CODEC-HEADERS-CI-LEGS-UNPINNED-2026-10-10).
"""

from __future__ import annotations

import importlib.util
import json
import re
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

TAG_NAME = "NV_CODEC_HEADERS_TAG"
COMMIT_NAME = "NV_CODEC_HEADERS_COMMIT"
# File -> the line that holds its copy of the value.
SITES = {
    "Dockerfile": (TAG_NAME, r'^ARG NV_CODEC_TAG="([^"\n]*)"$'),
    "dev/Containerfile": (TAG_NAME, r"^ARG NV_CODEC_HEADERS_REF=(\S+)$"),
    "docker/Dockerfile.tester": (COMMIT_NAME, r"^ARG NV_CODEC_HEADERS_COMMIT=(\S+)$"),
    "docker/Dockerfile.production-gpu": (COMMIT_NAME, r"^ARG NV_CODEC_HEADERS_COMMIT=(\S+)$"),
}
MESON = "core/src/meson.build"
LICENSING = "tools/rc1-tester/image/licensing.json"
BUNDLE_WORKFLOW = ".github/workflows/windows-tester-bundle.yml"
# Artifacts whose record describes an image that is already published.
PUBLISHED = "published-rc-"
# Artifacts that carry their own record of the header source; the production
# CUDA image takes the tester image's.
CURRENT_RECORDS = frozenset({"cuda-image", "windows-cuda-zip"})
RELEASE_TAG = re.compile(r"\bn\d+\.\d+\.\d+\.\d+\b")
COMMIT_ID = re.compile(r"\b[0-9a-f]{40}\b")
HEADER_LINE = re.compile(r"nv-codec|NV_CODEC|ffnvcodec", re.IGNORECASE)
# The release the tester and production images were pinned to before: the
# planted drift of the negative cases, and the record of the published candidates.
OLDER_TAG = "n13.0.19.0"
OLDER_COMMIT = "876af32a202d0de83bd1d36fe74ee0f7fcf86b0d"


def site_findings(name: str, text: str, config: dict[str, str]) -> list[str]:
    """What is wrong with one site's copy of the value it repeats."""
    key, pattern = SITES[name]
    values = re.findall(pattern, text, flags=re.MULTILINE)
    if len(values) != 1:
        return [f"{name}: {len(values)} lines match {pattern}, not 1"]
    if values[0] != config[key]:
        return [f"{name}: {values[0]} is not {key}={config[key]}"]
    return []


def literal_findings(name: str, text: str, config: dict[str, str]) -> list[str]:
    """Release tags and commit ids on nv-codec-headers lines that are not the pin."""
    findings = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not HEADER_LINE.search(line):
            continue
        for tag in RELEASE_TAG.findall(line):
            if tag != config[TAG_NAME]:
                findings.append(f"{name}:{number}: tag {tag} is not {config[TAG_NAME]}")
        for commit in COMMIT_ID.findall(line):
            if commit != config[COMMIT_NAME]:
                findings.append(f"{name}:{number}: commit {commit} is not the pinned commit")
    return findings


def record_findings(manifest: dict[str, Any], config: dict[str, str]) -> list[str]:
    """Licence records of current builds that name another source than the pin."""
    wanted = f"at commit {config[COMMIT_NAME]}, tag {config[TAG_NAME]} "
    findings = []
    seen = set()
    for artifact, record in manifest["artifacts"].items():
        for component in record.get("components", []):
            if component.get("id") != "nv-codec-headers" or "source" not in component:
                continue
            if artifact.startswith(PUBLISHED):
                continue
            seen.add(artifact)
            if wanted not in component["source"]:
                findings.append(f"{LICENSING}: {artifact} records {component['source']}")
    for artifact in sorted(CURRENT_RECORDS - seen):
        findings.append(f"{LICENSING}: {artifact} has no nv-codec-headers source record")
    return findings


class NvCodecHeadersSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.config = MODULE.load_config(ROOT)

    def read(self, name: str) -> str:
        return (ROOT / name).read_text(encoding="utf-8")

    def test_the_authority_is_a_release_tag_and_a_full_commit_id(self) -> None:
        self.assertRegex(self.config[TAG_NAME], rf"^{RELEASE_TAG.pattern}$")
        self.assertRegex(self.config[COMMIT_NAME], r"^[0-9a-f]{40}$")

    def test_every_site_repeats_the_authority(self) -> None:
        for name in SITES:
            with self.subTest(site=name):
                self.assertEqual(site_findings(name, self.read(name), self.config), [])

    def test_a_site_one_release_behind_is_reported(self) -> None:
        behind = {TAG_NAME: OLDER_TAG, COMMIT_NAME: OLDER_COMMIT}
        for name, (key, _) in SITES.items():
            with self.subTest(site=name):
                text = self.read(name).replace(self.config[key], behind[key])
                self.assertEqual(len(site_findings(name, text, self.config)), 1)

    def test_a_site_without_its_line_is_reported(self) -> None:
        for name in SITES:
            with self.subTest(site=name):
                text = self.read(name).replace("ARG NV_CODEC", "ARG OTHER")
                self.assertEqual(len(site_findings(name, text, self.config)), 1)

    def test_no_build_file_names_another_release(self) -> None:
        names = [*SITES, MESON, "build-config.env", BUNDLE_WORKFLOW]
        for name in names:
            with self.subTest(file=name):
                self.assertEqual(literal_findings(name, self.read(name), self.config), [])

    def test_another_release_on_a_header_line_is_reported(self) -> None:
        text = (
            f"ARG NV_CODEC_HEADERS_COMMIT={OLDER_COMMIT}\n"
            f"# nv-codec-headers {OLDER_TAG}\n"
            "ARG FFMPEG_COMMIT=946fcce07b6dcd0331c8cc609192aeff5e1924f8\n"
        )
        self.assertEqual(len(literal_findings("fixture", text, self.config)), 2)

    def test_meson_names_the_release_and_checks_the_newest_member(self) -> None:
        text = self.read(MESON)
        release = f"nv-codec-headers {self.config[TAG_NAME]} (commit {self.config[COMMIT_NAME]})"
        self.assertEqual(text.count(release), 1)
        self.assertIn("cc.has_member('CudaFunctions', 'cuArray3DGetDescriptor',", text)

    def test_the_windows_bundle_reads_the_tester_images_value(self) -> None:
        text = self.read(BUNDLE_WORKFLOW)
        self.assertIn("-Path docker/Dockerfile.tester", text)
        self.assertIn(r"'^ARG NV_CODEC_HEADERS_COMMIT=([0-9a-f]{40})$'", text)

    def test_the_licence_record_names_the_pinned_source(self) -> None:
        manifest = json.loads(self.read(LICENSING))
        self.assertEqual(record_findings(manifest, self.config), [])

    def test_a_record_of_another_commit_is_reported(self) -> None:
        manifest = json.loads(
            self.read(LICENSING).replace(
                f"at commit {self.config[COMMIT_NAME]}, tag",
                f"at commit {OLDER_COMMIT}, tag",
            )
        )
        self.assertEqual(len(record_findings(manifest, self.config)), 2)

    def test_the_published_candidates_keep_their_own_record(self) -> None:
        manifest = json.loads(self.read(LICENSING))
        sources = [
            component["source"]
            for artifact, record in manifest["artifacts"].items()
            if artifact.startswith(PUBLISHED)
            for component in record.get("components", [])
            if component.get("id") == "nv-codec-headers" and "source" in component
        ]
        self.assertEqual(len(sources), 1)
        self.assertIn(OLDER_COMMIT, sources[0])


if __name__ == "__main__":
    unittest.main()

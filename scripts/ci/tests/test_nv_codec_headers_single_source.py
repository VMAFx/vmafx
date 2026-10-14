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
  those images were built against;
* the hosted CUDA legs of ``build.yml`` and ``libvmaf-build-matrix.yml`` fetch
  the commit through ``scripts/ci/fetch-nv-codec-headers.sh``, which reads it
  from ``build-config.env``, tries each remote three times and refuses any other
  commit. No workflow clones the headers' default branch.

A production image pinned one release behind the dev image stopped the CUDA
release build in ``core/src/cuda/import_frame.c``, which calls a loader member
the older header does not declare.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
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
FETCH_SCRIPT = "scripts/ci/fetch-nv-codec-headers.sh"
# Workflows whose hosted CUDA legs install the headers.
CI_WORKFLOWS = (".github/workflows/build.yml", ".github/workflows/libvmaf-build-matrix.yml")
# Each install step of each workflow: Linux and Windows.
CI_STEPS_PER_WORKFLOW = 2
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


def workflow_findings(name: str, text: str) -> list[str]:
    """Workflow lines that fetch nv-codec-headers other than through the script."""
    findings = []
    for number, line in enumerate(text.splitlines(), start=1):
        if "nv-codec-headers" in line and re.search(r"\bgit\s+clone\b", line):
            findings.append(f"{name}:{number}: clones nv-codec-headers: {line.strip()}")
    calls = text.count(f"bash {FETCH_SCRIPT} ")
    if calls != CI_STEPS_PER_WORKFLOW:
        findings.append(f"{name}: {calls} calls of {FETCH_SCRIPT}, not {CI_STEPS_PER_WORKFLOW}")
    return findings


class NvCodecHeadersFetchScript(unittest.TestCase):
    """The script against a local remote: the pin, the fallback, the refusals."""

    def setUp(self) -> None:
        if shutil.which("git") is None or shutil.which("bash") is None:
            self.skipTest("git and bash are needed to run the fetch script")
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        # The caller's GIT_* (a hook's GIT_DIR above all) must not reach the fixture's git.
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.env.update(
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            GIT_TERMINAL_PROMPT="0",
            GIT_AUTHOR_NAME="Fixture",
            GIT_AUTHOR_EMAIL="fixture@example.invalid",
            GIT_COMMITTER_NAME="Fixture",
            GIT_COMMITTER_EMAIL="fixture@example.invalid",
        )
        self.remote = self.tmp / "remote"
        self.pin = self.commit(self.remote, with_loader=True)

    def git(self, *args: str, cwd: Path) -> str:
        result = run_command(
            [GIT, *args],
            allowed_executables=(GIT,),
            cwd=cwd,
            env=self.env,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=60,
        )
        return result.stdout.strip()

    def commit(self, repo: Path, *, with_loader: bool) -> str:
        if not repo.exists():
            repo.mkdir()
            self.git("init", "-q", cwd=repo)
            self.git("config", "uploadpack.allowAnySHA1InWant", "true", cwd=repo)
        header = repo / "include/ffnvcodec"
        header.mkdir(parents=True, exist_ok=True)
        loader = header / "dynlink_loader.h"
        if with_loader:
            loader.write_text("/* loader */\n", encoding="utf-8")
        elif loader.exists():
            loader.unlink()
        (repo / "stamp").write_text(str(len(list(repo.iterdir()))), encoding="utf-8")
        self.git("add", "-A", cwd=repo)
        self.git("commit", "-q", "--allow-empty", "-m", "c", cwd=repo)
        return self.git("rev-parse", "HEAD", cwd=repo)

    def run_script(self, commit: str, remotes: str) -> TextCommandResult:
        config = self.tmp / "build-config.env"
        config.write_text(f'NV_CODEC_HEADERS_COMMIT="{commit}"\n', encoding="utf-8")
        dest = self.tmp / "dest"
        shutil.rmtree(dest, ignore_errors=True)
        env = {
            **self.env,
            "NV_CODEC_HEADERS_REMOTES": remotes,
            "NV_CODEC_HEADERS_RETRY_DELAY": "0",
        }
        return run_command(
            [BASH, str(ROOT / FETCH_SCRIPT), "--config", str(config), str(dest)],
            allowed_executables=(BASH,),
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=120,
        )

    def test_the_pinned_commit_is_fetched(self) -> None:
        result = self.run_script(self.pin, f"file://{self.remote}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(self.pin, result.stdout)
        self.assertTrue((self.tmp / "dest/include/ffnvcodec/dynlink_loader.h").is_file())

    def test_a_failing_remote_falls_back_to_the_next(self) -> None:
        missing = f"file://{self.tmp / 'no-such-remote'}"
        result = self.run_script(self.pin, f"{missing} file://{self.remote}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count("failed"), 3)
        self.assertIn(f"from file://{self.remote}", result.stdout)

    def test_a_commit_no_remote_serves_is_refused(self) -> None:
        result = self.run_script("1" * 40, f"file://{self.remote}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no remote served commit", result.stderr)

    def test_a_tree_without_the_loader_is_refused(self) -> None:
        bare = self.commit(self.remote, with_loader=False)
        result = self.run_script(bare, f"file://{self.remote}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("dynlink_loader.h", result.stderr)

    def test_a_config_without_a_full_commit_is_refused(self) -> None:
        result = self.run_script(self.pin[:12], f"file://{self.remote}")
        self.assertEqual(result.returncode, 2)


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

    def test_the_ci_legs_fetch_through_the_script(self) -> None:
        for name in CI_WORKFLOWS:
            with self.subTest(workflow=name):
                self.assertEqual(workflow_findings(name, self.read(name)), [])

    def test_a_ci_leg_cloning_the_default_branch_is_reported(self) -> None:
        name = CI_WORKFLOWS[0]
        text = self.read(name).replace(
            f'bash {FETCH_SCRIPT} "$RUNNER_TEMP/nv-codec-headers"',
            "git clone https://github.com/FFmpeg/nv-codec-headers.git",
            1,
        )
        self.assertEqual(len(workflow_findings(name, text)), 2)

    def test_the_fetch_script_names_no_release(self) -> None:
        text = self.read(FETCH_SCRIPT)
        self.assertEqual(RELEASE_TAG.findall(text), [])
        self.assertEqual(COMMIT_ID.findall(text), [])
        self.assertIn("NV_CODEC_HEADERS_COMMIT", text)

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

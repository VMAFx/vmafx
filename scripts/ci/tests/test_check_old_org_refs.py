#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the old organisation reference guard and its exemptions."""

import datetime
import importlib.util
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import TextCommandResult, run

ROOT = Path(__file__).resolve().parents[3]
CHECKER = Path(
    os.environ.get("OLD_ORG_CHECKER_TEST_PATH", ROOT / "scripts/ci/check-old-org-refs.py")
).resolve()
git_path = shutil.which("git")
if git_path is None:
    raise RuntimeError("git not found on PATH")
GIT = str(Path(git_path).resolve())
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
spec = importlib.util.spec_from_file_location("old_org_guard", CHECKER)
if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load checker: {CHECKER}")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
DAY = datetime.date(2026, 11, 30)
NEXT_DAY = DAY + datetime.timedelta(days=1)
OLD_ORG = "VMA" "Fx"
OLD_TOOL_ORG = "cordana" "LLM"
OLD_DEPS_ORG = "go" "lusoris"
OLD_HOST = "github" ".com"
OLD_REGISTRY = "ghcr" ".io"
INLINE_MARKER = "old-org-" "ok"
OLD_MODULE = f"{OLD_HOST}/{OLD_ORG}/vmafx"
LEGACY_IMAGE = f"{OLD_REGISTRY}/{OLD_ORG.lower()}/vmafx"
WORKFLOW = ".github/workflows/example.yml"
ADR_HEADER = "docs/adr/_index_fragments/_header.md"
K8S_VALUES = "deploy/helm/vmafx/values.yaml"
TEXT_LIMIT = 8 * 1_048_576


def git_fixture_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    return env


def run_checker(cwd: str, env: dict[str, str], *extra: str) -> TextCommandResult:
    return run(
        [sys.executable, "-B", str(CHECKER), *extra],
        allowed_executables=(sys.executable,),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=SCRIPT_TIMEOUT_S,
    )


class OldOrgTests(unittest.TestCase):
    def hits(self, line: str, path: str = "example.txt", day: datetime.date = DAY) -> list[str]:
        findings: list[str] = module.line_findings(path, 1, line, day)
        return findings

    def test_forms(self) -> None:
        forms = {
            "a": f"{OLD_ORG}/repo",
            "b": f"{OLD_TOOL_ORG}/repo",
            "c": f"{OLD_DEPS_ORG}/{OLD_DEPS_ORG}",
            "d": f"https://{OLD_HOST}/{OLD_ORG.lower()}/repo",
            "e": f"{OLD_ORG}.github.io",
            "f": f"{OLD_HOST}/orgs/{OLD_DEPS_ORG}/people",
            "g": f'owner: "{OLD_ORG}"',
            "j": f"repo:{OLD_ORG.lower()}/vmafx",
        }
        for letter, text in forms.items():
            with self.subTest(letter=letter):
                self.assertIn(f"({letter})", self.hits(text)[0])

    def test_exempt_forms(self) -> None:
        for value in (
            f"pkg:golang/{OLD_MODULE}",
            f"mod\t{OLD_MODULE}",
            OLD_ORG,
            "vmafx/types.h",
            f"../{OLD_DEPS_ORG}/",
            f"{OLD_DEPS_ORG}/fx",
            "vidiko/vmafx/x",
            f"{OLD_ORG}-next/x",
            f"owner: {OLD_ORG}-next",
            f"{OLD_HOST}/{OLD_ORG}-next/x",
            f"owner: {OLD_DEPS_ORG}-x",
        ):
            with self.subTest(value=value):
                self.assertEqual([], self.hits(value))
        self.assertEqual([], self.hits(f"{OLD_ORG}/repo # {INLINE_MARKER}", "AGENTS.md"))
        self.assertEqual([], self.hits(f"module {OLD_MODULE}", "go.mod"))
        self.assertEqual([], self.hits(f'\t"{OLD_MODULE}/pkg/score"', "score.go"))
        self.assertTrue(self.hits(f"http://{OLD_HOST}/{OLD_ORG}/x"))
        self.assertTrue(self.hits(f"https://{OLD_HOST}/{OLD_ORG}/x"))
        self.assertTrue(self.hits(OLD_MODULE))

    def test_permanent_paths(self) -> None:
        for path in (
            "docs/adr/1234-title.md",
            "docs/adr/_index_fragments/1234-title.md",
            "docs/research/x",
            "scripts/ci/research-digest-id-baseline.json",
            "docs/rebase-notes.d/x",
            "docs/rebase-notes.md",
            "docs/adr/README.md",
            "docs/adr/titles.md",
            "docs/adr/by-tag/git.md",
            "docs/research/titles.md",
            "changelog.d/x",
            "CHANGELOG.md",
            "docs/changelog-archive/x",
        ):
            with self.subTest(path=path):
                self.assertTrue(module.allowed(path))

    def test_permanent_path_boundaries(self) -> None:
        self.assertFalse(module.allowed("docs/adr/by-tag-notes.md"))
        self.assertFalse(module.allowed("docs/adr/README-draft.md"))
        self.assertFalse(module.allowed(ADR_HEADER))
        self.assertFalse(module.allowed("scripts/ci/tests/test_check_old_org_refs.py"))

    def test_live_header_is_checked(self) -> None:
        self.assertTrue(self.hits(f"{OLD_HOST}/{OLD_ORG}/vmafx", ADR_HEADER))

    def test_dated_exceptions(self) -> None:
        entries = [
            ("core/include/libvmaf/pelorus/x.h", f"{OLD_ORG}/x"),
            ("core/src/interop/pelorus_x.c", f"{OLD_ORG}/x"),
            ("core/test/test_pelorus_interop.c", f"{OLD_ORG}/x"),
            ("docs/api/pelorus-interop.md", f"{OLD_ORG}/x"),
            ("build-config.env", f"FFMPEG_FIX_SERIES_REPO={OLD_ORG}/x"),
            ("lua/standards.lua", f"{OLD_ORG}/x"),
            ("tools/markdownlint/package.json", f"{OLD_ORG}/x"),
            ("tools/markdownlint/package-lock.json", f"{OLD_ORG}/x"),
            (K8S_VALUES, f"{LEGACY_IMAGE}-operator"),
            ("AGENTS.md", f"{OLD_ORG}/x # {INLINE_MARKER}"),
        ]
        for path, line in entries:
            with self.subTest(path=path):
                self.assertEqual([], self.hits(line, path))
                self.assertIn("expired exception", self.hits(line, path, NEXT_DAY)[0])
        with self.subTest(path="example.txt"):
            line = f"{OLD_ORG}/x # {INLINE_MARKER}"
            self.assertIn("(a)", self.hits(line, "example.txt")[0])
        self.assertTrue(self.hits(f"OTHER={OLD_ORG}/x", "build-config.env"))

    def test_pre_move_tester_identity_expires(self) -> None:
        path = "docs/usage/tester-image.md"
        line = (
            f"An image published before the move (`{LEGACY_IMAGE}:<VERSION>-tester`)"
            f" verifies with `{OLD_ORG}` as the organisation in the workflow identity."
        )
        expiry = datetime.date(2027, 3, 20)
        self.assertEqual([], self.hits(line, path, expiry))
        finding = self.hits(line, path, expiry + datetime.timedelta(days=1))[0]
        self.assertIn("images published before the move carry the old workflow identity", finding)
        self.assertIn("MEL D, 2026-11-20 to 2027-03-20", finding)
        self.assertTrue(self.hits(f"{LEGACY_IMAGE}:<VERSION>-tester", path))
        self.assertTrue(self.hits(line))

    def test_obsolete_forms_in_context(self) -> None:
        cases = (
            (WORKFLOW, f"{LEGACY_IMAGE}-operator"),
            (WORKFLOW, f"IMAGE_NAME: {OLD_ORG.lower()}/vmafx"),
            (WORKFLOW, f"  'CONTAINER_IMAGE_URL': \"{OLD_ORG}/repo:tag\""),
            (WORKFLOW, f'env: {{IMAGE_NAME: "{OLD_ORG.lower()}/vmafx"}}'),
            ("README.md", f"uri={OLD_MODULE}"),
            ("README.md", f"{OLD_HOST}/{OLD_ORG}/"),
            ("README.md", f"repo%3A{OLD_ORG}%2Fvmafx"),
            ("README.md", f"repo%3a{OLD_TOOL_ORG.lower()}%2fpraetor"),
            ("README.md", f"{OLD_REGISTRY}/{OLD_ORG.lower()}/x"),
            ("README.md", f"{OLD_REGISTRY}/{OLD_ORG}/x"),
            ("README.md", f"{OLD_REGISTRY}/{OLD_ORG.lower()}/"),
        )
        for path, line in cases:
            with self.subTest(path=path, line=line):
                self.assertTrue(self.hits(line, path))
        self.assertEqual([], self.hits(f"IMAGE_NAME: {OLD_ORG.lower()}/vmafx"))

    def test_text_size_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "data.txt"
            path.write_bytes((b"x" * 1023 + b"\n") * (TEXT_LIMIT // 1024))
            self.assertEqual([], module.scan(str(path), DAY))
            path.write_bytes(b"x" * (TEXT_LIMIT + 1))
            self.assertIn("tracked text exceeds", module.scan(str(path), DAY)[0])
            path.write_bytes(b"\xff" * (TEXT_LIMIT + 1))
            self.assertEqual([], module.scan(str(path), DAY))

    def test_all_git_failure(self) -> None:
        env = git_fixture_env()
        with tempfile.TemporaryDirectory() as tmp:
            env["GIT_CEILING_DIRECTORIES"] = str(Path(tmp).resolve().parent)
            result = run_checker(tmp, env, "--all")
            self.assertEqual(2, result.returncode)
            self.assertIn("check-old-org-refs:", result.stderr)

    def test_all_git_ceiling_blocks_enclosing_repository(self) -> None:
        env = git_fixture_env()
        with tempfile.TemporaryDirectory() as tmp:
            outer = Path(tmp).resolve()
            run(
                [GIT, "init", "-q", str(outer)],
                allowed_executables=(GIT,),
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=GIT_TIMEOUT_S,
            )
            inner = outer / "inner"
            inner.mkdir()
            result = run_checker(str(inner), env)
            self.assertEqual(0, result.returncode)
            self.assertEqual("", result.stdout)

            env["GIT_CEILING_DIRECTORIES"] = str(outer)
            result = run_checker(str(inner), env)
            self.assertEqual(2, result.returncode)
            self.assertIn("check-old-org-refs:", result.stderr)

    def test_planted_all_and_usage(self) -> None:
        env = git_fixture_env()
        env["OLD_ORG_REFS_TODAY"] = "2026-11-20"
        with tempfile.TemporaryDirectory() as tmp:
            run(
                [GIT, "init", "-q", tmp],
                allowed_executables=(GIT,),
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=GIT_TIMEOUT_S,
            )
            path = Path(tmp) / "planted.txt"
            path.write_text(f"https://{OLD_MODULE}/issues/1\n")
            run(
                [GIT, "-C", tmp, "add", "planted.txt"],
                allowed_executables=(GIT,),
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=GIT_TIMEOUT_S,
            )
            self.assertEqual(1, run_checker(tmp, env, "--all").returncode)
            path.write_text("clean\n")
            self.assertEqual(0, run_checker(tmp, env).returncode)
            self.assertEqual(2, run_checker(tmp, env, "--all", "file").returncode)


if __name__ == "__main__":
    unittest.main()

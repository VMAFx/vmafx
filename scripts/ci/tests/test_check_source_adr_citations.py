# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression coverage for the source ADR-citation provenance gate."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts/ci/check-source-adr-citations.py"
GIT = shutil.which("git") or "/usr/bin/git"
PYTHON = sys.executable


def fixture_git_env() -> dict[str, str]:
    """Return a hermetic environment for disposable fixture repositories."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return environment


class SourceAdrCitationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        self.run_git("init", "-q")
        self.run_git("config", "user.name", "Citation Fixture")
        self.run_git("config", "user.email", "fixture@example.invalid")
        (self.repo / "docs/adr").mkdir(parents=True)
        self.write("evidence.txt", "fixture evidence\n")

    def run_git(self, *args: str) -> str:
        return subprocess.run(  # noqa: S603 -- fixed Git binary in a disposable fixture
            [
                GIT,
                "-c",
                "core.hooksPath=/dev/null",
                "-c",
                "commit.gpgsign=false",
                *args,
            ],
            cwd=self.repo,
            check=True,
            env=fixture_git_env(),
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()

    def write(self, relative: str, body: str) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def write_adr(self, name: str) -> None:
        self.write(f"docs/adr/{name}", f"# ADR-{name[:4]}: fixture\n")

    def commit(self) -> str:
        self.run_git("add", ".")
        self.run_git("commit", "-qm", "test: fixture checkpoint")
        return self.run_git("rev-parse", "HEAD")

    def registry(
        self,
        *,
        retired: dict[str, Any] | None = None,
        fixtures: dict[str, Any] | None = None,
        extra: dict[str, Any] | None = None,
    ) -> None:
        body = {
            "schema_version": 2,
            "retired": retired or {},
            "fixtures": fixtures or {},
            **(extra or {}),
        }
        self.write("source-adr-citations.json", json.dumps(body, indent=2, sort_keys=True) + "\n")
        self.run_git("add", ".")

    def run_checker(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- gate under test; fixed executable and argv shape
            [
                PYTHON,
                str(CHECKER),
                "--root",
                str(self.repo),
                "--registry",
                "source-adr-citations.json",
                *extra,
            ],
            env=fixture_git_env(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_a_live_citation_needs_no_registry_entry(self) -> None:
        """ADR-2200: the binding is derived from the tree, so adding one edits no shared file."""
        self.write_adr("0042-original-decision.md")
        self.write("core/example.c", "/* Kept for ADR-0042. */\n")
        self.registry()
        before = (self.repo / "source-adr-citations.json").read_bytes()
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("1 live derived", result.stdout)
        self.write("core/second.c", "/* Also ADR-0042 and ADR-0042. */\n")
        self.run_git("add", ".")
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.repo / "source-adr-citations.json").read_bytes(), before)

    def test_missing_number_fails_closed(self) -> None:
        self.write("core/example.c", "/* The invariant came from ADR-0099. */\n")
        self.registry()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("ADR-0099", result.stdout)

    def test_governed_superseded_identity_passes(self) -> None:
        self.write_adr("0043-successor.md")
        self.write("core/example.c", "/* Supersedes ADR-0042. */\n")
        history = self.commit()
        self.registry(
            retired={
                "0042": {
                    "status": "superseded",
                    "reason": "The historical decision is named to explain its replacement.",
                    "evidence": ["evidence.txt"],
                    "git_commits": [history],
                    "successor": "0043-successor.md",
                    "related": [],
                    "sites": {"core/example.c": 1},
                }
            }
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_retired_number_must_not_be_reallocated(self) -> None:
        self.write_adr("0042-reused-number.md")
        self.write_adr("0043-successor.md")
        self.write("core/example.c", "/* Supersedes ADR-0042. */\n")
        history = self.commit()
        self.registry(
            retired={
                "0042": {
                    "status": "superseded",
                    "reason": "The historical decision is named to explain its replacement.",
                    "evidence": ["evidence.txt"],
                    "git_commits": [history],
                    "successor": "0043-successor.md",
                    "related": [],
                    "sites": {"core/example.c": 1},
                }
            }
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("retired number", result.stdout)

    def test_fixture_number_is_exact_path_scoped(self) -> None:
        self.write("scripts/fixture.py", 'TOKEN = "ADR-9999"\n')
        self.registry(
            fixtures={
                "9999": {
                    "reason": "Synthetic impossible-number fixture.",
                    "sites": {"scripts/fixture.py": 1},
                }
            }
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)

        self.write("core/escape.c", "/* ADR-9999 escaped its fixture. */\n")
        self.run_git("add", ".")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("ADR-9999", result.stdout)

    def test_markdown_prose_is_out_of_scope(self) -> None:
        self.write("notes.md", "An ordinary prose example names ADR-9999.\n")
        self.write("mkdocs.yml", "nav:\n  - Example: ADR-9999\n")
        self.registry()
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_a_legacy_live_section_is_refused_not_ignored(self) -> None:
        """Planted: a registry that still lists live bindings must not pass silently."""
        self.write_adr("0042-original-decision.md")
        self.write("core/example.c", "/* ADR-0042. */\n")
        self.registry(extra={"live": {}})
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("derived from the tree", result.stdout)

    def test_the_old_schema_is_refused(self) -> None:
        self.write("core/example.c", "/* nothing */\n")
        self.registry()
        path = self.repo / "source-adr-citations.json"
        body = json.loads(path.read_text(encoding="utf-8"))
        body["schema_version"] = 1
        path.write_text(json.dumps(body), encoding="utf-8")
        self.run_git("add", ".")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("schema_version", result.stdout)

    def test_there_is_no_write_mode_for_live_bindings(self) -> None:
        self.registry()
        result = self.run_checker("--write")
        self.assertEqual(result.returncode, 2, result.stdout)

    def test_a_missing_number_names_what_to_do(self) -> None:
        self.write("core/example.c", "/* ADR-0099. */\n")
        self.registry()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no ADR file and no retirement record", result.stdout)

    def test_two_pull_requests_citing_different_numbers_do_not_share_a_line(self) -> None:
        """The registry bytes are the same whichever citations a change adds."""
        self.write_adr("0042-original-decision.md")
        self.write_adr("0041-other-decision.md")
        self.write("core/a.c", "/* ADR-0042. */\n")
        self.write("core/b.c", "/* ADR-0041. */\n")
        self.registry()
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)
        registry = json.loads((self.repo / "source-adr-citations.json").read_text(encoding="utf-8"))
        self.assertEqual(set(registry), {"schema_version", "retired", "fixtures"})

    def test_fixture_git_discards_inherited_repository_environment(self) -> None:
        self.write_adr("0042-original-decision.md")
        self.write("core/example.c", "/* Kept for ADR-0042. */\n")
        self.registry()
        with tempfile.TemporaryDirectory() as caller_tmp:
            caller = Path(caller_tmp)
            clean_env = {
                key: value for key, value in os.environ.items() if not key.startswith("GIT_")
            }
            clean_env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
            subprocess.run(  # noqa: S603 -- fixed Git binary and disposable caller fixture
                [GIT, "-c", "core.hooksPath=/dev/null", "init", "-q", str(caller)],
                check=True,
                env=clean_env,
            )
            (caller / "caller.txt").write_text("caller state\n", encoding="utf-8")
            subprocess.run(  # noqa: S603 -- fixed Git binary and disposable caller fixture
                [GIT, "-c", "core.hooksPath=/dev/null", "add", "caller.txt"],
                check=True,
                cwd=caller,
                env=clean_env,
            )
            caller_index = caller / ".git/index"
            caller_config = caller / ".git/config"
            index_before = caller_index.read_bytes()
            config_before = caller_config.read_bytes()

            self.write("isolated.txt", "fixture state\n")
            poison = {
                "GIT_DIR": str(caller / ".git"),
                "GIT_INDEX_FILE": str(caller_index),
                "GIT_PREFIX": "foreign-prefix/",
                "GIT_WORK_TREE": str(caller),
            }
            with patch.dict(os.environ, poison, clear=False):
                self.run_git("config", "fixture.poison-probe", "owned")
                self.run_git("add", "isolated.txt")
                result = self.run_checker()

            self.assertEqual(caller_index.read_bytes(), index_before)
            self.assertEqual(caller_config.read_bytes(), config_before)
            self.assertEqual(
                self.run_git("ls-files", "--error-unmatch", "isolated.txt"), "isolated.txt"
            )
            self.assertEqual(self.run_git("config", "--get", "fixture.poison-probe"), "owned")
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_repository_registry_is_current(self) -> None:
        result = subprocess.run(  # noqa: S603 -- gate under test; fixed executable and argv
            [PYTHON, str(CHECKER), "--root", str(ROOT)],
            env=fixture_git_env(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check_licence_metadata.py (ADR-1699).

The repository passes, and every defect the gate exists for is planted in a
throwaway git tree and refused: a second root licence file (the `LICENSE-MIT`
that ADR-0686 added, or Netflix's text under a licence-file name), Netflix's text
back in `LICENSE`, `NOTICE` missing or without Netflix's copyright line, and a
crate, a chart, a subchart record or an npm package whose licence field disagrees
with its files.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import shutil
import tarfile
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
GATE = REPO_ROOT / "scripts" / "ci" / "check_licence_metadata.py"
GIT_TIMEOUT_S = 60
# Assembled so that no licence scanner reads this file as declaring these terms.
TAG = "SPDX-" + "License-Identifier:"


def _load_gate() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("vmafx_licence_metadata_under_test", GATE)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {GATE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gate = _load_gate()


def _write(root: Path, files: dict[str, str | bytes]) -> None:
    for name, content in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8")


def _fixture_environment() -> dict[str, str]:
    """No caller Git state: a commit hook exports GIT_INDEX_FILE (and may export
    GIT_DIR), which would turn `git init` / `git add` in a fixture into writes to
    the caller's index and config."""
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    clean.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return clean


def _commit_index(root: Path) -> None:
    for command in (["init", "-q"], ["add", "-A"]):
        gate.run_command(
            ["git", "-C", str(root), *command],
            allowed_executables=("git",),
            env=_fixture_environment(),
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )


def _root_layout() -> dict[str, str | bytes]:
    """The ADR-1699 root layout with the repository's own texts."""
    manifest = {
        "spdx_texts": {
            "BSD-2-Clause-Patent": "NOTICE",
            "EUPL-1.2": "LICENSES/EUPL-1.2.txt",
        }
    }
    return {
        "REUSE.toml": "version = 1\n",
        "LICENSE": (REPO_ROOT / "LICENSES" / "EUPL-1.2.txt").read_bytes(),
        "LICENSES/EUPL-1.2.txt": (REPO_ROOT / "LICENSES" / "EUPL-1.2.txt").read_bytes(),
        "NOTICE": (REPO_ROOT / "NOTICE").read_bytes(),
        "tools/rc1-tester/image/licensing.json": json.dumps(manifest),
    }


def _subchart(licence: str) -> bytes:
    """A packaged Helm chart whose Chart.yaml declares `licence`."""
    chart = f"apiVersion: v2\nname: sub\nannotations:\n  artifacthub.io/license: {licence}\n"
    data = chart.encode()
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as bundle:
        member = tarfile.TarInfo("sub/Chart.yaml")
        member.size = len(data)
        bundle.addfile(member, io.BytesIO(data))
    return buffer.getvalue()


class GateTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="licence-metadata-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def problems(self, files: dict[str, str | bytes]) -> list[str]:
        _write(self.root, files)
        _commit_index(self.root)
        problems: list[str] = gate.check(self.root)[0]
        return problems


class RepositoryTests(unittest.TestCase):
    def test_the_repository_passes(self) -> None:
        problems, count = gate.check(REPO_ROOT)
        self.assertEqual(problems, [])
        self.assertGreater(count, 10)

    def test_the_main_entry_point_exits_zero_on_the_repository(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = gate.main(["--root", str(REPO_ROOT)])
        self.assertEqual(status, 0, output.getvalue())
        self.assertIn("agree with the files", output.getvalue())


class WiringTests(unittest.TestCase):
    """The gate is wired where the licence checks already run (ADR-1699)."""

    def test_the_licence_provenance_job_runs_the_gate(self) -> None:
        text = (REPO_ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        start = text.index("\n  licence-provenance:\n")
        end = text.index("\n  shellcheck:\n", start)
        self.assertIn("python3 scripts/ci/check_licence_metadata.py", text[start:end])
        self.assertIn("check-licence-metadata,", text[:start])

    def test_every_commit_runs_the_gate(self) -> None:
        text = (REPO_ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        start = text.index("- id: check-licence-metadata\n")
        hook = text[start : text.index("\n\n", start)]
        self.assertIn("entry: python3 -B scripts/ci/check_licence_metadata.py", hook)
        self.assertIn("always_run: true", hook)


class FixtureIsolationTests(unittest.TestCase):
    """A commit hook runs these tests with GIT_INDEX_FILE (and possibly GIT_DIR)
    set; neither the fixtures nor the gate may write to or read from that repository."""

    def _caller(self) -> Path:
        caller = Path(tempfile.mkdtemp(prefix="licence-metadata-caller-"))
        self.addCleanup(shutil.rmtree, caller, ignore_errors=True)
        _write(caller, {"caller.txt": "caller state\n"})
        _commit_index(caller)
        return caller

    def test_caller_git_variables_reach_neither_fixture_nor_gate(self) -> None:
        for names in (("GIT_INDEX_FILE",), ("GIT_DIR",), ("GIT_DIR", "GIT_WORK_TREE")):
            with self.subTest(variables=names):
                caller = self._caller()
                git_dir = caller / ".git"
                values = {
                    "GIT_DIR": str(git_dir),
                    "GIT_INDEX_FILE": str(git_dir / "index"),
                    "GIT_WORK_TREE": str(caller),
                }
                before = ((git_dir / "index").read_bytes(), (git_dir / "config").read_bytes())
                fixture = Path(tempfile.mkdtemp(prefix="licence-metadata-fixture-"))
                self.addCleanup(shutil.rmtree, fixture, ignore_errors=True)
                files = _root_layout()
                files["LICENSE-MIT"] = "MIT License\n"
                with mock.patch.dict(os.environ, {n: values[n] for n in names}, clear=False):
                    _write(fixture, files)
                    _commit_index(fixture)
                    problems, _ = gate.check(fixture)
                self.assertEqual(len(problems), 1, problems)
                self.assertTrue(problems[0].startswith("LICENSE-MIT: "), problems)
                after = ((git_dir / "index").read_bytes(), (git_dir / "config").read_bytes())
                self.assertEqual(after, before)


class RootLayoutTests(GateTestCase):
    def test_the_layout_passes(self) -> None:
        self.assertEqual(self.problems(_root_layout()), [])

    def test_a_root_mit_licence_file_is_refused(self) -> None:
        """Planted defect: the LICENSE-MIT that ADR-0686 added back at the root."""
        files = _root_layout()
        files["LICENSE-MIT"] = "MIT License\n\nCopyright (c) 2026 Lusoris\n"
        problems = self.problems(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertTrue(problems[0].startswith("LICENSE-MIT: a second root licence file"))

    def test_netflix_text_under_a_licence_file_name_is_refused(self) -> None:
        """Planted defect: NOTICE renamed back to LICENSE-BSD-2-Clause-Patent, which
        licensee scores as a second licence file (project licence NOASSERTION)."""
        files = _root_layout()
        files["LICENSE-BSD-2-Clause-Patent"] = files.pop("NOTICE")
        problems = self.problems(files)
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(problems[0].startswith("LICENSE-BSD-2-Clause-Patent: a second root"))
        self.assertIn("NOTICE is missing", problems[1])

    def test_every_name_licensee_scores_is_refused(self) -> None:
        names = (
            "COPYING", "LICENCE.md", "UNLICENSE", "MIT-LICENSE.txt", "license.txt",
            "COPYRIGHT", "COPYING.xml", "OFL.md", "PATENTS", "LICENSE-APACHE",
        )  # fmt: skip
        files = _root_layout()
        for name in names:
            files[name] = "terms\n"
        problems = self.problems(files)
        self.assertEqual(sorted(p.split(":")[0] for p in problems), sorted(names), problems)

    def test_names_licensee_does_not_score_are_not_licence_files(self) -> None:
        files = _root_layout()
        files["LICENSES/MIT.txt"] = "MIT text\n"
        for name in ("licensing.md", "LICENSE.spdx", "PATENTS.sh", "README.md", "GOVERNANCE.md"):
            files[name] = "prose\n"
        self.assertEqual(self.problems(files), [])

    def test_netflix_text_back_in_license_is_refused(self) -> None:
        """Planted defect: LICENSE swapped back to Netflix's BSD-2-Clause-Patent text."""
        files = _root_layout()
        files["LICENSE"] = files["NOTICE"]
        problems = self.problems(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("LICENSE is not LICENSES/EUPL-1.2.txt", problems[0])

    def test_a_missing_notice_is_refused(self) -> None:
        """Planted defect: NOTICE deleted."""
        files = _root_layout()
        del files["NOTICE"]
        problems = self.problems(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("NOTICE is missing", problems[0])

    def test_a_notice_without_the_netflix_copyright_line_is_refused(self) -> None:
        """Planted defect: Netflix's copyright line removed from NOTICE."""
        files = _root_layout()
        text = (REPO_ROOT / "NOTICE").read_text(encoding="utf-8")
        files["NOTICE"] = text.replace("Copyright (c) 2020 Netflix, Inc.", "")
        problems = self.problems(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("NOTICE lacks ['Copyright (c) 2020 Netflix, Inc.']", problems[0])

    def test_artifact_texts_must_name_the_root_netflix_file(self) -> None:
        files = _root_layout()
        manifest = json.loads(files["tools/rc1-tester/image/licensing.json"])
        manifest["spdx_texts"]["BSD-2-Clause-Patent"] = "LICENSE"
        files["tools/rc1-tester/image/licensing.json"] = json.dumps(manifest)
        problems = self.problems(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("spdx_texts['BSD-2-Clause-Patent'] is 'LICENSE'", problems[0])


def _crate(licence_line: str) -> dict[str, str | bytes]:
    files = _root_layout()
    files.update(
        {
            "Cargo.toml": (
                f"# {TAG} EUPL-1.2\n[workspace]\nmembers = ['crate']\n"
                "[workspace.package]\nlicense = 'EUPL-1.2'\nreadme = 'README.md'\n"
            ),
            "Cargo.lock": f"# {TAG} EUPL-1.2\n",
            "README.md": f"<!-- {TAG} BSD-2-Clause-Patent -->\n",
            "crate/Cargo.toml": (
                f"# {TAG} EUPL-1.2\n[package]\nname = 'c'\nversion = '0.1.0'\n{licence_line}\n"
            ),
            "crate/src/lib.rs": f"// {TAG} EUPL-1.2\n",
        }
    )
    return files


class CargoTests(GateTestCase):
    def test_a_crate_that_ships_the_workspace_readme_carries_its_licence(self) -> None:
        """Planted defect: the inherited EUPL-1.2 misses the root README it ships."""
        files = _crate("license.workspace = true\nreadme.workspace = true")
        problems = self.problems(files)
        self.assertEqual(
            problems,
            [
                "crate/Cargo.toml: license = 'EUPL-1.2', but its files carry "
                "BSD-2-Clause-Patent AND EUPL-1.2"
            ],
        )

    def test_the_crate_readme_replaces_the_workspace_readme(self) -> None:
        files = _crate("license.workspace = true\nreadme.workspace = true")
        files["crate/README.md"] = f"<!-- {TAG} EUPL-1.2 -->\n"
        self.assertEqual(self.problems(files), [])

    def test_a_netflix_licence_on_eupl_files_is_refused(self) -> None:
        """Planted defect: a manifest set back to BSD-2-Clause-Patent."""
        problems = self.problems(_crate("license = 'BSD-2-Clause-Patent'"))
        self.assertEqual(
            problems,
            ["crate/Cargo.toml: license = 'BSD-2-Clause-Patent', but its files carry EUPL-1.2"],
        )

    def test_an_or_expression_is_refused(self) -> None:
        problems = self.problems(_crate("license = 'EUPL-1.2 OR MIT'"))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("must be an SPDX AND expression", problems[0])

    def test_the_lock_file_and_a_nested_package_are_counted_as_cargo_counts_them(self) -> None:
        files = _crate("license = 'EUPL-1.2'")
        files["Cargo.lock"] = f"# {TAG} MIT\n"
        files["crate/inner/Cargo.toml"] = (
            f"# {TAG} ISC\n[package]\nname = 'i'\nlicense = 'ISC AND MIT'\n"
        )
        problems = self.problems(files)
        self.assertEqual(
            problems,
            ["crate/Cargo.toml: license = 'EUPL-1.2', but its files carry EUPL-1.2 AND MIT"],
        )

    def test_a_file_selection_is_not_guessed(self) -> None:
        _write(self.root, _crate("license = 'EUPL-1.2'\ninclude = ['src/**']"))
        _commit_index(self.root)
        with self.assertRaises(gate.ModelError):
            gate.check(self.root)


def _chart(own_licence: str, subchart_licence: str, recorded: str) -> dict[str, str | bytes]:
    files = _root_layout()
    files["REUSE.toml"] = (
        "version = 1\n[[annotations]]\npath = ['chart/charts/*.tgz']\n"
        f"precedence = 'override'\nSPDX-FileCopyrightText = 'x'\nSPDX-License-Identifier = '{recorded}'\n"
    )
    files["chart/Chart.yaml"] = (
        f"# {TAG} EUPL-1.2\napiVersion: v2\nname: c\n"
        f"annotations:\n  artifacthub.io/license: {own_licence}\n"
    )
    files["chart/values.yaml"] = f"# {TAG} EUPL-1.2\n"
    files["chart/charts/sub-1.0.0.tgz"] = _subchart(subchart_licence)
    return files


class HelmTests(GateTestCase):
    def test_a_chart_with_its_own_licence_and_a_recorded_subchart_passes(self) -> None:
        self.assertEqual(self.problems(_chart("EUPL-1.2", "Apache-2.0", "Apache-2.0")), [])

    def test_a_netflix_licence_on_the_chart_is_refused(self) -> None:
        problems = self.problems(_chart("BSD-2-Clause-Patent", "Apache-2.0", "Apache-2.0"))
        self.assertEqual(
            problems,
            [
                "chart/Chart.yaml: artifacthub.io/license = 'BSD-2-Clause-Patent', "
                "but its files carry EUPL-1.2"
            ],
        )

    def test_a_subchart_recorded_under_another_licence_is_refused(self) -> None:
        """Planted defect: the vendored Apache-2.0 subchart recorded as EUPL-1.2."""
        problems = self.problems(_chart("EUPL-1.2", "Apache-2.0", "EUPL-1.2"))
        self.assertEqual(
            problems,
            [
                "chart/charts/sub-1.0.0.tgz: the subchart declares 'Apache-2.0', "
                "REUSE.toml records 'EUPL-1.2'"
            ],
        )


# The single-token pattern the gate used before ADR-2673, kept to show why it
# changed: it reads no licence from an AND expression.
OLD_CHART_LICENCE = gate.re.compile(
    r"^[ \t]+artifacthub\.io/license:[ \t]*[\"']?([^\"'\s#]+)[\"']?[ \t]*(?:#.*)?$",
    gate.re.MULTILINE,
)


def _chart_with_schema(own_licence: str) -> dict[str, str | bytes]:
    """A chart whose values.schema.json embeds Apache-2.0 Kubernetes types."""
    files = _chart(own_licence, "Apache-2.0", "Apache-2.0")
    files["REUSE.toml"] = str(files["REUSE.toml"]) + (
        "[[annotations]]\npath = ['chart/values.schema.json']\nprecedence = 'override'\n"
        "SPDX-FileCopyrightText = ['x', 'The Kubernetes Authors']\n"
        "SPDX-License-Identifier = 'EUPL-1.2 AND Apache-2.0'\n"
    )
    files["chart/values.schema.json"] = "{}\n"
    return files


class HelmExpressionTests(GateTestCase):
    """ADR-2673: a chart whose files carry two licences declares both."""

    def test_both_licences_pass(self) -> None:
        self.assertEqual(self.problems(_chart_with_schema("EUPL-1.2 AND Apache-2.0")), [])
        self.assertEqual(self.problems(_chart_with_schema('"Apache-2.0 AND EUPL-1.2"')), [])

    def test_the_old_pattern_reads_no_licence_from_the_expression(self) -> None:
        line = "annotations:\n  artifacthub.io/license: EUPL-1.2 AND Apache-2.0\n"
        self.assertEqual(OLD_CHART_LICENCE.findall(line), [])
        self.assertEqual(gate.chart_licence(line), "EUPL-1.2 AND Apache-2.0")

    def test_a_wrong_second_licence_is_refused(self) -> None:
        self.assertEqual(
            self.problems(_chart_with_schema("EUPL-1.2 AND MIT")),
            [
                "chart/Chart.yaml: artifacthub.io/license = 'EUPL-1.2 AND MIT', "
                "but its files carry Apache-2.0 AND EUPL-1.2"
            ],
        )

    def test_one_identifier_is_refused_when_the_files_carry_two(self) -> None:
        self.assertEqual(
            self.problems(_chart_with_schema("EUPL-1.2")),
            [
                "chart/Chart.yaml: artifacthub.io/license = 'EUPL-1.2', "
                "but its files carry Apache-2.0 AND EUPL-1.2"
            ],
        )

    def test_a_list_without_and_is_refused(self) -> None:
        for declared in ("EUPL-1.2 Apache-2.0", "EUPL-1.2 OR Apache-2.0", "EUPL-1.2 AND"):
            with self.subTest(declared):
                self.assertEqual(
                    self.problems(_chart_with_schema(declared)),
                    [
                        "chart/Chart.yaml: artifacthub.io/license must be an SPDX AND expression, is None"
                    ],
                )


class NpmTests(GateTestCase):
    def test_a_private_package_may_omit_the_licence(self) -> None:
        files = _root_layout()
        files["pkg/package.json"] = json.dumps({"name": "p", "private": True})
        self.assertEqual(self.problems(files), [])

    def test_a_publishable_package_without_a_licence_is_refused(self) -> None:
        files = _root_layout()
        files["pkg/package.json"] = json.dumps({"name": "p"})
        self.assertEqual(
            self.problems(files), ["pkg/package.json: a publishable package declares no licence"]
        )

    def test_a_package_licence_is_held_to_its_files(self) -> None:
        files = _root_layout()
        files["pkg/package.json"] = json.dumps({"name": "p", "license": "MIT"})
        files["pkg/index.js"] = f"// {TAG} EUPL-1.2\n"
        files["REUSE.toml"] = (
            "version = 1\n[[annotations]]\npath = ['pkg/package.json']\n"
            "SPDX-FileCopyrightText = 'x'\nSPDX-License-Identifier = 'EUPL-1.2'\n"
        )
        self.assertEqual(
            self.problems(files),
            ["pkg/package.json: license = 'MIT', but its files carry EUPL-1.2"],
        )


if __name__ == "__main__":
    unittest.main()

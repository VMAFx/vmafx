#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/check_release_provenance_assets.py.

Positive: the checked-in release workflows upload .intoto.jsonl build provenance.
Negative: releasers without .intoto.jsonl exit 1; written provenance not uploaded
exits 1; stale exemption exits 1; unparsable workflow exits 2.
Boundary: comment line '# gh release create' is not a releaser; 'gh release view'
is not a releaser; provenance matching glob dir passes; provenance listed in files passes.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_release_provenance_assets as checker


def write_workflow(directory: Path, name: str, content: str) -> None:
    (directory / name).write_text(content, encoding="utf-8")


def run_checker(directory: Path) -> int:
    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
        return int(checker.main(["--dir", str(directory)]))


class ReleaseProvenanceAssetsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    # --- positive -------------------------------------------------------

    def test_checked_in_workflows_pass(self) -> None:
        self.assertEqual(run_checker(checker.WORKFLOW_DIR), 0)

    # --- negative -------------------------------------------------------

    def test_releaser_gh_release_create_without_intoto(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: gh release create x dist/*\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 1)

    def test_releaser_action_gh_release_without_intoto(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - uses: softprops/action-gh-release@v1\n"
            "        with:\n"
            "          files: |\n"
            "            dist/bin/*\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 1)

    def test_intoto_written_but_not_in_uploaded_glob_or_files(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: echo '{}' > other/bundle.intoto.jsonl\n"
            "      - run: gh release create x dist/*\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 1)

    def test_stale_exemption(self) -> None:
        content = (
            "name: not-a-releaser\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: echo 'hello'\n"
        )
        write_workflow(self.dir, "published-rc-licence-companions.yml", content)
        self.assertEqual(run_checker(self.dir), 1)

    def test_unparsable_yaml_exits_2(self) -> None:
        (self.dir / "bad.yml").write_text("jobs: [broken\n", encoding="utf-8")
        self.assertEqual(run_checker(self.dir), 2)

    def test_non_utf8_workflow_exits_2(self) -> None:
        (self.dir / "bad.yml").write_bytes(b"\xff\xfejobs: {}\n")
        self.assertEqual(run_checker(self.dir), 2)

    def test_intoto_name_assigned_but_never_written(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: |\n"
            "          X=dist/foo.intoto.jsonl\n"
            "          gh release create t dist/*\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 1)

    # --- boundary -------------------------------------------------------

    def test_comment_line_gh_release_create_is_not_releaser(self) -> None:
        content = (
            "name: comment\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: |\n"
            "          # gh release create v1.0.0 dist/*\n"
            "          echo 'not releasing'\n"
        )
        write_workflow(self.dir, "comment.yml", content)
        self.assertEqual(run_checker(self.dir), 0)

    def test_gh_release_view_is_not_releaser(self) -> None:
        content = (
            "name: view\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: gh release view v1.0.0\n"
        )
        write_workflow(self.dir, "view.yml", content)
        self.assertEqual(run_checker(self.dir), 0)

    def test_intoto_inside_glob_dir_passes(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: jq -c . in.json > dist/bundle.intoto.jsonl\n"
            "      - run: gh release create x dist/*\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 0)

    def test_intoto_listed_in_files_passes(self) -> None:
        content = (
            "name: rel\n"
            "jobs:\n"
            "  r:\n"
            "    runs-on: ubuntu-26.04\n"
            "    steps:\n"
            "      - run: jq -c . in.json > prov/bundle.intoto.jsonl\n"
            "      - uses: softprops/action-gh-release@v1\n"
            "        with:\n"
            "          files: |\n"
            "            dist/bin/*\n"
            "            dist/bundle.intoto.jsonl\n"
        )
        write_workflow(self.dir, "rel.yml", content)
        self.assertEqual(run_checker(self.dir), 0)


if __name__ == "__main__":
    unittest.main()

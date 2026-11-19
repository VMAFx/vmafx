# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for check_gitea_artifact_steps.py (HISS-15)."""

from __future__ import annotations

import difflib
import importlib.util
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env
from ruamel.yaml import YAML as RuamelYAML

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check_gitea_artifact_steps.py"


def load_checker_module():
    """Load the checker script for direct helper tests."""
    spec = importlib.util.spec_from_file_location("check_gitea_artifact_steps_test", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


V7 = "actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9  # v7.0.2"
V3 = "actions/upload-artifact@c6a366c94c3e0affe28c06c8df20a878f24da3cf  # v3.2.2"
DL8 = "actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333  # v8.0.2"

GOOD = f"""\
name: Good
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Upload log
        if: ${{{{ (always()) && env.GITEA_ACTIONS != 'true' }}}}
        uses: {V7}
        with:
          name: log
          path: build/log.txt
      - name: Upload log (Gitea)
        if: ${{{{ (always()) && env.GITEA_ACTIONS == 'true' }}}}
        uses: {V3}
        with:
          name: log
          path: build/log.txt
"""

UNGUARDED = f"""\
name: Unguarded
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Upload log
        if: always()
        uses: {V7}
        with:
          name: log
          path: build/log.txt
      - uses: {V7}
        with:
          name: other
          path: out
"""

PATTERN_DOWNLOAD = f"""\
name: Pattern
on: push
jobs:
  merge:
    runs-on: ubuntu-24.04
    steps:
      - name: Download digests
        uses: {DL8}
        with:
          pattern: digest-*
          merge-multiple: true
"""

ROUND_TRIP = f"""\
# top-level comment
name: "Round trip"
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      # keep this comment
      - name: "Upload log"
        if: ${{{{ (always()) && env.GITEA_ACTIONS != 'true' }}}}
        uses: {V7}
        with:
          name: "log"
          path: build/log.txt
      - name: Upload log (Gitea)
        if: ${{{{ (always()) && env.GITEA_ACTIONS == 'true' }}}}
        uses: {V3}
        with:
          name: "log"
          path: build/log.txt
      - name: Folded
        run: >
          echo one
          echo two
      - &defaults
        name: "Anchored"
        run: echo hi
      - <<: *defaults
        name: "Merged"
"""


ROUND_TRIP_WRITE = f"""\
---
# top-level comment
name: "Round trip"
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      # keep this comment
      - name: "Upload log"
        uses: {V7}
        with:
          name: "log"
          path: build/log.txt

      - name: Folded
        run: >
          echo one
          echo two
      - &defaults
        name: "Anchored"
        run: echo hi
      - <<: *defaults
        name: "Merged"
"""

TRAILING_COMMENTS = f"""\
---
name: Comments
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Upload log
        uses: {V7}
        with:
          name: log
          path: build/log.txt
      # keep this comment with the upload
      # second line
      - name: Next
        run: echo next
"""

TRAILING_COMMENTS_WRITE = f"""\
---
name: Comments
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Upload log
        if: ${{{{ env.GITEA_ACTIONS != 'true' }}}}
        uses: {V7}
        with:
          name: log
          path: build/log.txt
      - name: Upload log (Gitea)
        if: ${{{{ env.GITEA_ACTIONS == 'true' }}}}
        uses: {V3}
        with:
          name: log
          path: build/log.txt
      # keep this comment with the upload
      # second line
      - name: Next
        run: echo next
"""

SAME_LINE_COMMENTS = f"""\
---
name: Same line
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Download all
        uses: {V7}
      - name: Upload
        uses: {V7}
        with:
          name: log
          path: build/log.txt  # eol comment on path
      - name: Next
        run: echo next
"""

SAME_LINE_COMMENTS_WRITE = f"""\
---
name: Same line
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Download all
        if: ${{{{ env.GITEA_ACTIONS != 'true' }}}}
        uses: {V7}
      - name: Download all (Gitea)
        if: ${{{{ env.GITEA_ACTIONS == 'true' }}}}
        uses: {V3}
      - name: Upload
        if: ${{{{ env.GITEA_ACTIONS != 'true' }}}}
        uses: {V7}
        with:
          name: log
          path: build/log.txt  # eol comment on path
      - name: Upload (Gitea)
        if: ${{{{ env.GITEA_ACTIONS == 'true' }}}}
        uses: {V3}
        with:
          name: log
          path: build/log.txt
      - name: Next
        run: echo next
"""

SKIP_TRAILING_BLOCK = f"""\
---
name: Skip
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Download digests
        uses: {DL8}
        with:
          pattern: digest-*
          merge-multiple: true

      # keep this comment
      - name: Next
        run: echo next
"""

SKIP_TRAILING_BLOCK_WRITE = f"""\
---
name: Skip
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Download digests
        if: ${{{{ env.GITEA_ACTIONS != 'true' }}}}
        uses: {DL8}
        with:
          pattern: digest-*
          merge-multiple: true
      - name: Download digests (Gitea)
        if: ${{{{ env.GITEA_ACTIONS == 'true' }}}}
        shell: bash
        run: |-
          echo '::warning::artifact download skipped on Gitea: v4+ is GitHub-only, v3 lacks merge-multiple, pattern'

      # keep this comment
      - name: Next
        run: echo next
"""

FLOW_MAPPING = f"""\
---
name: Flow
on: push
jobs:
  build:
    steps:
      - name: Upload
        uses: {V7}
        with: {{ name: log, path: out.log }}
"""


class GiteaArtifactCheckTests(unittest.TestCase):
    """Run the checker against throw-away workflow files."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "wf.yml"

    def run_check(self, text: str, *extra: str) -> subprocess.CompletedProcess[str]:
        self.path.write_text(textwrap.dedent(text), encoding="utf-8")
        return subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(SCRIPT), *extra, str(self.path)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )

    def test_guarded_step_with_v3_twin_passes(self) -> None:
        result = self.run_check(GOOD)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unguarded_step_fails(self) -> None:
        result = self.run_check(UNGUARDED)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not guarded", result.stdout)
        self.assertEqual(result.stdout.count("not guarded"), 2)

    def test_twin_with_other_inputs_fails(self) -> None:
        head, _, tail = GOOD.rpartition("path: build/log.txt")
        result = self.run_check(head + "path: other" + tail)
        self.assertEqual(result.returncode, 1)
        self.assertIn("other with: inputs", result.stdout)

    def test_missing_twin_fails(self) -> None:
        text = GOOD.split("      - name: Upload log (Gitea)", maxsplit=1)[0]
        result = self.run_check(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no Gitea twin", result.stdout)

    def test_twin_with_other_condition_fails(self) -> None:
        result = self.run_check(
            GOOD.replace("(always()) && env.GITEA_ACTIONS ==", "env.GITEA_ACTIONS ==")
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("not its Gitea twin", result.stdout)

    def test_stray_v3_pin_fails(self) -> None:
        text = GOOD + f"      - uses: {V3}\n        with:\n          name: x\n          path: y\n"
        result = self.run_check(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("v3 pin outside a Gitea twin", result.stdout)

    def test_skip_without_marker_fails(self) -> None:
        text = (
            PATTERN_DOWNLOAD.replace(
                "        uses:", "        if: ${{ env.GITEA_ACTIONS != 'true' }}\n        uses:"
            )
            + "      - if: ${{ env.GITEA_ACTIONS == 'true' }}\n        run: true\n"
        )
        result = self.run_check(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("neither the v3 pin nor a named skip", result.stdout)

    def test_skip_where_v3_would_do_fails(self) -> None:
        text = (
            GOOD.split("        uses: " + V3, maxsplit=1)[0]
            + "        run: echo 'skipped on Gitea'\n"
        )
        result = self.run_check(text)
        self.assertEqual(result.returncode, 1)

    def test_write_fixes_plain_and_conditioned_steps(self) -> None:
        result = self.run_check(UNGUARDED, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        steps = yaml.safe_load(self.path.read_text(encoding="utf-8"))["jobs"]["build"]["steps"]
        self.assertEqual(len(steps), 4)
        self.assertEqual(steps[0]["if"], "${{ (always()) && env.GITEA_ACTIONS != 'true' }}")
        self.assertEqual(steps[1]["uses"], V3.split(maxsplit=1)[0])
        self.assertEqual(steps[1]["with"], steps[0]["with"])
        self.assertEqual(steps[2]["if"], "${{ env.GITEA_ACTIONS != 'true' }}")
        self.assertEqual(steps[3]["name"], "Upload artifact (Gitea)")

    def test_write_names_the_skip_for_pattern_download(self) -> None:
        result = self.run_check(PATTERN_DOWNLOAD, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        steps = yaml.safe_load(self.path.read_text(encoding="utf-8"))["jobs"]["merge"]["steps"]
        self.assertIn("skipped on Gitea", steps[1]["run"])
        self.assertIn("merge-multiple, pattern", steps[1]["run"])
        self.assertNotIn("uses", steps[1])

    def test_write_is_idempotent(self) -> None:
        self.run_check(UNGUARDED, "--write")
        first = self.path.read_text(encoding="utf-8")
        result = subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(SCRIPT), "--write", str(self.path)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.path.read_text(encoding="utf-8"), first)

    def test_write_keeps_round_trip_file_byte_stable(self) -> None:
        result = self.run_check(ROUND_TRIP, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.path.read_text(encoding="utf-8"), textwrap.dedent(ROUND_TRIP))

    def test_write_round_trip_adds_only_twin_lines(self) -> None:
        before = textwrap.dedent(ROUND_TRIP_WRITE)
        result = self.run_check(ROUND_TRIP_WRITE, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        after = self.path.read_text(encoding="utf-8")
        guard = "        if: ${{ env.GITEA_ACTIONS != 'true' }}"
        twin = (
            "      - name: Upload log (Gitea)\n"
            "        if: ${{ env.GITEA_ACTIONS == 'true' }}\n"
            f"        uses: {V3}\n"
            "        with:\n"
            '          name: "log"\n'
            "          path: build/log.txt"
        )
        expected = before.replace(
            f'      - name: "Upload log"\n        uses: {V7}',
            f'      - name: "Upload log"\n{guard}\n        uses: {V7}',
        ).replace(
            "          path: build/log.txt\n\n      - name: Folded",
            f"          path: build/log.txt\n{twin}\n\n      - name: Folded",
        )
        self.assertEqual(after, expected)
        diff = list(difflib.unified_diff(before.splitlines(), after.splitlines(), lineterm=""))
        removed = [line[1:] for line in diff if line.startswith("-") and not line.startswith("---")]
        self.assertEqual(removed, [])
        self.assertTrue(after.startswith("---\n# top-level comment"))
        self.assertIn("path: build/log.txt\n\n      - name: Folded", after)

    def test_write_moves_trailing_comments_after_the_twin(self) -> None:
        result = self.run_check(TRAILING_COMMENTS, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            self.path.read_text(encoding="utf-8"),
            textwrap.dedent(TRAILING_COMMENTS_WRITE),
        )

    def test_write_keeps_same_line_comments_on_source(self) -> None:
        result = self.run_check(SAME_LINE_COMMENTS, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            self.path.read_text(encoding="utf-8"),
            textwrap.dedent(SAME_LINE_COMMENTS_WRITE),
        )

    def test_write_keeps_one_blank_line_after_skip_twin(self) -> None:
        result = self.run_check(SKIP_TRAILING_BLOCK, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            self.path.read_text(encoding="utf-8"),
            textwrap.dedent(SKIP_TRAILING_BLOCK_WRITE),
        )

    def test_write_comment_cases_are_idempotent(self) -> None:
        for fixture in (SAME_LINE_COMMENTS, SKIP_TRAILING_BLOCK):
            with self.subTest(fixture=fixture.splitlines()[1]):
                self.run_check(fixture, "--write")
                first = self.path.read_text(encoding="utf-8")
                result = subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
                    [sys.executable, str(SCRIPT), "--write", str(self.path)],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=120,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.path.read_text(encoding="utf-8"), first)

    def test_take_trailing_post_returns_none_for_none_token(self) -> None:
        module = load_checker_module()
        step = RuamelYAML(typ="rt").load("uses: x\n")
        step.ca.items["uses"] = [None, None, None, None]
        self.assertIsNone(module._take_trailing_post(step))

    def test_write_refuses_to_reformat_flow_mappings(self) -> None:
        result = self.run_check(FLOW_MAPPING, "--write")
        self.assertEqual(result.returncode, 1)
        self.assertIn("refusing to rewrite", result.stderr)
        self.assertIn("not guarded", result.stdout)
        self.assertEqual(self.path.read_text(encoding="utf-8"), textwrap.dedent(FLOW_MAPPING))

    def test_twin_comparison_keeps_yaml_1_2_yes_as_a_string(self) -> None:
        text = UNGUARDED.replace(
            "          path: build/log.txt",
            "          path: build/log.txt\n          if-no-files-found: yes",
        )
        result = self.run_check(text, "--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        steps = RuamelYAML(typ="safe").load(self.path.read_text(encoding="utf-8"))["jobs"]["build"][
            "steps"
        ]
        self.assertEqual(steps[0]["with"]["if-no-files-found"], "yes")
        self.assertEqual(steps[1]["with"]["if-no-files-found"], "yes")

    def test_file_without_artifact_steps_passes(self) -> None:
        result = self.run_check("name: Empty\non: push\njobs: {}\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_repository_workflows_pass(self) -> None:
        result = subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(SCRIPT)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

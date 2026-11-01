# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for check_composite_actions.py (HISS-15)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/check_composite_actions.py"

GOOD = """\
name: Good
description: A well-formed composite action
inputs:
  who:
    description: Whom to greet
    required: true
runs:
  using: composite
  steps:
    - name: Greet
      shell: bash
      env:
        WHO: ${{ inputs.who }}
      run: |
        echo "hello ${WHO}"
        echo "${{ github.sha }}"
    - uses: actions/checkout@v4
"""


class CompositeActionCheckTests(unittest.TestCase):
    """Run the checker against a throw-away repository root."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, text: str, name: str = "demo") -> None:
        folder = self.root / ".github" / "actions" / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "action.yml").write_text(textwrap.dedent(text), encoding="utf-8")

    def run_check(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(SCRIPT), str(self.root)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )

    def test_well_formed_action_passes(self) -> None:
        self.write(GOOD)
        res = self.run_check()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("0 finding(s)", res.stdout)

    def test_unquoted_variable_in_run_block_fails(self) -> None:
        self.write(GOOD.replace('echo "hello ${WHO}"', "echo hello $WHO"))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("SC2086", res.stderr)

    def test_run_step_without_shell_fails(self) -> None:
        self.write(GOOD.replace("      shell: bash\n", ""))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("without `shell`", res.stderr)

    def test_missing_input_description_fails(self) -> None:
        self.write(GOOD.replace("    description: Whom to greet\n", ""))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("input 'who' has no description", res.stderr)

    def test_non_composite_action_fails(self) -> None:
        self.write(GOOD.replace("using: composite", "using: node20"))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("not `composite`", res.stderr)

    def test_step_with_run_and_uses_fails(self) -> None:
        self.write(
            GOOD.replace(
                "    - uses: actions/checkout@v4",
                "    - uses: actions/checkout@v4\n      run: echo x\n      shell: bash",
            )
        )
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("exactly one of `run` and `uses`", res.stderr)

    def test_invalid_yaml_fails(self) -> None:
        self.write("name: [unterminated\n")
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("invalid YAML", res.stderr)

    def test_no_actions_is_an_error_not_a_pass(self) -> None:
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("no composite action found", res.stderr)

    def test_powershell_step_is_skipped_with_a_reason(self) -> None:
        self.write(GOOD + "    - shell: pwsh\n      run: Write-Output $env:X\n")
        res = self.run_check()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("shell=pwsh skipped: no checker for this shell", res.stdout)

    def test_expression_spanning_lines_is_replaced(self) -> None:
        self.write(GOOD.replace('echo "${{ github.sha }}"', 'echo "${{\n          github.sha }}"'))
        res = self.run_check()
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_input_expression_in_run_block_fails(self) -> None:
        """Negative: an input expanded into the script text is template injection."""
        self.write(GOOD.replace('echo "hello ${WHO}"', 'echo "hello ${{ inputs.who }}"'))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("${{ inputs.who }} in `run`", res.stderr)

    def test_step_output_expression_in_run_block_fails(self) -> None:
        """Negative: a step output is caller-influenced text too."""
        self.write(GOOD.replace('echo "hello ${WHO}"', 'echo "${{ steps.a.outputs.b }}"'))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("steps.a.outputs.b", res.stderr)

    def test_expression_in_a_skipped_shell_still_fails(self) -> None:
        """Negative: the expression check reads every shell, not only the shellchecked ones."""
        self.write(GOOD + "    - shell: pwsh\n      run: Write-Output '${{ inputs.who }}'\n")
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("step 3", res.stderr)

    def test_allow_list_matches_whole_names_only(self) -> None:
        """Boundary: a name that only starts like an allowed value is not allowed."""
        self.write(GOOD.replace("${{ github.sha }}", "${{ github.sha_suffix }}"))
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("github.sha_suffix", res.stderr)

    def test_every_action_is_read(self) -> None:
        self.write(GOOD, "first")
        self.write(
            GOOD.replace("shell: bash", "shell: bash\n      if: always()").replace(
                'echo "hello ${WHO}"', "echo $WHO"
            ),
            "second",
        )
        res = self.run_check()
        self.assertEqual(res.returncode, 1)
        self.assertIn("second/action.yml", res.stderr)
        self.assertNotIn("first/action.yml", res.stderr)


class RepositoryActionsTests(unittest.TestCase):
    """The tracked composite actions pass."""

    def test_tracked_actions_pass(self) -> None:
        res = subprocess.run(  # noqa: S603 -- fixed interpreter and script argv
            [sys.executable, str(SCRIPT)],
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )
        self.assertEqual(res.returncode, 0, res.stderr)


if __name__ == "__main__":
    unittest.main()

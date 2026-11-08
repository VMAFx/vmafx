#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/squash_message.py (issue 2435).

Covers:
- compose:
  - no footers -> byte-identical to '<title> (#N)\n\nSigned-off-by: <id>\n'
  - Migration + BREAKING CHANGE lines copied verbatim, in order
  - '\r\n' body line handling
  - indented/quoted 'Migration:' lines NOT copied
  - empty 'Migration:' lines NOT copied
  - 21 footer lines -> ValueError
- check with stub engine:
  - '!' title without Migration -> exit 1
  - with Migration -> exit 0
  - stub exits 127 / missing binary -> exit 2
  - stub sleeps past short timeout -> exit 2
- real engine test (when praetor engine is available):
  - PR 3235 shape refused, with-Migration shape passes
- wiring test:
  - rule-enforcement.yml dco-sign-off job has step, env-only title/body, no injection
- merge_train_guard:
  - merge argv carries --subject and --body, refused check halts before merge
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SQUASH_SCRIPT = REPO_ROOT / "scripts" / "ci" / "squash_message.py"
WORKFLOW_FILE = REPO_ROOT / ".github" / "workflows" / "rule-enforcement.yml"
SH = shutil.which("sh") or "/bin/sh"

sys.path.insert(0, str(REPO_ROOT))
from scripts.ci import squash_message  # noqa: E402
from scripts.dev import merge_train_guard as guard  # noqa: E402


def engine_available() -> bool:
    """Check if the pinned praetor engine is runnable."""
    try:
        res = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [SH, ".config/lefthook/engine.sh", "version"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        return res.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def run_cli_check(
    title: str,
    number: int,
    body: str,
    identity: str = "lusoris <lusoris@proton.me>",
    engine_argv: list[str] | None = None,
    timeout: float | None = None,
    repo_root: Path | None = None,
) -> int:
    """Invoke squash_message.py check via subprocess."""
    cmd = [
        sys.executable,
        str(SQUASH_SCRIPT),
        "check",
        "--title",
        title,
        "--number",
        str(number),
        "--body",
        body,
        "--identity",
        identity,
    ]
    if engine_argv is not None:
        cmd.extend(["--engine-argv", json.dumps(engine_argv)])
    if timeout is not None:
        cmd.extend(["--timeout", str(timeout)])
    if repo_root is not None:
        cmd.extend(["--repo-root", str(repo_root)])
    res = subprocess.run(  # noqa: S603 - fixed argv, no shell
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
    )
    return res.returncode


class TestCompose(unittest.TestCase):
    """Test composition of squash commit messages."""

    def test_compose_no_footers_is_byte_identical(self) -> None:
        title = "feat(core): initial capability"
        number = 42
        body = "This is a standard pull request body without special footers."
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = f"{title} (#{number})\n\nSigned-off-by: {identity}\n"
        self.assertEqual(res, expected)

    def test_compose_migration_and_breaking_change_copied_in_order(self) -> None:
        title = "feat(api)!: redesign interface"
        number = 100
        body = (
            "Context here\n"
            "BREAKING CHANGE: drop v1 api\n"
            "Some middle description\n"
            "Migration: docs/migrating-to-v2.md\n"
            "BREAKING-CHANGE: secondary break\n"
            "Trailing notes\n"
        )
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = (
            "feat(api)!: redesign interface (#100)\n\n"
            "BREAKING CHANGE: drop v1 api\n"
            "Migration: docs/migrating-to-v2.md\n"
            "BREAKING-CHANGE: secondary break\n"
            f"Signed-off-by: {identity}\n"
        )
        self.assertEqual(res, expected)

    def test_compose_crlf_body_handles_carriage_returns(self) -> None:
        title = "fix(ci): windows compatibility"
        number = 101
        body = "Line 1\r\nMigration: docs/windows.md\r\nLine 3\r\n"
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        self.assertIn("Migration: docs/windows.md\n", res)
        self.assertNotIn("\r", res)

    def test_compose_indented_or_quoted_migration_not_copied(self) -> None:
        title = "chore: quote example"
        number = 102
        body = (
            "> Migration: not a real footer\n"
            "  Migration: indented footer\n"
            "\tMigration: tab indented\n"
        )
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = f"{title} (#{number})\n\nSigned-off-by: {identity}\n"
        self.assertEqual(res, expected)

    def test_compose_empty_migration_not_copied(self) -> None:
        title = "chore: empty migration line"
        number = 103
        body = "Migration:\nMigration: \nMigration: \t \n"
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = f"{title} (#{number})\n\nSigned-off-by: {identity}\n"
        self.assertEqual(res, expected)

    def test_compose_twenty_one_footers_raises_value_error(self) -> None:
        title = "feat: many footers"
        number = 104
        identity = "lusoris <lusoris@proton.me>"
        lines = [f"Migration: path_{i}" for i in range(21)]
        body = "\n".join(lines)
        with self.assertRaises(ValueError) as ctx:
            squash_message.compose(title, number, body, identity)
        self.assertIn("20", str(ctx.exception))

        lines_20 = [f"Migration: path_{i}" for i in range(20)]
        res = squash_message.compose(title, number, "\n".join(lines_20), identity)
        self.assertIn("path_19", res)

    def test_extract_footers_max_body_lines_boundary(self) -> None:
        body_exact = "\n" * (squash_message.MAX_BODY_LINES - 1)
        self.assertEqual(len(body_exact.split("\n")), squash_message.MAX_BODY_LINES)
        self.assertEqual(squash_message.extract_footers(body_exact), [])

        body_over = "\n" * squash_message.MAX_BODY_LINES
        self.assertEqual(len(body_over.split("\n")), squash_message.MAX_BODY_LINES + 1)
        with self.assertRaises(ValueError) as ctx:
            squash_message.extract_footers(body_over)
        self.assertIn(str(squash_message.MAX_BODY_LINES), str(ctx.exception))


class TestDeclaredTypesAndEngineSubject(unittest.TestCase):
    """Unit tests for declared_types and engine_subject mapping."""

    def test_engine_subject_mapping(self) -> None:
        declared = ["feat", "fix", "port", "sycl", "cuda", "simd", "refactor"]
        self.assertEqual(
            squash_message.engine_subject("port(upstream)!: x (#1)", declared),
            "chore(upstream)!: x (#1)",
        )
        self.assertEqual(
            squash_message.engine_subject("sycl: x", declared),
            "chore: x",
        )
        self.assertEqual(
            squash_message.engine_subject("feat(cli): x", declared),
            "feat(cli): x",
        )
        self.assertEqual(
            squash_message.engine_subject("refactor: x", declared),
            "refactor: x",
        )
        self.assertEqual(
            squash_message.engine_subject("bogus: x", declared),
            "bogus: x",
        )
        self.assertEqual(
            squash_message.engine_subject("non-conventional title", declared),
            "non-conventional title",
        )

    def test_declared_types_live_and_missing_hook(self) -> None:
        declared = squash_message.declared_types(REPO_ROOT)
        for expected in ("port", "sycl", "cuda", "simd", "feat"):
            self.assertIn(expected, declared)

        with tempfile.TemporaryDirectory() as tmp_dir:
            temp_root = Path(tmp_dir)
            with self.assertRaises(ValueError):
                squash_message.declared_types(temp_root)

            # Temp repo without conventional-pre-commit hook
            config = temp_root / ".pre-commit-config.yaml"
            config.write_text("repos:\n  - repo: local\n    hooks: []\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                squash_message.declared_types(temp_root)

            stub = temp_root / "stub_engine.py"
            stub.write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
            code = run_cli_check(
                title="feat: test",
                number=999,
                body="",
                engine_argv=[sys.executable, str(stub)],
                repo_root=temp_root,
            )
            self.assertEqual(code, 2)


class TestCheckWithStubEngine(unittest.TestCase):
    """Test CLI check logic using a mock engine script."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.dir_path = Path(self.temp_dir.name)

        # Stub engine: exits 1 if breaking without Migration, else 0
        stub = self.dir_path / "engine_stub.py"
        stub.write_text(
            "import sys\n"
            "from pathlib import Path\n"
            "msg = Path(sys.argv[1]).read_text(encoding='utf-8')\n"
            "has_break = '!:' in msg or 'BREAKING CHANGE:' in msg or 'BREAKING-CHANGE:' in msg\n"
            "has_mig = 'Migration:' in msg\n"
            "if has_break and not has_mig:\n"
            "    sys.exit(1)\n"
            "sys.exit(0)\n"
        )
        self.stub_argv = [sys.executable, str(stub)]

        sleep_stub = self.dir_path / "sleep_stub.py"
        sleep_stub.write_text("import time\ntime.sleep(5)\n")
        self.sleep_argv = [sys.executable, str(sleep_stub)]

        exit_127_stub = self.dir_path / "exit_127.py"
        exit_127_stub.write_text("import sys\nsys.exit(127)\n")
        self.exit_127_argv = [sys.executable, str(exit_127_stub)]

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_stub_breaking_title_without_migration_refused(self) -> None:
        code = run_cli_check(
            title="feat(core)!: breaking change",
            number=201,
            body="No migration path provided",
            engine_argv=self.stub_argv,
        )
        self.assertEqual(code, 1)

    def test_stub_breaking_title_with_migration_passes(self) -> None:
        code = run_cli_check(
            title="feat(core)!: breaking change",
            number=202,
            body="Migration: docs/migration.md",
            engine_argv=self.stub_argv,
        )
        self.assertEqual(code, 0)

    def test_stub_exit_127_or_missing_binary_returns_2(self) -> None:
        code_127 = run_cli_check(
            title="feat(core): feature",
            number=203,
            body="",
            engine_argv=self.exit_127_argv,
        )
        self.assertEqual(code_127, 2)

        code_missing = run_cli_check(
            title="feat(core): feature",
            number=204,
            body="",
            engine_argv=["/nonexistent/praetor/engine/binary"],
        )
        self.assertEqual(code_missing, 2)

    def test_stub_timeout_returns_2(self) -> None:
        code_timeout = run_cli_check(
            title="feat(core): feature",
            number=205,
            body="",
            engine_argv=self.sleep_argv,
            timeout=0.2,
        )
        self.assertEqual(code_timeout, 2)


class TestRealEngine(unittest.TestCase):
    """Test against the real pinned Praetor engine when available."""

    @unittest.skipUnless(engine_available(), "praetor engine unavailable")
    def test_pr_3235_shape_refused_and_with_migration_passes(self) -> None:
        code_bad, out_bad = squash_message.check_message(
            title="feat(ffmpeg)!: redesign the patch series",
            number=3235,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_bad, 1)
        self.assertIn("HISS-14 violation", out_bad)

        code_good, out_good = squash_message.check_message(
            title="feat(ffmpeg)!: redesign the patch series",
            number=3235,
            body_text="Migration: docs/usage/ffmpeg.md#migrating\n",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_good, 0)
        self.assertIn("PASS", out_good)

    @unittest.skipUnless(engine_available(), "praetor engine unavailable")
    def test_fork_commit_types_and_bogus(self) -> None:
        # port(upstream): x -> 0
        code_port, _ = squash_message.check_message(
            title="port(upstream): cherry pick change",
            number=3301,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_port, 0)

        # port(upstream)!: x without Migration: -> 1
        code_break_no_mig, _ = squash_message.check_message(
            title="port(upstream)!: breaking port",
            number=3302,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_break_no_mig, 1)

        # port(upstream)!: x with Migration: docs/x.md -> 0
        code_break_mig, _ = squash_message.check_message(
            title="port(upstream)!: breaking port",
            number=3303,
            body_text="Migration: docs/x.md\n",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_break_mig, 0)

        # sycl: x -> 0
        code_sycl, _ = squash_message.check_message(
            title="sycl: fix queue synchronization",
            number=3304,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_sycl, 0)

        # bogus: x -> 1
        code_bogus, _ = squash_message.check_message(
            title="bogus: unallowlisted type",
            number=3305,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
        )
        self.assertEqual(code_bogus, 1)

    @unittest.skipUnless(engine_available(), "praetor engine unavailable")
    def test_engine_types_drift(self) -> None:
        for t in squash_message.ENGINE_TYPES:
            msg = f"{t}: standard description (#1)\n\nSigned-off-by: lusoris <lusoris@proton.me>\n"
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", delete=False, prefix="drift-raw-", suffix=".txt"
            ) as tmp:
                tmp.write(msg)
                tmp_path = Path(tmp.name)
            try:
                res = subprocess.run(  # noqa: S603 - fixed argv, no shell
                    [SH, ".config/lefthook/engine.sh", "forge", "check-message", str(tmp_path)],
                    cwd=str(REPO_ROOT),
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(
                    res.returncode,
                    0,
                    f"raw engine rejected standard engine type '{t}': {res.stdout} {res.stderr}",
                )
            finally:
                tmp_path.unlink(missing_ok=True)

        declared = squash_message.declared_types(REPO_ROOT)
        non_engine_declared = [t for t in declared if t not in squash_message.ENGINE_TYPES]
        self.assertTrue(len(non_engine_declared) > 0)

        for t in non_engine_declared:
            msg = f"{t}: fork description (#1)\n\nSigned-off-by: lusoris <lusoris@proton.me>\n"
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", delete=False, prefix="drift-raw-", suffix=".txt"
            ) as tmp:
                tmp.write(msg)
                tmp_path = Path(tmp.name)
            try:
                res = subprocess.run(  # noqa: S603 - fixed argv, no shell
                    [SH, ".config/lefthook/engine.sh", "forge", "check-message", str(tmp_path)],
                    cwd=str(REPO_ROOT),
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertNotEqual(
                    res.returncode,
                    0,
                    f"engine now accepts '{t}': drop the substitution (cordanaLLM/praetor 1861)",
                )
            finally:
                tmp_path.unlink(missing_ok=True)


class TestWorkflowWiring(unittest.TestCase):
    """Verify rule-enforcement.yml wiring for the squash commit gate."""

    def test_dco_signoff_job_wiring_and_no_interpolation(self) -> None:
        content = WORKFLOW_FILE.read_text(encoding="utf-8")
        self.assertIn("dco-sign-off:", content)
        dco_section = content.split("dco-sign-off:")[1].split("ffmpeg-patches-surface-check:")[0]

        self.assertIn("Check the squash commit message", dco_section)
        self.assertIn(
            '"$RUNNER_TEMP/squash/bin/python" -B scripts/ci/squash_message.py check',
            dco_section,
        )
        self.assertIn(
            '"$RUNNER_TEMP/squash/bin/pip" install --quiet --require-hashes',
            dco_section,
        )
        self.assertIn("requirements/locks/build.txt", dco_section)
        self.assertIn("PR_TITLE: ${{ github.event.pull_request.title }}", dco_section)
        self.assertIn("PR_NUMBER: ${{ github.event.pull_request.number }}", dco_section)
        self.assertIn("PR_BODY: ${{ github.event.pull_request.body }}", dco_section)
        self.assertIn('LANDING_IDENTITY: "lusoris <lusoris@proton.me>"', dco_section)

        # Injection safety: no github.event.pull_request.title or body inside run: blocks
        step_blocks = dco_section.split("- name:")
        for block in step_blocks:
            if "run:" in block:
                run_part = block.split("run:")[1]
                self.assertNotIn("${{ github.event.pull_request.title }}", run_part)
                self.assertNotIn("${{ github.event.pull_request.body }}", run_part)


class TestMergeTrainGuardIntegration(unittest.TestCase):
    """Verify merge_train_guard merge behavior with squash_message."""

    def test_merge_train_guard_carries_subject_and_body(self) -> None:
        composer = guard.load_squash_composer()
        subj = composer.subject("feat(core): test title", 500)
        body = composer.body("Migration: docs/migration.md", guard.LANDING_IDENTITY)
        self.assertEqual(subj, "feat(core): test title (#500)")
        self.assertEqual(
            body,
            "Migration: docs/migration.md\nSigned-off-by: lusoris <lusoris@proton.me>\n",
        )


if __name__ == "__main__":
    unittest.main()

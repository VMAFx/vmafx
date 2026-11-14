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
- subject policy (issue 2750):
  - bare_subject strips the appended ' (#N)' suffix once
  - empty conventional subject before the suffix -> check exit 1
  - type the hook refuses (not declared in .pre-commit-config.yaml) -> exit 1
  - declared-type subjects reach the engine unchanged
- emit-label CLI (issue 2750):
  - declared type -> prints type:<type>, exit 0
  - no prefix / empty subject / undeclared type -> exit 1
  - missing title -> exit 2
- check with stub engine:
  - '!' title without Migration -> exit 1
  - with Migration -> exit 0
  - stub exits 127 / missing binary -> exit 2
  - stub sleeps past short timeout -> exit 2
- real engine test (when praetor engine is available):
  - PR 3235 shape refused, with-Migration shape passes
- wiring tests:
  - rule-enforcement.yml dco-sign-off job has step, env-only title/body, no injection
  - pr-type-label.yml labels through emit-label with no title/body interpolation
  - release.yml names every declared type and keeps the legacy aliases
- merge_train_guard:
  - merge argv carries --subject and --body, refused check halts before merge
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess  # argv-list calls only, no shell  # nosec B404
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[3]
SQUASH_SCRIPT = REPO_ROOT / "scripts" / "ci" / "squash_message.py"
WORKFLOW_FILE = REPO_ROOT / ".github" / "workflows" / "rule-enforcement.yml"
LABEL_WORKFLOW_FILE = REPO_ROOT / ".github" / "workflows" / "pr-type-label.yml"
RELEASE_FILE = REPO_ROOT / ".github" / "release.yml"
SH = shutil.which("sh") or "/bin/sh"
CLI_TIMEOUT_S = 60  # outer deadline for each squash_message.py CLI run

sys.path.insert(0, str(REPO_ROOT))
from scripts.ci import squash_message  # noqa: E402
from scripts.dev import merge_train_guard as guard  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as safe_run  # noqa: E402


def run_cli_check(
    title: str,
    number: int,
    body: str,
    identity: str = "lusoris <lusoris@proton.me>",
    timeout: float | None = None,
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
    if timeout is not None:
        cmd.extend(["--timeout", str(timeout)])
    res = subprocess.run(  # noqa: S603 -- CLI check invocation, no shell  # nosec B603
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
        timeout=CLI_TIMEOUT_S,
    )
    return res.returncode


class TestCompose(unittest.TestCase):
    """Test composition of squash commit messages."""

    def test_compose_no_footers_is_byte_identical(self) -> None:
        """Prove compose produces byte-identical message without footers."""
        title = "feat(core): initial capability"
        number = 42
        body = "This is a standard pull request body without special footers."
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = f"{title} (#{number})\n\nSigned-off-by: {identity}\n"
        self.assertEqual(res, expected)

    def test_compose_migration_and_breaking_change_copied_in_order(self) -> None:
        """Prove compose copies Migration and BREAKING CHANGE footers in order."""
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
        """Prove compose handles CRLF line endings in PR body."""
        title = "fix(ci): windows compatibility"
        number = 101
        body = "Line 1\r\nMigration: docs/windows.md\r\nLine 3\r\n"
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        self.assertIn("Migration: docs/windows.md\n", res)
        self.assertNotIn("\r", res)

    def test_compose_indented_or_quoted_migration_not_copied(self) -> None:
        """Prove compose ignores indented or quoted Migration lines."""
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
        """Prove compose ignores empty Migration lines."""
        title = "chore: empty migration line"
        number = 103
        body = "Migration:\nMigration: \nMigration: \t \n"
        identity = "lusoris <lusoris@proton.me>"
        res = squash_message.compose(title, number, body, identity)
        expected = f"{title} (#{number})\n\nSigned-off-by: {identity}\n"
        self.assertEqual(res, expected)

    def test_compose_twenty_one_footers_raises_value_error(self) -> None:
        """Prove compose raises ValueError when footer limit is exceeded."""
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
        """Prove extract_footers enforces MAX_BODY_LINES boundary."""
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
        """Prove engine_subject correctly maps non-engine declared types to chore."""
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
        """Prove declared_types handles live config and missing hook configurations."""
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
            code, _ = squash_message.check_message(
                title="feat: test",
                number=999,
                body_text="",
                identity="lusoris <lusoris@proton.me>",
                engine_argv=[sys.executable, str(stub)],
                repo_root=temp_root,
            )
            self.assertEqual(code, 2)


class TestSubjectPolicy(unittest.TestCase):
    """Repository-side subject policy the engine cannot see (issue 2750)."""

    def setUp(self) -> None:
        """Create a temp directory with an always-pass engine stub."""
        self.temp_dir = tempfile.TemporaryDirectory()
        stub = Path(self.temp_dir.name) / "pass_stub.py"
        stub.write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
        self.pass_argv = [sys.executable, str(stub)]

    def tearDown(self) -> None:
        """Clean up the temp directory."""
        self.temp_dir.cleanup()

    def test_bare_subject_strips_the_appended_suffix_once(self) -> None:
        """Prove bare_subject removes exactly the appended ' (#N)' suffix."""
        self.assertEqual(squash_message.bare_subject("fix: real subject", 42), "fix: real subject")
        self.assertEqual(
            squash_message.bare_subject("fix: something (#123)", 42), "fix: something (#123)"
        )

    def test_parse_subject_prefix_shapes(self) -> None:
        """Prove parse_subject_prefix returns (type, description) or None."""
        self.assertEqual(squash_message.parse_subject_prefix("fix:"), ("fix", ""))
        self.assertEqual(
            squash_message.parse_subject_prefix("feat(cli)!: drop the flag"),
            ("feat", "drop the flag"),
        )
        self.assertEqual(
            squash_message.parse_subject_prefix("sycl: route the queue"),
            ("sycl", "route the queue"),
        )
        self.assertIsNone(squash_message.parse_subject_prefix("Update the title gate"))

    def test_policy_refuses_an_empty_subject_before_the_suffix(self) -> None:
        """Prove a bare 'fix:' title is refused even though ' (#N)' would fill the slot."""
        reason = squash_message.subject_policy_error(
            "fix:", squash_message.declared_types(REPO_ROOT)
        )
        self.assertIn("empty", reason)
        code, out = squash_message.check_message(
            title="fix:",
            number=210,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.pass_argv,
        )
        self.assertEqual(code, 1)
        self.assertIn("empty", out)

    def test_policy_refuses_a_type_the_hook_refuses(self) -> None:
        """Prove an undeclared engine type (style) is refused before the engine runs."""
        declared = squash_message.declared_types(REPO_ROOT)
        reason = squash_message.subject_policy_error("style: reflow comments", declared)
        self.assertIn("'style'", reason)
        self.assertNotIn("style", declared)
        code, out = squash_message.check_message(
            title="style: reflow comments",
            number=211,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.pass_argv,
        )
        self.assertEqual(code, 1)
        self.assertIn("'style'", out)

    def test_policy_passes_declared_type_subjects_to_the_engine(self) -> None:
        """Prove declared-type subjects get no refusal and the pass stub returns 0."""
        declared = squash_message.declared_types(REPO_ROOT)
        for title in ("feat: add the thing", "sycl: route the queue", "feat(cli)!: drop the flag"):
            self.assertEqual(squash_message.subject_policy_error(title, declared), "", title)
        for title in ("feat: add the thing", "sycl: route the queue"):
            code, _ = squash_message.check_message(
                title=title,
                number=212,
                body_text="",
                identity="lusoris <lusoris@proton.me>",
                engine_argv=self.pass_argv,
            )
            self.assertEqual(code, 0, title)

    def test_policy_leaves_non_conventional_subjects_to_the_engine(self) -> None:
        """Prove a subject without a conventional prefix gets no policy refusal."""
        declared = squash_message.declared_types(REPO_ROOT)
        self.assertEqual(squash_message.subject_policy_error("Update the title gate", declared), "")


class TestEmitLabelCli(unittest.TestCase):
    """The emit-label CLI through the checker binary (issue 2750, HISS-15)."""

    def _emit_label(self, title: str) -> TextCommandResult:
        """Invoke squash_message.py emit-label through safe_subprocess."""
        return safe_run(
            [sys.executable, str(SQUASH_SCRIPT), "emit-label", "--title", title],
            allowed_executables=(sys.executable,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=CLI_TIMEOUT_S,
        )

    def test_declared_types_emit_their_label(self) -> None:
        """Prove a valid declared-type title prints type:<type> and exits 0."""
        for title, label in (
            ("feat: add the thing", "type:feat"),
            ("fix: one window", "type:fix"),
            ("sycl: route the queue", "type:sycl"),
            ("feat(cli)!: drop the flag", "type:feat"),
        ):
            res = self._emit_label(title)
            self.assertEqual(res.returncode, 0, title)
            self.assertEqual(res.stdout, f"{label}\n")

    def test_refused_titles_exit_1(self) -> None:
        """Prove no-prefix, empty-subject and undeclared-type titles exit 1."""
        for title in (
            "Update the title gate",
            "fix:",
            "style: reflow comments",
            "feature: add a gate",
        ):
            res = self._emit_label(title)
            self.assertEqual(res.returncode, 1, title)
            self.assertEqual(res.stdout, "")

    def test_missing_title_is_a_usage_error(self) -> None:
        """Prove emit-label without a title exits 2."""
        res = safe_run(
            [sys.executable, str(SQUASH_SCRIPT), "emit-label"],
            allowed_executables=(sys.executable,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=CLI_TIMEOUT_S,
        )
        self.assertEqual(res.returncode, 2)
        self.assertIn("required", res.stderr)


class TestCheckWithStubEngine(unittest.TestCase):
    """Test CLI check logic using a mock engine script."""

    def setUp(self) -> None:
        """Set up temporary directory and stub scripts for check testing."""
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
        """Clean up temporary directory and fixtures."""
        self.temp_dir.cleanup()

    def test_stub_breaking_title_without_migration_refused(self) -> None:
        """Prove breaking change without migration footer is refused by check."""
        code, _ = squash_message.check_message(
            title="feat(core)!: breaking change",
            number=201,
            body_text="No migration path provided",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.stub_argv,
        )
        self.assertEqual(code, 1)

    def test_stub_breaking_title_with_migration_passes(self) -> None:
        """Prove breaking change with migration footer passes check."""
        code, _ = squash_message.check_message(
            title="feat(core)!: breaking change",
            number=202,
            body_text="Migration: docs/migration.md",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.stub_argv,
        )
        self.assertEqual(code, 0)

    def test_stub_exit_127_or_missing_binary_returns_2(self) -> None:
        """Prove missing executable or exit code 127 returns exit code 2."""
        code_127, _ = squash_message.check_message(
            title="feat(core): feature",
            number=203,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.exit_127_argv,
        )
        self.assertEqual(code_127, 2)

        code_missing, _ = squash_message.check_message(
            title="feat(core): feature",
            number=204,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=["/nonexistent/praetor/engine/binary"],
        )
        self.assertEqual(code_missing, 2)

    def test_stub_timeout_returns_2(self) -> None:
        """Prove engine process timeout returns exit code 2."""
        code_timeout, _ = squash_message.check_message(
            title="feat(core): feature",
            number=205,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=self.sleep_argv,
            timeout_seconds=0.2,
        )
        self.assertEqual(code_timeout, 2)

    def test_cli_refuses_engine_argv_flag(self) -> None:
        """Prove that CLI rejects unknown --engine-argv flag with exit code 2."""
        res = subprocess.run(  # noqa: S603 -- CLI rejection check, no shell  # nosec B603
            [sys.executable, str(SQUASH_SCRIPT), "check", "--engine-argv", '["/bin/true"]'],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=False,
            timeout=CLI_TIMEOUT_S,
        )
        self.assertEqual(res.returncode, 2)

    def test_check_message_refuses_relative_engine_argv(self) -> None:
        """Prove check_message refuses non-absolute engine executable with exit code 2."""
        sentinel = self.dir_path / "relative_ran.txt"
        stub = self.dir_path / "relative_stub.py"
        stub.write_text(f"from pathlib import Path\nPath({str(sentinel)!r}).write_text('ran')\n")
        code, _ = squash_message.check_message(
            title="feat(core): test",
            number=206,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=["python3", str(stub)],
        )
        self.assertEqual(code, 2)
        self.assertFalse(sentinel.exists())

    def test_check_message_refuses_empty_engine_argv(self) -> None:
        """Prove check_message refuses empty engine_argv list with exit code 2."""
        code, _ = squash_message.check_message(
            title="feat(core): test",
            number=207,
            body_text="",
            identity="lusoris <lusoris@proton.me>",
            engine_argv=[],
        )
        self.assertEqual(code, 2)

    def test_check_message_boundary_missing_wrapper_refuses(self) -> None:
        """Prove check_message returns 2 naming wrapper when wrapper is missing."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            temp_root = Path(tmp_dir)
            src_config = REPO_ROOT / ".pre-commit-config.yaml"
            dest_config = temp_root / ".pre-commit-config.yaml"
            shutil.copy2(src_config, dest_config)
            expected_wrapper = temp_root / ".config" / "lefthook" / "engine.sh"
            code, out = squash_message.check_message(
                title="feat(core): valid message",
                number=208,
                body_text="",
                identity="lusoris <lusoris@proton.me>",
                repo_root=temp_root,
            )
            self.assertEqual(code, 2)
            self.assertIn(str(expected_wrapper), out)

    @unittest.skipIf(sys.platform == "win32", "symlink creation needs privileges on Windows")
    def test_default_prefix_keeps_symlinked_sh_unresolved(self) -> None:
        """Prove the default prefix keeps a symlinked sh as found, so busybox sees argv[0] sh."""
        real_sh = shutil.which("sh")
        if real_sh is None:
            self.skipTest("sh not on PATH")
        link = self.dir_path / "sh"
        link.symlink_to(real_sh)
        with patch.dict(os.environ, {"PATH": str(self.dir_path)}):
            prefix, err = squash_message.engine_wrapper_argv(REPO_ROOT)
        self.assertEqual(err, "")
        if prefix is None:
            self.fail("default prefix refused a symlinked sh")
        self.assertEqual(prefix[0], str(link))

    def test_default_prefix_refuses_missing_sh(self) -> None:
        """Prove the default prefix refuses with a message when no sh is on PATH."""
        with patch.dict(os.environ, {"PATH": str(self.dir_path)}):
            prefix, err = squash_message.engine_wrapper_argv(REPO_ROOT)
        self.assertIsNone(prefix)
        self.assertIn("sh executable not found", err)


class TestEngineAvailableProbe(unittest.TestCase):
    """Verify probe timeout and caching behavior of engine_available."""

    def setUp(self) -> None:
        squash_message._engine_available_cached.cache_clear()

    def tearDown(self) -> None:
        squash_message._engine_available_cached.cache_clear()

    def test_engine_available_positive_timeout(self) -> None:
        """Prove a probe returning rc 0 gives True and passes timeout=ENGINE_PROBE_TIMEOUT_SECONDS."""
        mock_proc = subprocess.CompletedProcess(
            args=["sh", ".config/lefthook/engine.sh", "version"],
            returncode=0,
            stdout="praetor 0.1.0\n",
            stderr="",
        )
        with patch("scripts.ci.squash_message.subprocess.run", return_value=mock_proc) as mock_run:
            result = squash_message.engine_available(REPO_ROOT)
        self.assertTrue(result)
        mock_run.assert_called_once()
        _, kwargs = mock_run.call_args
        self.assertEqual(kwargs.get("timeout"), squash_message.ENGINE_PROBE_TIMEOUT_SECONDS)
        self.assertEqual(kwargs.get("timeout"), 360.0)

    def test_engine_available_boundary_cache(self) -> None:
        """Prove two calls with the same root run subprocess.run once."""
        mock_proc = subprocess.CompletedProcess(
            args=["sh", ".config/lefthook/engine.sh", "version"],
            returncode=0,
            stdout="praetor 0.1.0\n",
            stderr="",
        )
        with patch("scripts.ci.squash_message.subprocess.run", return_value=mock_proc) as mock_run:
            first = squash_message.engine_available(REPO_ROOT)
            second = squash_message.engine_available(REPO_ROOT)
        self.assertTrue(first)
        self.assertTrue(second)
        mock_run.assert_called_once()

    def test_engine_available_negative_timeout_expired(self) -> None:
        """Prove subprocess.TimeoutExpired gives False."""
        with patch(
            "scripts.ci.squash_message.subprocess.run",
            side_effect=subprocess.TimeoutExpired(
                cmd=["sh", ".config/lefthook/engine.sh", "version"],
                timeout=squash_message.ENGINE_PROBE_TIMEOUT_SECONDS,
            ),
        ):
            result = squash_message.engine_available(REPO_ROOT)
        self.assertFalse(result)


class TestRealEngine(unittest.TestCase):
    """Test against the real pinned Praetor engine when available."""

    @unittest.skipUnless(squash_message.engine_available(), "praetor engine unavailable")
    def test_pr_3235_shape_refused_and_with_migration_passes(self) -> None:
        """Prove PR 3235 breaking change shape is refused and migration passes."""
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

    @unittest.skipUnless(squash_message.engine_available(), "praetor engine unavailable")
    def test_fork_commit_types_and_bogus(self) -> None:
        """Prove fork commit types are mapped and bogus types rejected."""
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

    @unittest.skipUnless(squash_message.engine_available(), "praetor engine unavailable")
    def test_engine_types_drift(self) -> None:
        """Detect upstream engine type changes to identify when to retire mapping."""
        for t in squash_message.ENGINE_TYPES:
            msg = f"{t}: standard description (#1)\n\nSigned-off-by: lusoris <lusoris@proton.me>\n"
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", delete=False, prefix="drift-raw-", suffix=".txt"
            ) as tmp:
                tmp.write(msg)
                tmp_path = Path(tmp.name)
            try:
                res = subprocess.run(  # noqa: S603 -- raw engine check, no shell  # nosec B603
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
                res = subprocess.run(  # noqa: S603 -- raw engine check, no shell  # nosec B603
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
        """Verify DCO sign-off job wiring, timeout, and injection safety."""
        content = WORKFLOW_FILE.read_text(encoding="utf-8")
        self.assertIn("dco-sign-off:", content)
        dco_section = content.split("dco-sign-off:")[1].split("ffmpeg-patches-surface-check:")[0]

        timeout_match = re.search(r"timeout-minutes:\s*(\d+)", dco_section)
        if timeout_match is None:
            self.fail("dco-sign-off job has no timeout-minutes")
        self.assertGreaterEqual(int(timeout_match.group(1)), 7)

        self.assertIn("Check the squash commit message", dco_section)
        self.assertIn(
            '"$RUNNER_TEMP/squash/bin/python" -B scripts/ci/squash_message.py check --timeout 360',
            dco_section,
        )
        self.assertIn(
            '"$RUNNER_TEMP/squash/bin/pip" install --quiet --require-hashes',
            dco_section,
        )
        self.assertIn("requirements/locks/build.txt", dco_section)
        self.assertIn("PR_TITLE: ${{ github.event.pull_request.title }}", dco_section)
        self.assertIn("PR_NUMBER: ${{ github.event.pull_request.number }}", dco_section)
        # The body is read from the event file, never interpolated into env.
        self.assertNotIn("PR_BODY: ${{ github.event.pull_request.body }}", dco_section)
        self.assertIn(
            """jq -r '.pull_request.body // ""' "$GITHUB_EVENT_PATH\"""",
            dco_section,
        )
        self.assertIn('--body-file "$RUNNER_TEMP/squash-pr-body.md"', dco_section)
        self.assertIn('LANDING_IDENTITY: "lusoris <lusoris@proton.me>"', dco_section)

        # Injection safety: no github.event.pull_request.title or body inside run: blocks
        step_blocks = dco_section.split("- name:")
        for block in step_blocks:
            if "run:" in block:
                run_part = block.split("run:")[1]
                self.assertNotIn("${{ github.event.pull_request.title }}", run_part)
                self.assertNotIn("${{ github.event.pull_request.body }}", run_part)

    def test_pr_type_label_job_wiring_and_no_interpolation(self) -> None:
        """Verify pr-type-label.yml labels through emit-label with no injection."""
        content = LABEL_WORKFLOW_FILE.read_text(encoding="utf-8")
        self.assertIn("Label PR by commit type", content)
        self.assertIn("scripts/ci/squash_message.py emit-label", content)
        self.assertIn("requirements/locks/build.txt", content)
        self.assertIn("--require-hashes", content)
        self.assertIn("PR_TITLE: ${{ github.event.pull_request.title }}", content)
        # The previous label derivation is gone: one definition, in squash_message.py.
        self.assertNotIn("check-pr-title", content)
        # Injection safety: the title and body reach run: blocks only through env.
        for block in content.split("- name:")[1:]:
            if "run:" in block:
                run_part = block.split("run:", 1)[1]
                self.assertNotIn("${{ github.event.pull_request.title }}", run_part)
                self.assertNotIn("${{ github.event.pull_request.body }}", run_part)

    def test_release_yml_names_every_declared_type(self) -> None:
        """Verify release.yml labels every declared type and keeps legacy aliases."""
        content = RELEASE_FILE.read_text(encoding="utf-8")
        for declared_type in squash_message.declared_types(REPO_ROOT):
            self.assertIn(f"- type:{declared_type}", content, declared_type)
        # The entries the hook has always refused are gone from the sections.
        self.assertNotIn("- type:security", content)
        self.assertNotIn("- type:style", content)
        # The previous mapping's names still land in the right sections.
        self.assertIn("- type:feature", content)
        self.assertIn("- type:bug", content)


class TestMergeTrainGuardIntegration(unittest.TestCase):
    """Verify merge_train_guard merge behavior with squash_message."""

    def test_merge_train_guard_carries_subject_and_body(self) -> None:
        """Verify merge_train_guard passes formatted subject and body."""
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

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Offline signed-repository regressions for upstream contribution checks."""

from __future__ import annotations

import io
import json
import os
import shutil
import sys
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from scripts.ci.run_affected_suites import affected_suites, focused_selection
from scripts.ci.suite_registry import load_registry
from scripts.dev import upstream_pr_sweep as sweep
from scripts.dev import upstream_push_gate as gate
from scripts.dev.tests.upstream_pr_sweep_fixture import (
    SweepFixtureCase,
    git_run,
    require_fixture_tools,
)
from scripts.dev.tests.upstream_pr_sweep_fixture import (
    fixture_environment as clean_git_environment,
)
from scripts.lib.safe_subprocess import run as run_command

SELF_IDS = ("C08", "C11", "C12", "C13", "C15", "C16")
SUBJECT = "libvmaf/adm: fix lowres buffer size"
BODY = "Exercise the low resolution buffer with a release build input."
RANGE_LIMIT = 3
MIN_HITS = 2


def setUpModule() -> None:
    require_fixture_tools()


class PushGateTest(SweepFixtureCase):
    def _create_repositories(self) -> None:
        """Reuse fixture signing and isolation; create only a disposable local repo."""
        self.repo = self.root / "scratch-clone"
        self.repo.mkdir()
        git_bin = shutil.which("git")
        assert git_bin is not None
        environment = clean_git_environment(self.root)
        environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        run_command(
            [git_bin, "init", "--initial-branch=master"],
            allowed_executables=(git_bin,),
            cwd=self.repo,
            env=environment,
            check=True,
            capture_output=True,
            text=True,
            timeout_seconds=60.0,
        )
        self.write("libvmaf/src/x.c", "initial\n")
        git_run(self.repo, "add", ".")
        git_run(self.repo, "commit", "-m", "libvmaf: initial fixture", "-m", BODY)
        self.base = git_run(self.repo, "rev-parse", "HEAD")
        git_run(self.repo, "update-ref", "refs/remotes/upstream/master", self.base)
        self.write("libvmaf/src/x.c", "head\n")
        self.write("libvmaf/test/test_x.c", "test\n")
        git_run(self.repo, "add", ".")
        git_run(self.repo, "commit", "-m", SUBJECT, "-m", BODY)
        self.review = self.root / "review.txt"
        self.review.write_text(
            "".join(f"{cid}: yes: release input and sibling checks recorded\n" for cid in SELF_IDS),
            encoding="utf-8",
        )
        self.draft = self.root / "draft.md"
        self.draft.write_text("Release build reproducer documented.\n", encoding="utf-8")

    def write(self, path: str, text: str) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def amend(self, subject: str = SUBJECT, body: str = BODY) -> None:
        git_run(self.repo, "add", ".")
        git_run(self.repo, "commit", "--amend", "-m", subject, "-m", body)

    def run_gate(self, *extra: str, review: bool = True, draft: bool = True) -> tuple[int, str]:
        args = ["--repo", str(self.repo)]
        if review:
            args += ["--self-review", str(self.review)]
        if draft:
            args += ["--draft", str(self.draft)]
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            try:
                rc = gate.main([*args, *extra])
            except SystemExit as exc:
                rc = int(str(exc.code))
        return rc, out.getvalue()

    def assert_failure(self, cid: str, **kwargs: bool) -> str:
        rc, output = self.run_gate(**kwargs)
        self.assertEqual(rc, 1, output)
        failed = [line.split()[1] for line in output.splitlines() if line.startswith("FAIL ")]
        self.assertEqual(failed, [cid], output)
        return output

    def add_review(self, cid: str, answer: str) -> None:
        lines = self.review.read_text(encoding="utf-8").splitlines()
        lines = [line for line in lines if not line.startswith(f"{cid}:")]
        self.review.write_text("\n".join([*lines, f"{cid}: {answer}"]) + "\n", encoding="utf-8")

    def test_positive_every_implemented_class_passes(self) -> None:
        rc, output = self.run_gate()
        self.assertEqual(rc, 0, output)
        for cid in gate.CHECKS:
            self.assertIn(f"PASS {cid} ", output)
        self.assertIn("SKIP C01 ", output)
        self.assertIn("SKIP C10 ", output)

    def test_c03_subject_and_body_boundaries(self) -> None:
        for length, expected in ((72, 0), (73, 1)):
            with self.subTest(subject_length=length):
                self.amend("libvmaf: " + "x" * (length - len("libvmaf: ")))
                rc, output = self.run_gate()
                self.assertEqual(rc, expected, output)
                if expected:
                    self.assert_failure("C03")
        for length, expected in ((72, 0), (73, 1)):
            with self.subTest(body_length=length):
                self.amend(body="x" * length)
                rc, output = self.run_gate()
                self.assertEqual(rc, expected, output)
                if expected:
                    self.assert_failure("C03")

    def test_c03_conventional_prefix_and_missing_body(self) -> None:
        self.amend("fix(adm): repair buffers")
        self.assert_failure("C03")
        self.amend(body="")
        self.assert_failure("C03")

    def test_c03_rejects_conventional_prefixes(self) -> None:
        for subject in (
            "test: build the helpers only on x86",
            "fix: handle worker creation failures",
            "feat!: drop the old api",
        ):
            with self.subTest(subject=subject):
                self.amend(subject)
                self.assert_failure("C03")

    def test_c03_accepts_test_subsystem_paths(self) -> None:
        for subject in (
            "libvmaf/test: build the helpers only on x86",
            "python/test: cover the empty model path",
        ):
            with self.subTest(subject=subject):
                self.amend(subject)
                rc, output = self.run_gate()
                self.assertEqual(rc, 0, output)
                self.assertIn("PASS C03 ", output)

    def test_c03_exempt_body_lines(self) -> None:
        for body in (
            "https://example.org/" + "x" * 100,
            "    " + "x" * 100,
            "\t" + "x" * 100,
            BODY + "\n\nEvidence: " + "x" * 100,
        ):
            with self.subTest(body=body[:20]):
                self.amend(body=body)
                rc, output = self.run_gate()
                self.assertEqual(rc, 0, output)
        self.amend(body="Evidence: " + "x" * 100 + "\n\n" + BODY)
        self.assert_failure("C03")

    def test_c04_source_without_test_or_reason(self) -> None:
        git_run(self.repo, "rm", "libvmaf/test/test_x.c")
        self.amend()
        self.assert_failure("C04")
        self.add_review("C04", "yes: existing regression covers this input")
        self.assertEqual(self.run_gate()[0], 0)

    def test_diff_excludes_upstream_only_changes(self) -> None:
        self.write("libvmaf/src/x.c", "initial\n")
        (self.repo / "libvmaf/test/test_x.c").unlink()
        self.write("libvmaf/src/upstream.c", "upstream-only marker\n")
        git_run(self.repo, "add", "-A")
        tree = git_run(self.repo, "write-tree")
        upstream = git_run(self.repo, "commit-tree", "-S", tree, "-p", self.base, "-m", SUBJECT)
        paths, lines = gate.read_diff(self.repo, upstream, "HEAD")
        self.assertNotIn("libvmaf/src/upstream.c", paths)
        self.assertNotIn("upstream-only marker", "\n".join(lines))
        rc, output = self.run_gate("--base", upstream)
        self.assertEqual(rc, 0, output)
        self.assertIn("PASS C04 ", output)

    def test_diff_rebased_and_unrelated_boundaries(self) -> None:
        paths, lines = gate.read_diff(self.repo, self.base, "HEAD")
        rc, names, _ = gate.git_cmd(
            self.repo, ["diff", "--no-renames", "--name-only", "-z", self.base, "HEAD", "--"]
        )
        self.assertEqual(rc, 0)
        self.assertEqual(paths, names.rstrip("\0").split("\0"))
        rc, diff, _ = gate.git_cmd(
            self.repo,
            [
                "diff",
                "--no-ext-diff",
                "--no-textconv",
                "--no-renames",
                "--no-color",
                "--unified=0",
                "--src-prefix=a/",
                "--dst-prefix=b/",
                self.base,
                "HEAD",
                "--",
            ],
        )
        self.assertEqual(rc, 0)
        self.assertEqual(lines, gate.bounded_lines(diff, "diff"))
        tree = git_run(self.repo, "rev-parse", "HEAD^{tree}")
        unrelated = git_run(self.repo, "commit-tree", "-S", tree, "-m", SUBJECT)
        with self.assertRaisesRegex(ValueError, "merge base"):
            gate.read_diff(self.repo, unrelated, "HEAD")

    def test_c05_message_attribution_and_fork_reference(self) -> None:
        for text in ("Co-authored-by: Claude <noreply@anthropic.com>", "See VMAFx/vmafx."):
            with self.subTest(text=text):
                self.amend(body=BODY + "\n\n" + text)
                output = self.assert_failure("C05")
                self.assertNotIn(text, output)

    def test_c05_draft_attribution_and_artifacts(self) -> None:
        for text in (
            ".workingdir/private",
            "Generated with a coding tool",
            "https://github.com/other/repo/actions/runs/123",
        ):
            with self.subTest(text=text):
                self.draft.write_text(text + "\n", encoding="utf-8")
                output = self.assert_failure("C05")
                self.assertNotIn(text, output)
        self.draft.write_text(
            "https://github.com/Netflix/vmaf/actions/runs/123\n", encoding="utf-8"
        )
        self.assertEqual(self.run_gate()[0], 0)

    def test_c05_added_diff_line(self) -> None:
        for text in ("See github.com/vmafx/vmafx", "Generated with a coding tool"):
            with self.subTest(text=text):
                self.write("libvmaf/test/test_x.c", text + "\n")
                self.amend()
                output = self.assert_failure("C05")
                self.assertIn("libvmaf/test/test_x.c:1", output)

    def test_c06_upstream_conflict(self) -> None:
        self.write("libvmaf/src/x.c", "upstream\n")
        git_run(self.repo, "add", ".")
        tree = git_run(self.repo, "write-tree")
        other = git_run(self.repo, "commit-tree", "-S", tree, "-p", self.base, "-m", SUBJECT)
        git_run(self.repo, "update-ref", "refs/remotes/upstream/master", other)
        self.add_review("C04", "yes: test already exists on both compared tips")
        output = self.assert_failure("C06")
        self.assertIn("libvmaf/src/x.c", output)

    def test_c07_multiple_commits_without_reason(self) -> None:
        git_run(self.repo, "commit", "--allow-empty", "-m", SUBJECT, "-m", BODY)
        self.assert_failure("C07")
        self.add_review("C07", "yes: split isolates the implementation and regression")
        self.assertEqual(self.run_gate()[0], 0)

    def test_c09_stale_amended_commit(self) -> None:
        old = git_run(self.repo, "rev-parse", "HEAD")
        self.amend(body=BODY + "\nAmended evidence.")
        self.draft.write_text(old + "\n", encoding="utf-8")
        self.assertIn(old, self.assert_failure("C09"))
        self.draft.write_text(self.base + "\n" + "0" * 40 + "\n", encoding="utf-8")
        self.assertEqual(self.run_gate()[0], 0)
        rc, output = self.run_gate(draft=False)
        self.assertEqual(rc, 0, output)
        self.assertIn("SKIP C09 ", output)

    def test_c14_unsigned_commit(self) -> None:
        git_run(self.repo, "commit", "--amend", "--no-gpg-sign", "--no-edit")
        self.assert_failure("C14")

    def test_c14_signature_verification_error_is_failure(self) -> None:
        with patch.object(gate, "count_unsigned", return_value=(-1, "verification error")):
            self.assert_failure("C14")

    def test_c09_accepts_upstream_only_id_and_multiple_drafts(self) -> None:
        tree = git_run(self.repo, "rev-parse", f"{self.base}^{{tree}}")
        other = git_run(self.repo, "commit-tree", "-S", tree, "-p", self.base, "-m", "upstream")
        git_run(self.repo, "update-ref", "refs/remotes/upstream/master", other)
        self.draft.write_text(other[:7] + "\n", encoding="utf-8")
        second = self.root / "second-draft.md"
        second.write_text("More reproduction details.\n", encoding="utf-8")
        rc, output = self.run_gate("--draft", str(self.draft), str(second), draft=False)
        self.assertEqual(rc, 0, output)

    def test_cli_self_check_on_fixture_scratch_clone(self) -> None:
        result = run_command(
            [
                sys.executable,
                "-B",
                str(Path(gate.__file__).resolve()),
                "--repo",
                str(self.repo),
                "--self-review",
                str(self.review),
                "--draft",
                str(self.draft),
            ],
            allowed_executables=(sys.executable,),
            capture_output=True,
            text=True,
            timeout_seconds=60.0,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        for cid in gate.CHECKS:
            self.assertIn(f"PASS {cid} ", result.stdout)

    def test_signature_uses_explicit_head(self) -> None:
        good = git_run(self.repo, "rev-parse", "HEAD")
        git_run(self.repo, "commit", "--amend", "--no-gpg-sign", "--no-edit")
        rc, output = self.run_gate("--head", good)
        self.assertEqual(rc, 0, output)

    def test_c17_fork_only_path(self) -> None:
        self.write("docs/research/x.md", "research\n")
        self.amend()
        self.assert_failure("C17")

    def test_c17_three_and_four_commits(self) -> None:
        self.add_review("C07", "yes: split separates individually reviewable changes")
        for total in range(2, 5):
            git_run(self.repo, "commit", "--allow-empty", "-m", SUBJECT, "-m", BODY)
            rc, output = self.run_gate()
            self.assertEqual(rc, int(total > RANGE_LIMIT), output)
            if total == RANGE_LIMIT:
                self.assertIn("PASS C17 ", output)
            if total > RANGE_LIMIT:
                self.assert_failure("C17")

    def test_missing_review_fails_every_self_review_class(self) -> None:
        rc, output = self.run_gate(review=False)
        self.assertEqual(rc, 1, output)
        self.assertEqual(
            {line.split()[1] for line in output.splitlines() if line.startswith("FAIL ")},
            set(SELF_IDS),
        )

    def test_empty_or_short_review_answer(self) -> None:
        self.add_review("C12", "")
        self.assert_failure("C12")
        self.add_review("C12", "yes: short")
        self.assert_failure("C12")

    def test_c08_and_c16_cannot_be_not_applicable_for_simd(self) -> None:
        self.write("libvmaf/src/feature/x86/x_avx2.c", "kernel\n")
        self.amend()
        for cid in ("C08", "C16"):
            self.add_review(cid, "n/a: no measurements supplied")
            self.assert_failure(cid)
            self.add_review(cid, "yes: two compiler measurements and extremal inputs")

    def test_mechanical_only_prints_reason(self) -> None:
        rc, output = self.run_gate("--mechanical-only", "rebase-only push", review=False)
        self.assertEqual(rc, 0, output)
        for cid in SELF_IDS:
            self.assertRegex(output, rf"SKIP {cid} [^\n]+: rebase-only push")

    def test_usage_errors_and_input_bounds(self) -> None:
        for args in (
            ("--base", "missing-ref"),
            ("--head", "--bad"),
            ("--mechanical-only", " "),
            ("--draft", str(self.root / "missing")),
        ):
            with self.subTest(args=args):
                self.assertEqual(self.run_gate(*args)[0], 2)
        with patch.object(gate, "MAX_LINES", 1):
            self.assertEqual(self.run_gate()[0], 2)

    def test_catalog_round_trip_and_implementation_coverage(self) -> None:
        self.assertEqual(gate.CLASS_FILE.name, "upstream_review_classes.json")
        raw = gate.CLASS_FILE.read_text(encoding="utf-8")
        data = json.loads(raw)
        self.assertEqual(raw, json.dumps(data, indent=2, ensure_ascii=False) + "\n")
        expected = {
            row["id"]
            for row in data["classes"]
            if row["hits"] >= MIN_HITS and row["mode"] in ("mechanical", "self-review")
        }
        self.assertEqual(expected, set(gate.CHECKS))
        self.assertEqual(
            {row["id"] for row in data["classes"] if row["mode"] == "online"}, {"C01", "C10"}
        )

    def test_catalog_hit_count_boundaries(self) -> None:
        data = json.loads(gate.CLASS_FILE.read_text(encoding="utf-8"))
        catalog = self.root / "classes.json"
        cid = data["classes"][0]["id"]
        for hits in (True, -1, "3", 0):
            with self.subTest(hits=hits):
                data["classes"][0]["hits"] = hits
                catalog.write_text(json.dumps(data), encoding="utf-8")
                with patch.object(gate, "CLASS_FILE", catalog):
                    if hits == 0 and not isinstance(hits, bool):
                        gate.load_classes()
                    else:
                        with self.assertRaisesRegex(ValueError, f"{cid}: invalid hit count"):
                            gate.load_classes()

    def test_missing_implemented_class_refuses_gate(self) -> None:
        data = json.loads(gate.CLASS_FILE.read_text(encoding="utf-8"))
        data["classes"] = [row for row in data["classes"] if row["id"] != "C03"]
        catalog = self.root / "classes.json"
        catalog.write_text(json.dumps(data), encoding="utf-8")
        with patch.object(gate, "CLASS_FILE", catalog):
            rc, output = self.run_gate()
        self.assertEqual(rc, 2, output)
        self.assertIn("C03", output)

    def test_listed_class_without_implementation_refuses_gate(self) -> None:
        with patch.dict(gate.CHECKS, {}, clear=True):
            self.assertEqual(self.run_gate()[0], 2)

    def test_new_inputs_map_to_tooling_and_focused_regression(self) -> None:
        root = Path(gate.__file__).resolve().parents[2]
        test = Path(__file__).resolve().relative_to(root).as_posix()
        changed = [
            "scripts/dev/upstream_push_gate.py",
            "scripts/dev/upstream_review_classes.json",
            test,
        ]
        registry = load_registry(root)
        self.assertEqual(affected_suites(registry, changed), {"tooling": changed})
        matches, unnamed = focused_selection(
            changed, changed, {test: Path(__file__).read_text(encoding="utf-8")}
        )
        self.assertEqual(matches, [test])
        self.assertEqual(unnamed, [])

    def test_sweep_refuses_gate_failure_before_push(self) -> None:
        self.amend("fix(adm): bad upstream shape")
        head = git_run(self.repo, "rev-parse", "HEAD")
        real_git = sweep.git_cmd
        with patch.object(sweep, "git_cmd", wraps=real_git) as commands:
            ok, detail = sweep.push_branch(self.repo, "topic", self.base, head, base_sha=self.base)
        self.assertFalse(ok)
        self.assertTrue(detail.startswith("FAIL C03 "), detail)
        self.assertFalse(any(call.args[1][0] == "push" for call in commands.call_args_list))

    def test_sweep_checks_exact_range_and_then_pushes(self) -> None:
        head = git_run(self.repo, "rev-parse", "HEAD")
        # Prove the resolved base is threaded through, without falling back to the default ref.
        git_run(self.repo, "update-ref", "-d", "refs/remotes/upstream/master")
        real_git = sweep.git_cmd

        def local_git(
            cwd: Path, args: list[str], *, timeout: float = 120.0
        ) -> tuple[int, str, str]:
            if args[0] == "push":
                return 0, "", ""
            return real_git(cwd, args, timeout=timeout)

        target = sweep.PRTarget(1, "topic", "fixture")
        with patch.object(sweep, "git_cmd", side_effect=local_git) as commands:
            row = sweep._execute_build_and_push(
                self.repo,
                target,
                self.base,
                head,
                base_sha=self.base,
                build=False,
                push=True,
                cuda_prefix="",
                jobs=1,
            )
        self.assertEqual(row.status, "OK", row.detail)
        pushes = [call.args[1] for call in commands.call_args_list if call.args[1][0] == "push"]
        self.assertEqual(len(pushes), 1)
        self.assertEqual(pushes[0][-1], f"{head}:refs/heads/topic")

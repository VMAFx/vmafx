#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bounded-batching, partition, and batch-report validation tests for check_bandit_new."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.bandit_new_test_support import BASE, UNSAFE, BanditNewTestCase


class CheckBanditNew(BanditNewTestCase):
    def test_config_change_batches_thirty_tracked_files(self) -> None:
        """A rule-enabling config change over 30 tracked files reports only the 2 hits in <=2 runs."""
        self.git("rm", "-q", "harness.py")
        (self.root / "pkg").mkdir()
        flagged = {4, 21}
        for index in range(30):
            body = "assert True\n" if index in flagged else BASE
            (self.root / f"pkg/mod_{index:02d}.py").write_text(body, encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        with self.count_bandit_runs() as calls:
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual(status, 1, err)
        self.assertEqual(
            out.splitlines(),
            [
                "pkg/mod_04.py:1: B101: assert True",
                "pkg/mod_21.py:1: B101: assert True",
            ],
        )
        self.assertLessEqual(len(calls), 2)
        self.assertEqual({cwd.name for cwd, _ in calls}, {"current", "base"})
        self.git("add", "bandit.yml")
        self.git("commit", "-q", "-m", "enable b101")
        push_status, push_out, push_err = self.outgoing_entry(to_ref="change")
        self.assertEqual(push_status, 1, push_err)
        self.assertEqual(push_out.splitlines(), out.splitlines())

    def test_config_change_without_config_selected_skips_unchanged_file(self) -> None:
        """A config change with only an unrelated unchanged file selected starts zero scans."""
        (self.root / "harness.py").write_text("assert True\n", encoding="utf-8", newline="\n")
        (self.root / "unrelated.py").write_text(BASE, encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        with self.count_bandit_runs() as calls:
            status, out, err = self.invoke("unrelated.py")
        self.assertEqual((status, out, err), (0, "", ""))
        self.assertEqual(calls, [])

    def test_config_change_disjoint_partitions_scan_tree_once(self) -> None:
        """Disjoint partitions with bandit.yml in one partition run one full scan total."""
        self.git("rm", "-q", "harness.py")
        (self.root / "pkg").mkdir()
        flagged = {2, 9}
        for index in range(12):
            body = "assert True\n" if index in flagged else BASE
            (self.root / f"pkg/mod_{index:02d}.py").write_text(body, encoding="utf-8", newline="\n")
        self.commit_base()
        (self.root / "bandit.yml").write_text("tests: [B101]\n", encoding="utf-8", newline="\n")
        with self.count_bandit_runs() as single_calls:
            single_status, _, single_err = self.invoke("bandit.yml")
        self.assertEqual(single_status, 1, single_err)
        self.assertLessEqual(len(single_calls), 2)
        partitions = (
            tuple(f"pkg/mod_{index:02d}.py" for index in range(4)),
            ("bandit.yml", *(f"pkg/mod_{index:02d}.py" for index in range(4, 8))),
            tuple(f"pkg/mod_{index:02d}.py" for index in range(8, 12)),
        )
        statuses: list[int] = []
        printed: list[str] = []
        with self.count_bandit_runs() as partition_calls:
            for part in partitions:
                status, out, err = self.invoke(*part)
                self.assertTrue(status == 0 or "new Bandit finding" in err, err)
                statuses.append(status)
                printed.extend(out.splitlines())
        self.assertEqual(statuses, [0, 1, 0])
        self.assertEqual(len(partition_calls), len(single_calls))
        self.assertLessEqual(len(partition_calls), 2)
        self.assertEqual(
            printed,
            [
                "pkg/mod_02.py:1: B101: assert True",
                "pkg/mod_09.py:1: B101: assert True",
            ],
        )

    def test_config_change_with_changed_file_selected_prints_once(self) -> None:
        """Selecting bandit.yml alongside a changed file reports that file's finding once."""
        (self.root / "bandit.yml").write_text(
            "tests: [B101, B603]\n", encoding="utf-8", newline="\n"
        )
        (self.root / "harness.py").write_text(
            "# changed\nassert True\n", encoding="utf-8", newline="\n"
        )
        for config_arg in ("bandit.yml", "./bandit.yml", str(self.root / "bandit.yml")):
            with self.subTest(config_arg=config_arg), self.count_bandit_runs() as calls:
                status, out, err = self.invoke(config_arg, "harness.py")
                self.assertEqual(status, 1, err)
                self.assertEqual(out.splitlines(), ["harness.py:2: B101: assert True"])
                self.assertLessEqual(len(calls), 2)

    def test_selected_path_outside_repository_is_refused(self) -> None:
        """A parent-relative or outside absolute path fails as a scan error with no Bandit run."""
        outside = self.root.parent / "outside.py"
        for arg, pattern in (
            ("../outside.py", "escapes repository"),
            (str(outside), "outside repository"),
        ):
            with self.subTest(arg=arg), self.count_bandit_runs() as calls:
                status, out, err = self.invoke(arg)
                self.assertEqual((status, out), (2, ""))
                self.assertIn(pattern, err)
                self.assertEqual(calls, [])

    def test_unchanged_config_and_identical_files_start_zero_processes(self) -> None:
        """Unchanged configuration and base-identical files invoke Bandit zero times."""
        (self.root / "pkg").mkdir()
        for name in ("pkg/a.py", "pkg/b.py"):
            (self.root / name).write_text(BASE, encoding="utf-8", newline="\n")
        self.commit_base()
        with self.count_bandit_runs() as calls:
            status, out, err = self.invoke("harness.py", "pkg/a.py", "pkg/b.py")
        self.assertEqual((status, out, err), (0, "", ""))
        self.assertEqual(calls, [])

    def test_batch_file_count_boundary(self) -> None:
        """MAX_BATCH_FILES runs one process per side; MAX_BATCH_FILES + 1 runs two per side."""
        self.git("rm", "-q", "harness.py")
        (self.root / "batch").mkdir()
        base_cfg = (self.root / "bandit.yml").read_bytes()
        for index in range(check.MAX_BATCH_FILES):
            (self.root / f"batch/f_{index:03d}.py").write_text(
                "value = 1\n", encoding="utf-8", newline="\n"
            )
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(base_cfg + b"# boundary 200\n")
        with self.count_bandit_runs() as at_limit:
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual((status, out), (0, ""), err)
        self.assertEqual([cwd.name for cwd, _ in at_limit], ["current", "base"])

        (self.root / "bandit.yml").write_bytes(base_cfg)
        (self.root / f"batch/f_{check.MAX_BATCH_FILES:03d}.py").write_text(
            "value = 1\n", encoding="utf-8", newline="\n"
        )
        self.commit_base()
        (self.root / "bandit.yml").write_bytes(base_cfg + b"# boundary 201\n")
        with self.count_bandit_runs() as over_limit:
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual((status, out), (0, ""), err)
        self.assertEqual(
            [cwd.name for cwd, _ in over_limit], ["current", "current", "base", "base"]
        )

    def test_long_paths_split_at_arg_char_limit_before_file_limit(self) -> None:
        """Long relative paths split at MAX_BATCH_ARG_CHARS before reaching MAX_BATCH_FILES."""
        self.git("rm", "-q", "harness.py")
        prefix = self.root / ("d" * 60) / ("e" * 60) / ("f" * 60)
        prefix.mkdir(parents=True)
        for index in range(130):
            (prefix / f"m_{index:03d}.py").write_text("value = 1\n", encoding="utf-8", newline="\n")
        self.commit_base()
        with (self.root / "bandit.yml").open("a", encoding="utf-8", newline="\n") as config:
            config.write("# arg char boundary\n")
        with self.count_bandit_runs() as calls:
            status, out, err = self.invoke("bandit.yml")
        self.assertEqual((status, out), (0, ""), err)
        self.assertEqual([cwd.name for cwd, _ in calls], ["current", "current", "base", "base"])
        for _, argv in calls:
            targets = argv[argv.index("--") + 1 :]
            self.assertLess(len(targets), check.MAX_BATCH_FILES)
            self.assertLessEqual(len(" ".join(targets)), check.MAX_BATCH_ARG_CHARS)

    def test_extensionless_shebang_inside_batch_is_scanned(self) -> None:
        """An extensionless Python shebang script inside a multi-file batch still reports B603."""
        (self.root / "pkg").mkdir()
        (self.root / "bin").mkdir()
        (self.root / "pkg/a.py").write_text(BASE, encoding="utf-8", newline="\n")
        (self.root / "pkg/b.py").write_text(BASE, encoding="utf-8", newline="\n")
        (self.root / "bin/py_tool").write_text(
            "#!/usr/bin/env python3\n" + BASE + UNSAFE, encoding="utf-8", newline="\n"
        )
        (self.root / "bin/sh_tool").write_text(
            "#!/bin/sh\n" + BASE + UNSAFE, encoding="utf-8", newline="\n"
        )
        with self.count_bandit_runs() as calls:
            status, out, err = self.invoke("pkg/a.py", "pkg/b.py", "bin/py_tool", "bin/sh_tool")
        self.assertEqual(status, 1, err)
        self.assertEqual(
            out.splitlines(),
            [
                "bin/py_tool:5: B404: import subprocess",
                f"bin/py_tool:7: B603: {UNSAFE.splitlines()[-1]}",
            ],
        )
        self.assertEqual(len(calls), 1)

    def test_missing_staged_file_in_metrics_is_error(self) -> None:
        """A Bandit JSON report omitting a staged file from metrics fails closed with exit 2."""
        for name in ("a.py", "b.py"):
            (self.root / name).write_text(BASE + "# changed\n", encoding="utf-8", newline="\n")
        payload = {"results": [], "errors": [], "metrics": {"_totals": {}, "./a.py": {"loc": 3}}}
        result = mock.Mock(returncode=0, stdout=json.dumps(payload))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.invoke("a.py", "b.py")
        self.assertEqual(status, 2)
        self.assertIn("b.py", err)
        self.assertIn("metrics", err)

    def test_batch_non_empty_errors_names_relative_path(self) -> None:
        """Non-empty Bandit JSON errors in a batch return 2 and name the failing relative path."""
        for name in ("a.py", "b.py"):
            (self.root / name).write_text(BASE + "# changed\n", encoding="utf-8", newline="\n")
        payload = {
            "results": [],
            "errors": [{"filename": "./b.py", "reason": "syntax error"}],
            "metrics": {"_totals": {}, "./a.py": {"loc": 3}, "./b.py": {"loc": 0}},
        }
        result = mock.Mock(returncode=0, stdout=json.dumps(payload))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.invoke("a.py", "b.py")
        self.assertEqual(status, 2)
        self.assertIn("scan errors", err)
        self.assertIn("b.py", err)

    def test_unknown_result_filename_is_error(self) -> None:
        """A Bandit JSON finding naming a file outside the batch returns 2."""
        (self.root / "a.py").write_text(BASE + "# changed\n", encoding="utf-8", newline="\n")
        payload = {
            "results": [{"filename": "./rogue.py", "line_number": 1, "test_id": "B101"}],
            "errors": [],
            "metrics": {"_totals": {}, "./a.py": {"loc": 3}},
        }
        result = mock.Mock(returncode=1, stdout=json.dumps(payload))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.invoke("a.py")
        self.assertEqual(status, 2)
        self.assertIn("rogue.py", err)

    def test_out_of_range_line_number_in_batch_is_error(self) -> None:
        """A Bandit JSON finding with a line number outside the staged file returns 2."""
        (self.root / "a.py").write_text("value = 1\n", encoding="utf-8", newline="\n")
        payload = {
            "results": [{"filename": "./a.py", "line_number": 9, "test_id": "B101"}],
            "errors": [],
            "metrics": {"_totals": {}, "./a.py": {"loc": 1}},
        }
        result = mock.Mock(returncode=1, stdout=json.dumps(payload))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.invoke("a.py")
        self.assertEqual(status, 2)
        self.assertIn("a.py", err)


if __name__ == "__main__":
    unittest.main()

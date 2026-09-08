#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise guarded baseline writes and real filesystem failure paths (ADR-1243)."""

from __future__ import annotations

import copy
import importlib.util
import json
import multiprocessing
import os
import sys
import tempfile
import unittest
from multiprocessing.connection import Connection
from pathlib import Path
from typing import Any
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "tidy_scoped_module", Path(__file__).resolve().parents[1] / "tidy-ratchet.py"
)
assert SPEC is not None and SPEC.loader is not None
ratchet = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ratchet
SPEC.loader.exec_module(ratchet)


def competing_writer(path: Path, scoped: bool, result: Connection) -> None:
    measured = ratchet.Measurement(
        lane="cpu", tus=1, sources=["core/src/a.c"], clang_tidy_version="22.1.8"
    )
    if scoped:
        code = ratchet.write_scoped_baseline(path, measured, measured.sources)
    else:
        code = ratchet.write_full_baseline(path, measured, path.read_bytes())
    result.send(code)
    result.close()


class ScopedWriter(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.source = self.root / "core/src/a.c"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("int value;\n")
        self.key = "core/src/a.c"
        self.baseline = self.root / "baseline.json"
        self.original: dict[str, Any] = {
            "schema": 1,
            "lane": "cpu",
            "tus": 292,
            "generator": "scripts/ci/tidy-ratchet.py",
            "clang_tidy_version": "22.1.8",
            "total_warnings": 14,
            "total_nolint_uncited": 6,
            "warnings": {self.key: 5, "core/src/b.c": 7, "core/src/shared.h": 2},
            "nolint_uncited": {self.key: 2, "core/src/b.c": 1, "core/src/shared.h": 3},
            "full_report_metadata": {"preserve": "exactly"},
        }
        self.baseline.write_text(json.dumps(self.original) + "\n")
        self.before = self.baseline.read_bytes()
        self.database = self.build / "compile_commands.json"
        self.database.write_text(
            json.dumps([{"directory": str(self.root), "file": str(self.source), "command": "cc"}])
        )

    def measurement(self, **changes: Any) -> Any:
        values: dict[str, Any] = {
            "lane": "cpu",
            "tus": 1,
            "sources": [self.key],
            "clang_tidy_version": "22.1.8",
        }
        values.update(changes)
        return ratchet.Measurement(**values)

    def arguments(self, *extra: str) -> list[str]:
        return [
            "--repo-root",
            str(self.root),
            "--build-dir",
            str(self.build),
            "--baseline",
            str(self.baseline),
            "--only",
            str(self.source),
            "--write",
            *extra,
        ]

    def test_zero_tightening_preserves_unselected_entries_and_full_metadata(self) -> None:
        result = ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key])
        self.assertEqual(result, 0)
        updated = json.loads(self.baseline.read_text())
        for metric in ("warnings", "nolint_uncited"):
            self.assertNotIn(self.key, updated[metric])
            for path in ("core/src/b.c", "core/src/shared.h"):
                self.assertEqual(updated[metric][path], self.original[metric][path])
        for key in ("tus", "generator", "clang_tidy_version", "full_report_metadata"):
            self.assertEqual(updated[key], self.original[key])
        self.assertEqual(updated["total_warnings"], 9)
        self.assertEqual(updated["total_nolint_uncited"], 4)
        provenance = updated["scoped_updates"][0]
        self.assertEqual(provenance["sources"], [self.key])
        self.assertEqual(provenance["changes"][self.key]["warnings"], [5, 0])
        self.assertEqual(len(provenance["previous_baseline_sha256"]), 64)
        once = self.baseline.read_bytes()
        self.assertEqual(
            ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key]), 0
        )
        self.assertEqual(self.baseline.read_bytes(), once)

    def test_observed_increases_never_write_even_for_unselected_headers(self) -> None:
        for changes in (
            {"warnings": {self.key: 6}},
            {"nolint_uncited": {self.key: 3}},
            {"warnings": {"core/src/shared.h": 3}},
            {"warnings": {"core/src/new.c": 1}},
        ):
            with self.subTest(changes=changes):
                self.assertEqual(
                    ratchet.write_scoped_baseline(
                        self.baseline, self.measurement(**changes), [self.key]
                    ),
                    2,
                )
                self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_empty_inexact_failed_or_incompatible_measurement_never_writes(self) -> None:
        for changes in (
            {"tus": 0, "sources": []},
            {"tus": 2},
            {"sources": ["core/src/b.c"]},
            {"sources": [self.key, self.key], "tus": 2},
            {"compile_failures": [self.key]},
            {"lane": "cuda"},
            {"clang_tidy_version": "23.0.0"},
            {"clang_tidy_version": ""},
            {"warnings": {self.key: -1}},
        ):
            with self.subTest(changes=changes):
                self.assertEqual(
                    ratchet.write_scoped_baseline(
                        self.baseline, self.measurement(**changes), [self.key]
                    ),
                    5,
                )
                self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_invalid_baseline_never_writes(self) -> None:
        bad_count = copy.deepcopy(self.original)
        bad_count["warnings"][self.key] = 1.5
        for text in ("not JSON\n", "[]\n", json.dumps(bad_count)):
            with self.subTest(text=text):
                self.baseline.write_text(text)
                self.assertEqual(
                    ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key]),
                    5,
                )
                self.assertEqual(self.baseline.read_text(), text)

    def test_report_aliases_are_rejected_before_measurement_or_output(self) -> None:
        symlink = self.root / "symlink.json"
        symlink.symlink_to(self.baseline)
        hardlink = self.root / "hardlink.json"
        os.link(self.baseline, hardlink)
        for report in (self.baseline, symlink, hardlink):
            with self.subTest(report=report), patch.object(ratchet, "measure") as measure:
                self.assertEqual(ratchet.main(self.arguments("--report", str(report))), 5)
                measure.assert_not_called()
                self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_baseline_symlink_preserves_link_and_atomically_updates_target(self) -> None:
        link = self.root / "baseline-link.json"
        link.symlink_to(self.baseline)
        self.assertEqual(ratchet.write_scoped_baseline(link, self.measurement(), [self.key]), 0)
        self.assertTrue(link.is_symlink())
        self.assertNotIn(self.key, json.loads(self.baseline.read_text())["warnings"])

    def test_atomic_replace_failure_preserves_original_and_removes_temporary(self) -> None:
        files_before = set(self.root.iterdir())
        with patch.object(ratchet.os, "replace", side_effect=OSError("injected replace failure")):
            self.assertEqual(
                ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key]), 5
            )
        self.assertEqual(self.baseline.read_bytes(), self.before)
        self.assertEqual(set(self.root.iterdir()), files_before)

    def test_cli_rejects_failed_measurement_without_mutating_baseline(self) -> None:
        failed = self.measurement(compile_failures=[self.key])
        with patch.object(ratchet, "measure", return_value=failed):
            self.assertEqual(ratchet.main(self.arguments()), 4)
        self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_report_write_failure_returns_clean_error_without_baseline_write(self) -> None:
        with patch.object(ratchet, "measure", return_value=self.measurement()):
            self.assertEqual(ratchet.main(self.arguments("--report", str(self.root))), 5)
        self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_missing_requested_tu_and_empty_database_do_not_start_the_tool(self) -> None:
        for wanted, entries in (([str(self.root / "missing.c")], None), ([], [])):
            if entries is not None:
                self.database.write_text(json.dumps(entries))
            with self.subTest(wanted=wanted), patch.object(ratchet, "run_one") as run:
                with self.assertRaises(ValueError):
                    ratchet.measure("cpu", self.build, self.root, "clang-tidy", [], 1, wanted)
                run.assert_not_called()

    def test_nonzero_tool_exit_without_diagnostics_is_not_clean(self) -> None:
        with (
            patch.object(ratchet, "clang_tidy_version", return_value="22.1.8"),
            patch.object(ratchet, "run_one", return_value=(str(self.source), "tool crashed", 1)),
        ):
            measured = ratchet.measure("cpu", self.build, self.root, "clang-tidy", [], 1)
        self.assertEqual(measured.compile_failures, [self.key])
        self.assertEqual(measured.sources, [self.key])

    def test_unreadable_source_or_lane_header_cannot_clear_suppression_debt(self) -> None:
        header = self.source.parent / "shared.h"
        header.write_text("// NOLINT\n")
        original_read = Path.read_text
        for unreadable in (self.source, header):

            def read_text(
                path: Path, *args: Any, unreadable: Path = unreadable, **kwargs: Any
            ) -> str:
                if path == unreadable:
                    raise PermissionError("injected unreadable NOLINT input")
                return original_read(path, *args, **kwargs)

            with (
                self.subTest(path=unreadable),
                patch.object(ratchet, "clang_tidy_version", return_value="22.1.8"),
                patch.object(ratchet, "run_one", return_value=(str(self.source), "", 0)),
                patch.object(Path, "read_text", read_text),
            ):
                self.assertEqual(ratchet.main(self.arguments()), 5)
            self.assertEqual(self.baseline.read_bytes(), self.before)

    def test_actual_second_full_and_scoped_writers_are_refused_while_locked(self) -> None:
        context = multiprocessing.get_context("fork")
        with ratchet.baseline_lock(self.baseline):
            for scoped in (True, False):
                with self.subTest(scoped=scoped):
                    receiver, sender = context.Pipe(duplex=False)
                    child = context.Process(
                        target=competing_writer, args=(self.baseline, scoped, sender)
                    )
                    child.start()
                    try:
                        self.assertTrue(receiver.poll(5), "competing writer must fail promptly")
                        self.assertEqual(receiver.recv(), 5)
                        child.join(5)
                        self.assertEqual(child.exitcode, 0)
                    finally:
                        if child.is_alive():
                            child.terminate()
                            child.join(5)
                        receiver.close()
                        sender.close()
                    self.assertEqual(self.baseline.read_bytes(), self.before)
        self.assertEqual(
            ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key]), 0
        )

    def test_external_baseline_change_during_atomic_write_is_preserved(self) -> None:
        concurrent = copy.deepcopy(self.original)
        concurrent["warnings"].pop("core/src/b.c")
        concurrent["total_warnings"] -= 7
        concurrent["another_writer"] = "preserve this metadata"
        replacement = json.dumps(concurrent).encode()
        original_fsync = os.fsync

        def race(descriptor: int) -> None:
            self.baseline.write_bytes(replacement)
            original_fsync(descriptor)

        with patch.object(ratchet.os, "fsync", side_effect=race):
            self.assertEqual(
                ratchet.write_scoped_baseline(self.baseline, self.measurement(), [self.key]), 5
            )
        self.assertEqual(self.baseline.read_bytes(), replacement)

    def test_baseline_changed_while_measuring_requires_a_fresh_run(self) -> None:
        replacement = self.before + b"\n"

        def measure(*args: Any, **kwargs: Any) -> Any:
            self.baseline.write_bytes(replacement)
            return self.measurement()

        with patch.object(ratchet, "measure", side_effect=measure):
            self.assertEqual(ratchet.main(self.arguments()), 5)
        self.assertEqual(self.baseline.read_bytes(), replacement)

    def test_promoted_checks_remain_measurable_but_other_failures_do_not(self) -> None:
        diagnostic = (
            f"{self.source}:1:2: error: deliberate warning "
            "[readability-magic-numbers,-warnings-as-errors]\n"
        )
        summary = "1 warning treated as error\n"
        for output, returncode, failed in (
            (diagnostic + summary, 1, False),
            (diagnostic + summary, 2, True),
            (diagnostic, 1, True),
            (diagnostic + summary + "error: cannot load compilation database", 1, True),
        ):
            with (
                self.subTest(returncode=returncode, output=output),
                patch.object(ratchet, "clang_tidy_version", return_value="22.1.8"),
                patch.object(
                    ratchet, "run_one", return_value=(str(self.source), output, returncode)
                ),
            ):
                measured = ratchet.measure("cpu", self.build, self.root, "clang-tidy", [], 1)
            self.assertEqual(bool(measured.compile_failures), failed)
            self.assertEqual(measured.warnings[self.key], 1)

    def test_report_round_trip_preserves_scope_and_failure_status(self) -> None:
        measured = self.measurement(compile_failures=[self.key])
        report = measured.to_json()
        self.assertEqual(report["measured_sources"], [self.key])
        self.assertEqual(report["compile_failures"], [self.key])
        decoded = ratchet.Measurement.from_json(report)
        self.assertEqual(decoded.sources, measured.sources)
        self.assertEqual(decoded.compile_failures, measured.compile_failures)

    def test_unparsed_diagnostics_and_compiler_errors_are_fail_closed(self) -> None:
        for line in (
            f"{self.source}:1:2: warning: unexpected format without check ID",
            f"{self.source}:1:2: error: missing include [clang-diagnostic-error]",
            "clang-tidy: error: unknown option",
            "error: cannot load compilation database",
        ):
            with self.subTest(line=line):
                _diagnostics, failed = ratchet.parse_diagnostics(line, self.root, self.root)
                self.assertTrue(failed)
        _diagnostics, failed = ratchet.parse_diagnostics(
            '  42 | log("error: input");\n4 warnings generated.', self.root, self.root
        )
        self.assertFalse(failed)


if __name__ == "__main__":
    unittest.main()

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Regression tests for the two supported BACKLOG.md schemas."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.lib.backlog_tracker import BacklogFormatError, BacklogItem, BacklogTracker

FIXTURE = Path(__file__).with_name("testdata") / "current-backlog.md"


class BacklogTrackerTests(unittest.TestCase):
    def test_current_checklist_schema_preserves_ids_status_and_continuations(self) -> None:
        tracker = BacklogTracker(FIXTURE)

        self.assertEqual(len(tracker.all()), 5)
        master = self._item(tracker, "T-RC1-MASTER-GREEN")
        self.assertEqual(master.title, "Master green and the queue drained (#1236) —")
        self.assertEqual(master.status, "OPEN")
        self.assertEqual(master.priority, None)
        self.assertEqual(master.pr_refs, [1528])
        self.assertIn("PR #1528 is pending", master.raw_row)

        self.assertEqual(self._item(tracker, "T-RC1-RELEASE-PIPELINE").status, "DONE")
        self.assertEqual(self._item(tracker, "T-RC2-BENCH-TUNE").status, "BLOCKED")
        self.assertEqual(self._item(tracker, "T-RC3-MODEL-RETRAIN").status, "BLOCKED")
        self.assertEqual(self._item(tracker, "T-POSTRC1-REUSE").status, "DEFERRED")

    def test_legacy_table_schema_remains_compatible(self) -> None:
        path = self._write_backlog(
            "| **T3-9** | open task |\n" "| ~~**T7-5**~~ **[DONE — PR #82]** | ~~closed task~~ |\n"
        )

        tracker = BacklogTracker(path)

        self.assertEqual(self._item(tracker, "T3-9").priority, 3)
        self.assertEqual(self._item(tracker, "T7-5").status, "DONE")
        self.assertEqual(self._item(tracker, "T7-5").pr_refs, [82])

    def test_unmarked_checklist_item_fails_closed(self) -> None:
        path = self._write_backlog("- [ ] A task without a stable machine ID.\n")

        with self.assertRaisesRegex(BacklogFormatError, "stable.*ID"):
            BacklogTracker(path).all()

    def test_duplicate_id_fails_closed(self) -> None:
        path = self._write_backlog(
            "- [ ] `T-DUPLICATE-ID` first\n" "- [ ] `T-DUPLICATE-ID` second\n"
        )

        with self.assertRaisesRegex(BacklogFormatError, "duplicate.*T-DUPLICATE-ID"):
            BacklogTracker(path).all()

    def test_checked_item_with_open_class_status_fails_closed(self) -> None:
        path = self._write_backlog("- [x] `T-CONFLICTING-STATE` **[BLOCKED]** task\n")

        with self.assertRaisesRegex(BacklogFormatError, "conflicts with status BLOCKED"):
            BacklogTracker(path).all()

    def test_unchecked_item_with_closed_status_fails_closed(self) -> None:
        for status in ("DONE", "CLOSED", "REMOVED"):
            with self.subTest(status=status):
                path = self._write_backlog(
                    f"- [ ] `T-CONFLICTING-{status}` **[{status}]** completed task\n"
                )

                with self.assertRaisesRegex(
                    BacklogFormatError, f"conflicts with closed status {status}"
                ):
                    BacklogTracker(path).all()

    def test_unknown_explicit_status_fails_closed(self) -> None:
        path = self._write_backlog("- [ ] `T-UNKNOWN-STATE` **[READY]** task\n")

        with self.assertRaisesRegex(BacklogFormatError, "unsupported status READY"):
            BacklogTracker(path).all()

    def test_existing_file_without_tracked_items_fails_closed(self) -> None:
        path = self._write_backlog("# Backlog\n\nNo tracked work.\n")

        with self.assertRaisesRegex(BacklogFormatError, "no tracked items"):
            BacklogTracker(path).all()

    def _write_backlog(self, content: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "BACKLOG.md"
        path.write_text(content, encoding="utf-8")
        return path

    def _item(self, tracker: BacklogTracker, item_id: str) -> BacklogItem:
        item = tracker.get(item_id)
        self.assertIsNotNone(item)
        assert item is not None
        return item


if __name__ == "__main__":
    unittest.main()

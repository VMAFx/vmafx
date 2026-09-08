# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Migration fixtures never touch the real train or execute GitHub requests."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import install_merge_train_guard as installer
import merge_train_guard as guard


class MigrationContract(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / "source"
        self.source.mkdir()
        installer.git(self.source, "init", "--initial-branch=master")
        installer.git(self.source, "config", "user.name", "Migration fixture")
        installer.git(self.source, "config", "user.email", "fixture@example.invalid")
        installer.git(self.source, "config", "core.hooksPath", "/dev/null")
        installer.git(self.source, "config", "commit.gpgsign", "false")
        gateway = self.source / installer.SOURCE_PATH
        gateway.parent.mkdir(parents=True)
        gateway.write_text("import json,sys\nprint(json.dumps(sys.argv[1:]))\n")
        installer.git(self.source, "add", ".")
        installer.git(self.source, "commit", "-m", "fixture gateway")
        self.state = self.directory / "state with spaces"
        self.state.mkdir()
        for name in installer.RUNTIME_FILES:
            (self.state / name).write_text(f"original {name}\n")
        (self.state / "hold.txt").write_text("1396 human hold\n1416 owner\n1421 stack\n")
        (self.state / "policy.json").write_text(
            json.dumps({"schema": 1, "protected_branches": ["existing"], "protected_worktrees": []})
        )
        self.before = {
            name: (self.state / name).read_bytes()
            for name in (*installer.RUNTIME_FILES, "hold.txt", "policy.json")
        }

    def plan(self) -> tuple[dict[str, object], bytes]:
        return installer.plan(
            self.source,
            self.source,
            self.state,
            ["active-root"],
            [str(self.directory / "agent-active")],
        )

    def test_preview_is_read_only_and_apply_preserves_exact_originals_holds_and_policy(
        self,
    ) -> None:
        value, committed = self.plan()
        self.assertFalse((self.state / "PAUSED").exists())
        self.assertEqual({path.name for path in self.state.iterdir()}, set(self.before))
        backup = installer.install(value, committed, installer.plan_hash(value))
        for name, data in self.before.items():
            self.assertEqual((backup / name).read_bytes(), data)
        self.assertEqual((self.state / "hold.txt").read_bytes(), self.before["hold.txt"])
        self.assertTrue((self.state / "PAUSED").exists())
        self.assertEqual(
            json.loads((self.state / "policy.json").read_text())["protected_branches"],
            ["active-root", "existing"],
        )
        self.assertTrue((backup / "receipt.json").exists())

    def test_installed_observers_default_read_only_and_gateway_tampering_fails(self) -> None:
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        for argv in (
            [str(self.state / "train.sh")],
            [sys.executable, str(self.state / "merge_train_operator.py")],
        ):
            output = guard.execute(argv, self.source)
            arguments = json.loads(output)
            self.assertEqual(arguments[0], "cycle")
            self.assertNotIn("--apply", arguments)
        with self.assertRaises(guard.Refused):
            guard.execute(
                [sys.executable, str(self.state / "merge_train_operator.py"), "--apply"],
                self.source,
            )
        with self.assertRaises(guard.Refused):
            guard.execute([str(self.state / "watchdog.sh"), "--apply"], self.source)
        gateway = self.state / "releases" / str(value["gateway_sha256"]) / "merge_train_guard.py"
        gateway.chmod(0o644)
        gateway.write_text("print('tampered')\n")
        with self.assertRaisesRegex(guard.Refused, "hash changed"):
            guard.execute([str(self.state / "train.sh")], self.source)

    def test_stale_preview_and_source_edits_refuse_before_replacing_runtime(self) -> None:
        value, committed = self.plan()
        (self.state / "hold.txt").write_text("1422 new hold\n")
        fresh, _ = self.plan()
        with self.assertRaisesRegex(ValueError, "plan changed"):
            installer.install(fresh, committed, installer.plan_hash(value))
        for name in installer.RUNTIME_FILES:
            self.assertEqual((self.state / name).read_bytes(), self.before[name])
        (self.source / installer.SOURCE_PATH).write_text("print('uncommitted')\n")
        with self.assertRaisesRegex(ValueError, "committed"):
            self.plan()

    def test_target_symlink_is_refused_and_never_followed(self) -> None:
        outside = self.directory / "evidence"
        outside.write_text("keep\n")
        target = self.state / "train.sh"
        target.unlink()
        target.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "non-regular"):
            self.plan()
        self.assertEqual(outside.read_text(), "keep\n")

    def test_failed_install_leaves_pause_backups_and_foreign_release_untouched(self) -> None:
        value, committed = self.plan()
        outside = self.directory / "foreign-release"
        outside.mkdir()
        (outside / "keep.txt").write_text("unknown evidence\n")
        (self.state / "releases").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink release"):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertTrue((self.state / "PAUSED").exists())
        self.assertEqual(list(outside.iterdir()), [outside / "keep.txt"])
        backup = next(self.state.glob("migration-*"))
        self.assertTrue((backup / "plan.json").exists())
        for name in installer.RUNTIME_FILES:
            self.assertEqual((backup / name).read_bytes(), self.before[name])
            self.assertEqual((self.state / name).read_bytes(), self.before[name])

    def test_cooperating_mutation_lock_blocks_install(self) -> None:
        value, committed = self.plan()
        train = guard.Train(self.source, self.state)
        with train.mutation_lock(), self.assertRaises(BlockingIOError):
            installer.install(value, committed, installer.plan_hash(value))
        self.assertFalse((self.state / "PAUSED").exists())

    def test_generated_shell_adapters_parse_and_rebase_is_explicit(self) -> None:
        value, committed = self.plan()
        installer.install(value, committed, installer.plan_hash(value))
        for name in ("train.sh", "watchdog.sh", "rebase-clean.sh"):
            guard.execute(["sh", "-n", str(self.state / name)], self.source)
        arguments = json.loads(
            guard.execute([str(self.state / "rebase-clean.sh"), "1422", "--apply"], self.source)
        )
        self.assertEqual(arguments[:3], ["rebase", "1422", "--apply"])


if __name__ == "__main__":
    unittest.main()

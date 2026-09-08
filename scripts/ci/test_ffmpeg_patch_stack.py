"""Exercise real Git replay, release selection and failure preservation."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "patch_stack", Path(__file__).with_name("ffmpeg_patch_stack.py")
)
assert SPEC and SPEC.loader
STACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STACK)


class ReleaseSelection(unittest.TestCase):
    def test_only_stable_tags_and_numeric_order(self) -> None:
        refs = "\n".join(
            f"{'a' * 40}\trefs/tags/{tag}"
            for tag in ("n9.0.1", "n9.9", "n10.0", "n11.0-dev", "n12.0-rc1", "snapshot", "master")
        )
        self.assertEqual(STACK.latest_release(refs), "n10.0")
        for tag in ("master", "n9.1-dev", "n9.1rc1", "n9.1-rc1", "1234567", "n9.1.0^{}"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                STACK.stable_version(tag)


class RealReplay(unittest.TestCase):
    def git(self, path: Path, *args: str) -> str:
        # Fixed Git executable and fixture-owned arguments; no shell.
        # Fixture setup needs isolation too: -C does not override hook GIT_*.
        environment = {
            key: value for key, value in os.environ.items() if not key.startswith("GIT_")
        }
        environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        return subprocess.check_output(  # noqa: S603
            [
                shutil.which("git") or "/usr/bin/git",
                "-c",
                "core.hooksPath=/dev/null",
                "-c",
                "commit.gpgsign=false",
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@localhost",
                "-C",
                str(path),
                *args,
            ],
            text=True,
            stderr=subprocess.DEVNULL,
            env=environment,
        )

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.upstream = self.root / "upstream"
        self.upstream.mkdir()
        self.git(self.upstream, "init", "-q")
        (self.upstream / "sample").write_text("before\n")
        self.git(self.upstream, "add", "sample")
        self.git(self.upstream, "commit", "-qm", "base")
        self.git(self.upstream, "tag", "n9.0.1")
        (self.upstream / "sample").write_text("patched\n")
        self.git(self.upstream, "commit", "-qam", "add integration")
        patch = self.git(self.upstream, "format-patch", "-1", "--stdout")
        self.repo = self.root / "repo"
        (self.repo / "ffmpeg-patches").mkdir(parents=True)
        self.patch = self.repo / "ffmpeg-patches/0001-integration.patch"
        self.patch.write_text(patch)
        (self.repo / "ffmpeg-patches/series.txt").write_text(
            "# Full stack\n0001-integration.patch\n"
        )
        (self.repo / "build-config.env").write_text(
            f'FFMPEG_REMOTE="{self.upstream}"\nFFMPEG_TAG="n9.0.1"\n'
        )
        for name in STACK.MIRRORS:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"ARG FFMPEG_TAG=n9.0.1\nARG FFMPEG_REMOTE={self.upstream}\n")
        self.output = self.root / "report"

    def contents(self) -> dict[str, bytes]:
        return {
            str(path.relative_to(self.repo)): path.read_bytes()
            for path in self.repo.rglob("*")
            if path.is_file()
        }

    def test_refresh_is_replayable_and_idempotent(self) -> None:
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.maintain(self.repo, self.output, False, False)
        result = STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(result["patch_count"], 1)
        before = self.contents()
        checked = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(checked["changed"], [])
        STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.contents(), before)

    def test_caller_global_config_cannot_change_canonical_patches(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        before = self.contents()
        hostile_home = self.root / "hostile-home"
        hostile_home.mkdir()
        (hostile_home / ".gitconfig").write_text(
            "[format]\nsubjectPrefix = WRONG\nthread = deep\nsignOff = true\n"
            "[core]\nautocrlf = true\n[commit]\ngpgSign = true\n"
        )
        with patch.dict(os.environ, {"HOME": str(hostile_home)}):
            checked = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(checked["changed"], [])
        self.assertEqual(self.contents(), before)

    def test_invalid_configuration_retains_failure_receipt(self) -> None:
        (self.repo / "build-config.env").write_text('FFMPEG_TAG="master"\n')
        with self.assertRaises(KeyError):
            STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(json.loads((self.output / "receipt.json").read_text())["status"], "failed")

    def test_latest_release_rebases_and_updates_all_mirrors(self) -> None:
        self.git(self.upstream, "switch", "--detach", "n9.0.1")
        (self.upstream / "other").write_text("new release\n")
        self.git(self.upstream, "add", "other")
        self.git(self.upstream, "commit", "-qm", "release")
        self.git(self.upstream, "tag", "n9.1")
        self.git(self.upstream, "tag", "n10.0-dev")
        result = STACK.maintain(self.repo, self.output, True, True)
        self.assertEqual(result["tag"], "n9.1")
        for name in STACK.MIRRORS:
            self.assertIn("ARG FFMPEG_TAG=n9.1\n", (self.repo / name).read_text())
        self.assertIn('FFMPEG_TAG="n9.1"', (self.repo / "build-config.env").read_text())
        STACK.maintain(self.repo, self.output, False, False)

    def test_conflicting_release_preserves_every_input(self) -> None:
        self.git(self.upstream, "switch", "--detach", "n9.0.1")
        (self.upstream / "sample").write_text("incompatible upstream\n")
        self.git(self.upstream, "commit", "-qam", "release")
        self.git(self.upstream, "tag", "n9.1")
        before = self.contents()
        with self.assertRaises(RuntimeError):
            STACK.maintain(self.repo, self.output, True, True)
        self.assertEqual(self.contents(), before)
        self.assertEqual(json.loads((self.output / "receipt.json").read_text())["status"], "failed")
        self.assertIn("CONFLICT", (self.output / "replay.log").read_text())

    def test_network_failure_is_failure_and_preserves_files(self) -> None:
        config = self.repo / "build-config.env"
        config.write_text(f'FFMPEG_REMOTE="{self.root / "missing"}"\nFFMPEG_TAG="n9.0.1"\n')
        before = self.contents()
        with self.assertRaises(RuntimeError):
            STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.contents(), before)

    def test_hook_git_environment_cannot_redirect_replay(self) -> None:
        before = self.git(self.upstream, "rev-parse", "HEAD")
        index = (self.upstream / ".git/index").read_bytes()
        poisoned = {
            "GIT_DIR": str(self.upstream / ".git"),
            "GIT_WORK_TREE": str(self.upstream),
            "GIT_INDEX_FILE": str(self.upstream / ".git/index"),
            "GIT_COMMON_DIR": str(self.upstream / ".git"),
        }
        with patch.dict(os.environ, poisoned):
            STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.git(self.upstream, "rev-parse", "HEAD"), before)
        self.assertEqual((self.upstream / ".git/index").read_bytes(), index)
        self.assertEqual((self.upstream / "sample").read_text(), "patched\n")

    def test_replace_failure_restores_all_original_bytes(self) -> None:
        config = self.repo / "build-config.env"
        before = self.contents()
        actual_replace = os.replace
        calls = 0
        fail_on_call = 2

        def fail_second(source: Path, target: Path) -> None:
            nonlocal calls
            calls += 1
            if calls == fail_on_call:
                raise OSError("injected replace failure")
            return actual_replace(source, target)

        with patch.object(STACK.os, "replace", side_effect=fail_second):
            with self.assertRaises(OSError):
                STACK.replace_files({self.patch: b"replacement", config: b"new config"})
        self.assertEqual(self.contents(), before)
        self.assertEqual(list(self.repo.rglob(".ffmpeg-refresh-*")), [])

    def test_interrupt_restores_originals(self) -> None:
        config = self.repo / "build-config.env"
        before = self.contents()
        actual_replace = os.replace
        calls = 0
        fail_on_call = 2

        def interrupt_second(source: Path, target: Path) -> None:
            nonlocal calls
            calls += 1
            if calls == fail_on_call:
                raise KeyboardInterrupt
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=interrupt_second),
            self.assertRaises(KeyboardInterrupt),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"})
        self.assertEqual(self.contents(), before)

    def test_second_interrupt_keeps_recovery_backups(self) -> None:
        config = self.repo / "build-config.env"
        original_patch = self.patch.read_bytes()
        actual_replace = os.replace
        calls = 0
        fail_from_call = 2

        def interrupt_recovery(source: Path, target: Path) -> None:
            nonlocal calls
            calls += 1
            if calls >= fail_from_call:
                raise KeyboardInterrupt
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=interrupt_recovery),
            self.assertRaises(KeyboardInterrupt),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"})
        backups = list(self.patch.parent.glob(f".ffmpeg-refresh-{self.patch.name}-original-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), original_patch)

    def test_rollback_failure_keeps_original_backups(self) -> None:
        config = self.repo / "build-config.env"
        original_patch = self.patch.read_bytes()
        actual_replace = os.replace
        calls = 0
        fail_from_call = 2

        def fail_persistently(source: Path, target: Path) -> None:
            nonlocal calls
            calls += 1
            if calls >= fail_from_call:
                raise OSError("filesystem unavailable")
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=fail_persistently),
            self.assertRaisesRegex(RuntimeError, "original backups retained"),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"})
        backups = list(self.patch.parent.glob(f".ffmpeg-refresh-{self.patch.name}-original-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), original_patch)

    def test_invalid_or_incomplete_series(self) -> None:
        path = self.repo / "ffmpeg-patches/series.txt"
        for content in (
            "",
            "../outside.patch\n",
            "0001-integration.patch\n0001-integration.patch\n",
            "0002-missing.patch\n",
        ):
            with self.subTest(content=content), self.assertRaises(ValueError):
                path.write_text(content)
                STACK.series(self.repo)

    def test_unlisted_patch_is_not_silently_omitted(self) -> None:
        (self.repo / "ffmpeg-patches/0018-omitted.patch").write_text(self.patch.read_text())
        with self.assertRaisesRegex(ValueError, "outside series"):
            STACK.series(self.repo)

    def test_late_patch_failure_does_not_rewrite_earlier_patch(self) -> None:
        (self.repo / "ffmpeg-patches/0002-broken.patch").write_text("invalid patch\n")
        (self.repo / "ffmpeg-patches/series.txt").write_text(
            "0001-integration.patch\n0002-broken.patch\n"
        )
        before = self.contents()
        with self.assertRaises(RuntimeError):
            STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.contents(), before)


if __name__ == "__main__":
    unittest.main()

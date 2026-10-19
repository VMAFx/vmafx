"""Exercise real Git replay, release selection and failure preservation."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ffmpeg_shared_series_fixture as shared_series
from scripts.lib.safe_subprocess import run as run_command

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
        git = shutil.which("git")
        self.assertIsNotNone(git)
        assert git is not None
        result = run_command(
            [
                git,
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
            allowed_executables=(git,),
            capture_output=True,
            text=True,
            env=environment,
            check=True,
            timeout_seconds=60,
        )
        assert isinstance(result.stdout, str)
        return result.stdout

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
        series_patch = self.git(self.upstream, "format-patch", "-1", "--stdout")
        self.repo = self.root / "repo"
        (self.repo / "ffmpeg-patches").mkdir(parents=True)
        self.patch = self.repo / "ffmpeg-patches/0001-integration.patch"
        self.patch.write_text(series_patch)
        (self.repo / "ffmpeg-patches/series.txt").write_text(
            "# Full stack\n0001-integration.patch\n"
        )
        self.pin = self.git(self.upstream, "rev-parse", "n9.0.1^{commit}").strip()
        self.head = self.git(self.upstream, "rev-parse", "HEAD").strip()
        (self.repo / "build-config.env").write_text(
            f'FFMPEG_REMOTE="{self.upstream}"\nFFMPEG_TAG="n9.0.1"\nFFMPEG_COMMIT="{self.pin}"\n'
        )
        # The source cache lives under the user cache dir: keep every test off the real one.
        self.cache_home = self.root / "cache-home"
        environment = patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.cache_home)})
        environment.start()
        self.addCleanup(environment.stop)
        for name in STACK.MIRRORS:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"ARG FFMPEG_TAG=n9.0.1\nARG FFMPEG_REMOTE={self.upstream}\n")
        self.output = self.root / "report"

    def cache_repos(self) -> list[Path]:
        return sorted(self.cache_home.glob("vmafx/ffmpeg-patch-stack/*/source.git"))

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
        config = (self.repo / "build-config.env").read_text()
        self.assertIn('FFMPEG_TAG="n9.1"', config)
        self.assertIn(f'FFMPEG_COMMIT="{result["upstream_commit"]}"', config)
        self.assertNotIn(self.pin, config)
        self.assertEqual(result["target_source"], "fetched (unpinned)")
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
        config.write_text(
            f'FFMPEG_REMOTE="{self.root / "missing"}"\nFFMPEG_TAG="n9.0.1"\nFFMPEG_COMMIT="{self.pin}"\n'
        )
        before = self.contents()
        with self.assertRaises(RuntimeError):
            STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.contents(), before)

    def test_second_run_uses_the_cache_without_the_network(self) -> None:
        first = STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(first["source"], "fetched into cache")
        self.assertEqual(len(self.cache_repos()), 1)
        moved = self.root / "upstream-offline"
        self.upstream.rename(moved)  # the configured remote no longer exists
        second = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(second["source"], "cache hit")
        self.assertEqual(second["upstream_commit"], self.pin)
        self.assertEqual(second["changed"], [])

    def test_cache_origin_is_named_in_the_command_output(self) -> None:
        argv = ["ffmpeg_patch_stack.py", "--refresh", "--cache-dir", str(self.root / "c")]
        for expected in ("fetched into cache", "cache hit"):
            with (
                patch.object(STACK, "__file__", str(self.repo / "scripts/ci/x.py")),
                patch.object(STACK.sys, "argv", argv),
                patch("builtins.print") as printed,
            ):
                self.assertEqual(STACK.main(), 0)
            self.assertIn(f"source: {expected}", printed.call_args_list[0].args[0])

    def test_planted_wrong_commit_in_the_cache_fails_naming_both(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        (cache,) = self.cache_repos()
        self.git(cache, "fetch", "--depth=1", str(self.upstream), self.head)
        self.git(cache, "update-ref", "refs/vmafx/tags/n9.0.1", self.head)
        with self.assertRaises(ValueError) as caught:
            STACK.maintain(self.repo, self.output, False, False)
        self.assertIn(self.head, str(caught.exception))
        self.assertIn(self.pin, str(caught.exception))
        self.assertEqual(json.loads((self.output / "receipt.json").read_text())["status"], "failed")

    def test_tag_moved_upstream_fails_naming_both(self) -> None:
        self.git(self.upstream, "tag", "-f", "n9.0.1", self.head)
        with self.assertRaises(ValueError) as caught:
            STACK.maintain(self.repo, self.output, False, False)
        self.assertIn(self.head, str(caught.exception))
        self.assertIn(self.pin, str(caught.exception))
        # The wrong commit never became a cache entry.
        for cache in self.cache_repos():
            self.assertEqual(self.git(cache, "for-each-ref", "refs/vmafx/").strip(), "")

    def test_empty_or_corrupt_cache_is_refetched(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        (cache,) = self.cache_repos()
        shutil.rmtree(cache)
        cache.mkdir()  # empty directory
        self.assertEqual(
            STACK.maintain(self.repo, self.output, False, False)["source"], "fetched into cache"
        )
        (cache / "HEAD").write_text("garbage\n")  # corrupt metadata
        shutil.rmtree(cache / "objects")
        self.assertEqual(
            STACK.maintain(self.repo, self.output, False, False)["source"], "fetched into cache"
        )
        for pack in (cache / "objects").rglob("*"):  # ref kept, objects gone
            if pack.is_file():
                pack.unlink()
        result = STACK.maintain(self.repo, self.output, False, False)
        self.assertIn("fetched into cache", result["source"])
        self.assertEqual(
            STACK.maintain(self.repo, self.output, False, False)["source"], "cache hit"
        )

    def test_missing_or_malformed_pin_is_rejected(self) -> None:
        config = self.repo / "build-config.env"
        text = config.read_text()
        config.write_text(text.replace(f'FFMPEG_COMMIT="{self.pin}"\n', ""))
        with self.assertRaises(KeyError):
            STACK.maintain(self.repo, self.output, False, False)
        config.write_text(text.replace(self.pin, "n9.0.1"))
        with self.assertRaisesRegex(ValueError, "FFMPEG_COMMIT"):
            STACK.maintain(self.repo, self.output, False, False)

    def test_concurrent_runs_share_one_cache(self) -> None:
        STACK.maintain(self.repo, self.root / "prime", True, False, self.root / "other-cache")

        def run(index: int) -> str:
            result = STACK.maintain(self.repo, self.root / f"report-{index}", False, False)
            return str(result["source"])

        with ThreadPoolExecutor(max_workers=4) as pool:
            sources = list(pool.map(run, range(4)))
        self.assertEqual(sources.count("fetched into cache"), 1)
        self.assertEqual(sources.count("cache hit"), 3)

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


class SharedSeriesReplay(RealReplay):
    """The shared FFmpeg fix series is verified and applied before ours (ADR-3143)."""

    def setUp(self) -> None:
        super().setUp()
        self.git(self.upstream, "switch", "-q", "--detach", "n9.0.1")
        (self.upstream / "shared-fix").write_text("fixed upstream of the series\n")
        self.git(self.upstream, "add", "shared-fix")
        self.git(self.upstream, "commit", "-qm", "shared fix")
        self.shared_patch = self.git(self.upstream, "format-patch", "-1", "--stdout")
        self.git(self.upstream, "switch", "-q", "--detach", self.head)
        self.base_config = (self.repo / "build-config.env").read_text()
        self.pin_series("n9.0.1", self.pin)

    def pin_series(self, tag: str, commit: str, **members: bytes) -> None:
        archive, digest = shared_series.tarball(
            self.root / "shared-series",
            tag,
            commit,
            {shared_series.PATCH_NAME: self.shared_patch},
            extra_members=members,
        )
        (self.repo / "build-config.env").write_text(
            self.base_config + shared_series.pins(archive, digest)
        )

    def assert_refused(self, error: type[Exception], needle: str) -> None:
        before = self.contents()
        with self.assertRaisesRegex(error, needle):
            STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(self.contents(), before)
        self.assertEqual(json.loads((self.output / "receipt.json").read_text())["status"], "failed")

    def test_series_is_applied_first_and_ours_are_rendered_alone(self) -> None:
        result = STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(result["shared_series"], shared_series.TAG)
        self.assertEqual(result["shared_patch_count"], 1)
        self.assertEqual(result["patch_count"], 1)
        log = (self.output / "replay.log").read_text()
        self.assertLess(log.index(shared_series.PATCH_NAME), log.index("0001-integration.patch"))
        rendered = self.patch.read_text()
        self.assertIn("add integration", rendered)
        self.assertNotIn("shared fix", rendered)
        self.assertEqual(STACK.maintain(self.repo, self.output, False, False)["changed"], [])

    def test_another_sha256_is_refused(self) -> None:
        config = self.repo / "build-config.env"
        text = config.read_text()
        digest = text.split('FFMPEG_FIX_SERIES_SHA256="', 1)[1][:64]
        config.write_text(text.replace(digest, "0" * 64))
        self.assert_refused(RuntimeError, "build-config.env pins")

    def test_series_for_another_ffmpeg_is_refused(self) -> None:
        self.pin_series("n9.9.9", self.pin)
        self.assert_refused(RuntimeError, "targets n9.9.9")
        self.pin_series("n9.0.1", "0" * 40)
        self.assert_refused(RuntimeError, "targets n9.0.1")

    def test_member_outside_the_archive_is_refused(self) -> None:
        self.pin_series("n9.0.1", self.pin, **{"../escaped": b"x"})
        self.assert_refused(RuntimeError, "leaves the archive root")
        self.assertFalse((self.root / "escaped").exists())

    def test_partial_pin_is_refused(self) -> None:
        config = self.repo / "build-config.env"
        lines = [
            line
            for line in config.read_text().splitlines()
            if not line.startswith("FFMPEG_FIX_SERIES_SHA256")
        ]
        config.write_text("\n".join(lines) + "\n")
        self.assert_refused(ValueError, "without")

    def test_required_signature_cannot_be_skipped(self) -> None:
        with patch.dict(os.environ, {"FFMPEG_FIX_SERIES_VERIFY": "cosign"}):
            self.assert_refused(RuntimeError, "cosign|signature cannot be checked")

    # With a series pinned, the inherited release move is refused instead.
    test_latest_release_rebases_and_updates_all_mirrors = None  # type: ignore[assignment]

    def test_new_ffmpeg_release_waits_for_a_series_release(self) -> None:
        self.git(self.upstream, "switch", "-q", "--detach", "n9.0.1")
        (self.upstream / "other").write_text("new release\n")
        self.git(self.upstream, "add", "other")
        self.git(self.upstream, "commit", "-qm", "release")
        self.git(self.upstream, "tag", "n9.1")
        before = self.contents()
        with self.assertRaisesRegex(
            ValueError, "n9.1 is released, but the pinned shared fix series"
        ):
            STACK.maintain(self.repo, self.output, True, True)
        self.assertEqual(self.contents(), before)
        self.assertIn(
            "add integration", (self.output / "patches/0001-integration.patch").read_text()
        )


if __name__ == "__main__":
    unittest.main()

"""Exercise real Git replay, release selection and failure preservation."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import hashlib
import importlib.util
import json
import os
import re
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


class BashInputsLineEndings(unittest.TestCase):
    def test_shared_series_inputs_check_out_with_lf(self) -> None:
        repo = Path(__file__).resolve().parents[2]
        paths = [STACK.SHARED_SERIES_SCRIPT.relative_to(repo).as_posix(), "build-config.env"]
        git = shutil.which("git")
        assert git is not None
        result = run_command(
            [git, "-C", str(repo), "check-attr", "eol", "--", *paths],
            allowed_executables={git},
            # The caller's GIT_DIR or GIT_WORK_TREE would point git at another repository.
            env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=30,
        )
        self.assertEqual(result.stdout.splitlines(), [f"{path}: eol: lf" for path in paths])


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
        # The refresh lock and journal live in the checkout's Git directory.
        self.git(self.repo, "init", "-q")
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
        self.readme = self.repo / "ffmpeg-patches/README.md"
        self.readme.write_text(
            "> `n9.0.2` checkout (cumulative `git am --3way`), NOT a per-patch\n"
            "> `git apply --check`. The latter rejects `0002+` because they\n"
            "> reference cumulative-state hunks that don't exist in pristine `n9.0.2`.\n"
            "> The 1 patches here replay cumulatively on upstream commit\n"
            f"> `{self.pin}`; the resulting tree is\n"
            "> `0000000000000000000000000000000000000000`\n"
        )
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

    def test_keep_tree_copies_the_patched_sources_without_git(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        kept = self.root / "kept"
        result = STACK.maintain(self.repo, self.output, False, False, keep_tree=kept)
        self.assertEqual(result["kept_tree"], str(kept))
        self.assertEqual((kept / "sample").read_text(), "patched\n")
        self.assertFalse((kept / ".git").exists())

    def test_keep_tree_refuses_an_existing_destination(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        kept = self.root / "kept"
        kept.mkdir()
        (kept / "stale").write_text("old\n")
        with self.assertRaisesRegex(ValueError, "destination exists"):
            STACK.maintain(self.repo, self.output, False, False, keep_tree=kept)
        self.assertEqual(sorted(path.name for path in kept.iterdir()), ["stale"])

    def test_source_file_is_the_patch_content(self) -> None:
        # RC4 WP9: ffmpeg-patches/src/ holds the source of a file a patch adds.
        sources = {"sample": "sample.c"}
        (self.repo / "ffmpeg-patches/src").mkdir()
        source = self.repo / "ffmpeg-patches/src/sample.c"
        source.write_text("patched\n")
        with patch.object(STACK, "SOURCES", sources):
            STACK.maintain(self.repo, self.output, True, False)
            self.assertEqual(STACK.maintain(self.repo, self.output, False, False)["changed"], [])
            source.write_text("edited in the source file\n")
            with self.assertRaisesRegex(ValueError, "drift"):
                STACK.maintain(self.repo, self.output, False, False)
            refreshed = STACK.maintain(self.repo, self.output, True, False)
            self.assertEqual(
                sorted(refreshed["changed"]),
                ["ffmpeg-patches/0001-integration.patch", "ffmpeg-patches/README.md"],
            )
            self.assertIn("+edited in the source file", self.patch.read_text())
            self.assertEqual(STACK.maintain(self.repo, self.output, False, False)["changed"], [])

    def test_source_file_without_a_patch_fails(self) -> None:
        (self.repo / "ffmpeg-patches/src").mkdir()
        (self.repo / "ffmpeg-patches/src/orphan.c").write_text("x\n")
        with (
            patch.object(STACK, "SOURCES", {"libavfilter/orphan.c": "orphan.c"}),
            self.assertRaisesRegex(ValueError, "no patch in series.txt adds libavfilter/orphan.c"),
        ):
            STACK.maintain(self.repo, self.output, False, False)

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
            if Path(target).name == STACK.JOURNAL_NAME:
                return actual_replace(source, target)
            nonlocal calls
            calls += 1
            if calls == fail_on_call:
                raise OSError("injected replace failure")
            return actual_replace(source, target)

        with patch.object(STACK.os, "replace", side_effect=fail_second):
            with self.assertRaises(OSError):
                STACK.replace_files({self.patch: b"replacement", config: b"new config"}, self.repo)
        self.assertEqual(self.contents(), before)
        self.assertEqual(list(self.repo.rglob(".ffmpeg-refresh-*")), [])

    def test_interrupt_restores_originals(self) -> None:
        config = self.repo / "build-config.env"
        before = self.contents()
        actual_replace = os.replace
        calls = 0
        fail_on_call = 2

        def interrupt_second(source: Path, target: Path) -> None:
            if Path(target).name == STACK.JOURNAL_NAME:
                return actual_replace(source, target)
            nonlocal calls
            calls += 1
            if calls == fail_on_call:
                raise KeyboardInterrupt
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=interrupt_second),
            self.assertRaises(KeyboardInterrupt),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"}, self.repo)
        self.assertEqual(self.contents(), before)

    def test_second_interrupt_keeps_recovery_backups(self) -> None:
        config = self.repo / "build-config.env"
        original_patch = self.patch.read_bytes()
        actual_replace = os.replace
        calls = 0
        fail_from_call = 2

        def interrupt_recovery(source: Path, target: Path) -> None:
            if Path(target).name == STACK.JOURNAL_NAME:
                return actual_replace(source, target)
            nonlocal calls
            calls += 1
            if calls >= fail_from_call:
                raise KeyboardInterrupt
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=interrupt_recovery),
            self.assertRaises(KeyboardInterrupt),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"}, self.repo)
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
            if Path(target).name == STACK.JOURNAL_NAME:
                return actual_replace(source, target)
            nonlocal calls
            calls += 1
            if calls >= fail_from_call:
                raise OSError("filesystem unavailable")
            return actual_replace(source, target)

        with (
            patch.object(STACK.os, "replace", side_effect=fail_persistently),
            self.assertRaisesRegex(RuntimeError, "original backups retained"),
        ):
            STACK.replace_files({self.patch: b"replacement", config: b"new config"}, self.repo)
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

    def test_readme_claim_refreshes_and_passes_check(self) -> None:
        result = STACK.maintain(self.repo, self.output, True, False)
        readme = (self.repo / "ffmpeg-patches/README.md").read_text()
        claim_match = STACK.README_CLAIM_PATTERN.search(readme)
        self.assertIsNotNone(claim_match)
        assert claim_match is not None
        claim_text = claim_match.group(0)
        self.assertIn(result["upstream_commit"], claim_text)
        self.assertIn(result["patched_tree"], claim_text)
        self.assertIn(f"`{result['tag']}` checkout", claim_text)
        self.assertIn(f"pristine `{result['tag']}`", claim_text)
        if result.get("shared_series", "none") != "none":
            self.assertIn(f"shared fix series `{result['shared_series']}`", claim_text)
            self.assertIn(f"and the {result['patch_count']}\n> patches here", claim_text)
        else:
            self.assertIn(f"The {result['patch_count']} patches here", claim_text)
        checked = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(checked["changed"], [])

    def test_refresh_fails_when_lock_held(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        lock = STACK.repo_git_dir(self.repo) / "ffmpeg-patch-stack.lock"
        lock.write_text("54321\n")
        guidance = (
            "if that process is no longer running, delete the lock file and retry the refresh"
        )
        try:
            with self.assertRaisesRegex(
                RuntimeError, rf"{re.escape(str(lock))}.*54321.*{re.escape(guidance)}"
            ):
                STACK.maintain(self.repo, self.output, True, False)
            checked = STACK.maintain(self.repo, self.output, False, False)
            self.assertEqual(checked["changed"], [])
            argv = ["ffmpeg_patch_stack.py", "--refresh"]
            with (
                patch.object(STACK, "__file__", str(self.repo / "scripts/ci/x.py")),
                patch.object(STACK.sys, "argv", argv),
                patch("builtins.print") as printed,
            ):
                self.assertEqual(STACK.main(), 1)
                output = " ".join(call.args[0] for call in printed.call_args_list if call.args)
                self.assertIn(str(lock), output)
                self.assertIn("54321", output)
                self.assertIn(guidance, output)
        finally:
            lock.unlink(missing_ok=True)

    def test_check_mode_outside_a_git_checkout_succeeds(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        plain = self.root / "plain"
        shutil.copytree(self.repo, plain, ignore=shutil.ignore_patterns(".git"))
        STACK.recover_journal(plain, False)
        with STACK.maintain_lock(plain, enabled=False):
            pass
        checked = STACK.maintain(plain, self.root / "plain-report", False, False)
        self.assertEqual(checked["changed"], [])
        with self.assertRaisesRegex(RuntimeError, "not a Git checkout"):
            STACK.recover_journal(plain, True)
        with self.assertRaisesRegex(RuntimeError, "not a Git checkout"):
            with STACK.maintain_lock(plain, enabled=True):
                pass
        with self.assertRaisesRegex(RuntimeError, "not a Git checkout"):
            STACK.maintain(plain, self.root / "plain-report", True, False)

    def test_refresh_outside_a_git_checkout_fails(self) -> None:
        shutil.rmtree(self.repo / ".git")
        with self.assertRaisesRegex(RuntimeError, "not a Git checkout"):
            STACK.maintain(self.repo, self.output, True, False)

    def test_interrupted_replace_recovers_from_journal(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        config = self.repo / "build-config.env"
        original_config = config.read_bytes()
        new_config = original_config + b"# journal recovery test\n"
        temp_config = self.repo / ".ffmpeg-refresh-build-config.env-candidate-recovery"
        temp_config.write_bytes(new_config)
        backup_config = self.repo / ".ffmpeg-refresh-build-config.env-original-recovery"
        backup_config.write_bytes(original_config)

        original_patch = self.patch.read_bytes()
        new_patch = original_patch + b"\n# patch candidate\n"
        temp_patch = self.repo / f".ffmpeg-refresh-{self.patch.name}-candidate-recovery"
        temp_patch.write_bytes(new_patch)
        backup_patch = self.repo / f".ffmpeg-refresh-{self.patch.name}-original-recovery"
        backup_patch.write_bytes(original_patch)

        # Simulate interrupted mid-rename: self.patch was already renamed to candidate,
        # while config is untouched (still original_config).
        self.patch.write_bytes(new_patch)

        git_dir = STACK.repo_git_dir(self.repo)
        journal = git_dir / STACK.JOURNAL_NAME
        journal_entries = [
            {
                "target": str(self.patch),
                "temp": str(temp_patch),
                "backup": str(backup_patch),
                "original_sha256": hashlib.sha256(original_patch).hexdigest(),
                "candidate_sha256": hashlib.sha256(new_patch).hexdigest(),
            },
            {
                "target": str(config),
                "temp": str(temp_config),
                "backup": str(backup_config),
                "original_sha256": hashlib.sha256(original_config).hexdigest(),
                "candidate_sha256": hashlib.sha256(new_config).hexdigest(),
            },
        ]
        STACK.write_journal_atomically(journal, journal_entries)

        # --check never mutates the tree; it names the journal and the recovery command.
        with self.assertRaisesRegex(ValueError, "interrupted refresh journal"):
            STACK.maintain(self.repo, self.output, False, False)
        self.assertTrue(journal.exists())
        STACK.recover_journal(self.repo, True)
        self.assertFalse(journal.exists())
        self.assertFalse(temp_config.exists())
        self.assertFalse(backup_config.exists())
        self.assertFalse(temp_patch.exists())
        self.assertFalse(backup_patch.exists())
        self.assertEqual(self.patch.read_bytes(), original_patch)
        self.assertEqual(config.read_bytes(), original_config)

    def test_target_hand_edited_after_stop_raises(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        config = self.repo / "build-config.env"
        original_config = config.read_bytes()
        new_config = original_config + b"# candidate\n"
        temp_config = self.repo / ".ffmpeg-refresh-build-config.env-candidate-recovery"
        temp_config.write_bytes(new_config)
        backup_config = self.repo / ".ffmpeg-refresh-build-config.env-original-recovery"
        backup_config.write_bytes(original_config)

        git_dir = STACK.repo_git_dir(self.repo)
        journal = git_dir / STACK.JOURNAL_NAME
        journal_entries = [
            {
                "target": str(config),
                "temp": str(temp_config),
                "backup": str(backup_config),
                "original_sha256": hashlib.sha256(original_config).hexdigest(),
                "candidate_sha256": hashlib.sha256(new_config).hexdigest(),
            }
        ]
        STACK.write_journal_atomically(journal, journal_entries)

        # Target hand-edited after stop
        hand_edited = original_config + b"# manual edit\n"
        config.write_bytes(hand_edited)

        expected_msg = (
            f"{config} changed since the interrupted refresh; "
            f"resolve by hand, then delete {journal}"
        )
        with self.assertRaises(ValueError) as ctx:
            STACK.recover_journal(self.repo, True)
        self.assertEqual(str(ctx.exception), expected_msg)

        # No file changed, journal kept
        self.assertTrue(journal.exists())
        self.assertEqual(config.read_bytes(), hand_edited)
        self.assertTrue(temp_config.exists())
        self.assertTrue(backup_config.exists())

    def test_rollback_interrupted_recovers_all_originals(self) -> None:
        file1 = self.patch
        file2 = self.repo / "build-config.env"
        file3 = self.repo / "ffmpeg-patches/series.txt"
        before = {f: f.read_bytes() for f in (file1, file2, file3)}
        before_contents = self.contents()

        actual_replace = os.replace
        calls = 0
        fail_forward_call = 3
        fail_rollback_call = 5

        def fail_second_rollback(source: Path, target: Path) -> None:
            if Path(target).name == STACK.JOURNAL_NAME:
                return actual_replace(source, target)
            nonlocal calls
            calls += 1
            if calls == fail_forward_call:
                raise OSError("forward rename failed")
            if calls == fail_rollback_call:
                raise OSError("second rollback replace failed")
            return actual_replace(source, target)

        journal = STACK.repo_git_dir(self.repo) / STACK.JOURNAL_NAME
        with (
            patch.object(STACK.os, "replace", side_effect=fail_second_rollback),
            self.assertRaisesRegex(RuntimeError, "original backups retained"),
        ):
            STACK.replace_files(
                {file1: b"new1\n", file2: b"new2\n", file3: b"new3\n"},
                self.repo,
            )

        # Rollback interrupted: journal kept, file1 still candidate, file2 rolled back
        self.assertTrue(journal.exists())
        self.assertEqual(file1.read_bytes(), b"new1\n")
        self.assertEqual(file2.read_bytes(), before[file2])
        self.assertEqual(file3.read_bytes(), before[file3])

        # A later recovery restores all originals
        STACK.recover_journal(self.repo, True)
        self.assertFalse(journal.exists())
        self.assertEqual(file1.read_bytes(), before[file1])
        self.assertEqual(file2.read_bytes(), before[file2])
        self.assertEqual(file3.read_bytes(), before[file3])
        self.assertEqual(self.contents(), before_contents)

    def test_recovery_empty_journal_removes_journal(self) -> None:
        before = self.contents()
        git_dir = STACK.repo_git_dir(self.repo)
        journal = git_dir / STACK.JOURNAL_NAME
        journal.write_text("[]", encoding="utf-8")
        STACK.recover_journal(self.repo, True)
        self.assertFalse(journal.exists())
        self.assertEqual(self.contents(), before)

    def test_tree_hash_extra_hex_digit_reports_drift(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        stale = re.sub(
            r"(the resulting tree is\r?\n> `[0-9a-f]{40})`",
            r"\g<1>a`",
            readme.read_text(),
        )
        readme.write_text(stale)
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.maintain(self.repo, self.output, False, False)
        receipt = json.loads((self.output / "receipt.json").read_text())
        self.assertEqual(receipt["changed"], ["ffmpeg-patches/README.md"])

    def test_stale_claim_with_correct_hash_elsewhere_fails_check(self) -> None:
        result = STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        text = readme.read_text()
        tree = result["patched_tree"]
        stale = text.replace(f"`{tree}`", f"`{'0' * 40}`")
        stale += f"\n<!-- correct tree was {tree} -->\n"
        readme.write_text(stale)
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.maintain(self.repo, self.output, False, False)
        receipt = json.loads((self.output / "receipt.json").read_text())
        self.assertEqual(receipt["changed"], ["ffmpeg-patches/README.md"])

    def test_crlf_readme_with_current_claim_passes_check_and_refresh_writes_nothing(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        crlf_text = readme.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        readme.write_bytes(crlf_text)
        checked = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(checked["changed"], [])
        refreshed = STACK.maintain(self.repo, self.output, True, False)
        self.assertEqual(refreshed["changed"], [])
        self.assertIn(b"\r\n", readme.read_bytes())

    def test_tag_change_reports_drift(self) -> None:
        result = STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        text = readme.read_text()
        current_tag = result["tag"]
        stale = text.replace(f"`{current_tag}`", "`n9.9.9`")
        readme.write_text(stale)
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.maintain(self.repo, self.output, False, False)
        receipt = json.loads((self.output / "receipt.json").read_text())
        self.assertEqual(receipt["changed"], ["ffmpeg-patches/README.md"])

    def test_stale_readme_claim_fails_check_with_drift(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        stale = re.sub(r"The \d+ patches", "The 999 patches", readme.read_text())
        readme.write_text(stale)
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.maintain(self.repo, self.output, False, False)
        receipt = json.loads((self.output / "receipt.json").read_text())
        self.assertEqual(receipt["changed"], ["ffmpeg-patches/README.md"])

    def test_readme_claim_missing_or_duplicate_raises_value_error(self) -> None:
        STACK.maintain(self.repo, self.output, True, False)
        readme = self.repo / "ffmpeg-patches/README.md"
        valid = readme.read_text()

        readme.write_text("No claim here.\n")
        with self.assertRaisesRegex(ValueError, r"ffmpeg-patches/README\.md"):
            STACK.maintain(self.repo, self.output, False, False)

        readme.write_text(valid + "\n" + valid)
        with self.assertRaisesRegex(ValueError, r"ffmpeg-patches/README\.md"):
            STACK.maintain(self.repo, self.output, False, False)


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

    def test_tarball_not_listed_in_sha256sums_is_refused(self) -> None:
        # Planted defect: the pin matches the tarball but the release's SHA256SUMS lists another.
        sums = self.root / "shared-series" / "SHA256SUMS"
        sums.write_text(f"{'0' * 64}  ffmpeg-patches-{shared_series.TAG}.tar.gz\n")
        self.assert_refused(RuntimeError, "SHA256SUMS of .* lists 0{64}")
        sums.write_text("")
        self.assert_refused(RuntimeError, "lists nothing")

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

    def test_readme_claim_renders_shared_series_and_passes_check(self) -> None:
        result = STACK.maintain(self.repo, self.output, True, False)
        readme = (self.repo / "ffmpeg-patches/README.md").read_text()
        claim_match = STACK.README_CLAIM_PATTERN.search(readme)
        self.assertIsNotNone(claim_match)
        assert claim_match is not None
        claim_text = claim_match.group(0)
        self.assertIn(
            f"The {result['shared_patch_count']} patches of the shared fix series `{result['shared_series']}`",
            claim_text,
        )
        self.assertIn(f"and the {result['patch_count']}\n> patches here", claim_text)
        self.assertIn(result["upstream_commit"], claim_text)
        self.assertIn(result["patched_tree"], claim_text)
        self.assertIn(f"`{result['tag']}` checkout", claim_text)
        self.assertIn(f"pristine `{result['tag']}`", claim_text)
        checked = STACK.maintain(self.repo, self.output, False, False)
        self.assertEqual(checked["changed"], [])


def ffmpeg_guard(path: str) -> str:
    """The inclusion guard FFmpeg's tests/fate/source-check.sh requires of `path`."""
    macro = re.sub(r"[^A-Za-z0-9]+", "_", re.sub(r"^lib", "", path))
    for infix in ("_af_", "_vf_", "_avf_", "_vaf_"):
        macro = macro.replace(infix, "_", 1)
    return macro.upper()


def added_headers(patch_text: str) -> dict[str, list[str]]:
    """FFmpeg path -> added lines of every header a patch creates."""
    headers: dict[str, list[str]] = {}
    for chunk in patch_text.split("\ndiff --git a/")[1:]:
        path = chunk.split(" ", 1)[0]
        if path.endswith(".h") and "\nnew file mode" in chunk.split("\n@@", 1)[0]:
            headers[path] = [line[1:] for line in chunk.splitlines() if line.startswith("+")]
    return headers


def missing_guards(headers: dict[str, list[str]]) -> list[str]:
    return [path for path, lines in headers.items() if f"#define {ffmpeg_guard(path)}" not in lines]


class HeaderGuards(unittest.TestCase):
    """fate-source fails on a header the series adds without FFmpeg's guard name."""

    PATCHES = Path(__file__).resolve().parents[2] / "ffmpeg-patches"

    def test_guard_names_follow_fate_source(self) -> None:
        self.assertEqual(ffmpeg_guard("libavfilter/vf_vmafx_options.h"), "AVFILTER_VMAFX_OPTIONS_H")
        self.assertEqual(ffmpeg_guard("fftools/ffmpeg_qpfile.h"), "FFTOOLS_FFMPEG_QPFILE_H")

    def test_another_guard_is_refused(self) -> None:
        patch_text = (
            "x\ndiff --git a/libavfilter/vf_x.h b/libavfilter/vf_x.h\nnew file mode 100644\n"
            "@@ -0,0 +1,2 @@\n+#ifndef VMAFX_VF_X_H\n+#define VMAFX_VF_X_H\n"
        )
        self.assertEqual(missing_guards(added_headers(patch_text)), ["libavfilter/vf_x.h"])

    def test_every_added_header_has_the_ffmpeg_guard(self) -> None:
        series = (self.PATCHES / "series.txt").read_text(encoding="utf-8").splitlines()
        names = [line.strip() for line in series if line.strip() and not line.startswith("#")]
        headers: dict[str, list[str]] = {}
        for name in names:
            headers.update(added_headers((self.PATCHES / name).read_text(encoding="utf-8")))
        for path, source in STACK.SOURCES.items():
            if path.endswith(".h"):
                headers[path] = (self.PATCHES / "src" / source).read_text().splitlines()
        self.assertIn("libavfilter/vf_vmafx_options.h", headers)
        self.assertEqual(missing_guards(headers), [])


class AtomicJournalWrite(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.temp_dir = Path(self.temporary.name)
        self.journal = self.temp_dir / STACK.JOURNAL_NAME

    def test_write_journal_atomically_positive(self) -> None:
        data = [{"temp": "/path/to/temp1", "target": "/path/to/target1"}]
        STACK.write_journal_atomically(self.journal, data)
        self.assertTrue(self.journal.exists())
        loaded = json.loads(self.journal.read_text(encoding="utf-8"))
        self.assertEqual(loaded, data)
        leftovers = list(self.temp_dir.glob(f".{STACK.JOURNAL_NAME}-*"))
        self.assertEqual(leftovers, [])

    def test_write_journal_atomically_negative(self) -> None:
        initial_content = '[{"temp": "preserved", "target": "preserved"}]'
        self.journal.write_text(initial_content, encoding="utf-8")
        data = [{"temp": "/path/to/new_temp", "target": "/path/to/new_target"}]
        with (
            patch.object(STACK.json, "dump", side_effect=ValueError("simulated dump error")),
            self.assertRaises(ValueError),
        ):
            STACK.write_journal_atomically(self.journal, data)
        self.assertEqual(self.journal.read_text(encoding="utf-8"), initial_content)
        leftovers = list(self.temp_dir.glob(f".{STACK.JOURNAL_NAME}-*"))
        self.assertEqual(leftovers, [])

    def test_write_journal_atomically_boundary(self) -> None:
        STACK.write_journal_atomically(self.journal, [])
        self.assertTrue(self.journal.exists())
        self.assertEqual(self.journal.read_text(encoding="utf-8"), "[]")
        leftovers = list(self.temp_dir.glob(f".{STACK.JOURNAL_NAME}-*"))
        self.assertEqual(leftovers, [])


class WorkingTreeTextLineEndings(unittest.TestCase):
    """Ensure Windows CRLF working tree checkouts do not cause spurious drift or leak CR."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)

    def test_read_repo_text_bytes_normalises_crlf(self) -> None:
        target = self.repo / "sample.txt"
        target.write_bytes(b"first\r\nsecond\r\nthird\n")
        self.assertEqual(STACK.read_repo_text_bytes(target), b"first\nsecond\nthird\n")

    def test_read_repo_text_bytes_preserves_lone_cr(self) -> None:
        target = self.repo / "lone_cr.txt"
        target.write_bytes(b"first\rsecond\r\n")
        self.assertEqual(STACK.read_repo_text_bytes(target), b"first\rsecond\n")

    def test_settle_updates_crlf_target_passes_check_without_drift(self) -> None:
        target = self.repo / "mirrored.txt"
        data = b"ARG FFMPEG_TAG=n9.0.2\nARG FFMPEG_REMOTE=https://example.com/ffmpeg\n"
        target.write_bytes(data.replace(b"\n", b"\r\n"))
        receipt: dict[str, object] = {}
        STACK.settle_updates(self.repo, {target: data}, refresh=False, receipt=receipt)
        self.assertEqual(receipt["changed"], [])
        self.assertEqual(receipt["status"], "checked")

    def test_settle_updates_real_content_difference_with_crlf_raises_drift(self) -> None:
        target = self.repo / "mirrored.txt"
        data = b"ARG FFMPEG_TAG=n9.0.2\n"
        target.write_bytes(b"ARG FFMPEG_TAG=n9.0.1\r\n")
        receipt: dict[str, object] = {}
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.settle_updates(self.repo, {target: data}, refresh=False, receipt=receipt)
        self.assertEqual(receipt["changed"], ["mirrored.txt"])

    def test_settle_updates_lone_cr_is_not_normalised_and_raises_drift(self) -> None:
        target = self.repo / "mirrored.txt"
        data = b"line 1\nline 2\n"
        target.write_bytes(b"line 1\rline 2\n")
        receipt: dict[str, object] = {}
        with self.assertRaisesRegex(ValueError, "drift"):
            STACK.settle_updates(self.repo, {target: data}, refresh=False, receipt=receipt)
        self.assertEqual(receipt["changed"], ["mirrored.txt"])

    def test_sync_sources_crlf_fragment_writes_lf_target(self) -> None:
        src_dir = self.repo / "ffmpeg-patches/src"
        src_dir.mkdir(parents=True, exist_ok=True)
        ffmpeg_path = "libavfilter/vf_vmafx.c"
        src_file = src_dir / "vf_vmafx.c"
        src_file.write_bytes(b"/* header */\r\nstatic int fn(void) {\r\n    return 0;\r\n}\r\n")

        checkout = self.repo / "checkout"
        target = checkout / ffmpeg_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"/* initial */\n")

        class FakeReplay:
            def __init__(self, chk: Path) -> None:
                self.checkout = chk
                self.calls: list[tuple[str, ...]] = []

            def git(self, *args: str) -> str:
                self.calls.append(args)
                if args and args[0] == "diff-tree":
                    return f"{ffmpeg_path}\n"
                return ""

        fake_replay = FakeReplay(checkout)
        owners: dict[str, str] = {}
        STACK.sync_sources(self.repo, fake_replay, "0001-test.patch", owners)

        self.assertEqual(owners[ffmpeg_path], "0001-test.patch")
        expected_lf = b"/* header */\nstatic int fn(void) {\n    return 0;\n}\n"
        self.assertEqual(target.read_bytes(), expected_lf)
        self.assertNotIn(b"\r", target.read_bytes())
        self.assertIn(("add", "--", ffmpeg_path), fake_replay.calls)
        self.assertIn(("commit", "--quiet", "--amend", "--no-edit"), fake_replay.calls)


CONFIG_H_INCLUDE = re.compile(r'^\s*#\s*include\s*["<]config\.h[">]')
CONDITIONAL_CONFIG = re.compile(r"^\s*#\s*(?:if|elif|ifdef|ifndef)\b.*\b(?:HAVE_|CONFIG_)\w+")


def unconfigured_conditionals(source: str | Path) -> list[str]:
    """Return premature HAVE_/CONFIG_ conditionals appearing before #include "config.h"."""
    text = source.read_text(encoding="utf-8") if isinstance(source, Path) else source
    violations: list[str] = []
    for line in text.splitlines():
        if CONFIG_H_INCLUDE.match(line):
            break
        if CONDITIONAL_CONFIG.match(line):
            violations.append(line.strip())
    return violations


class ConfigHeaderPrecedence(unittest.TestCase):
    """Premature HAVE_/CONFIG_ conditionals before config.h leave macros undefined."""

    PATCHES_SRC = Path(__file__).resolve().parents[2] / "ffmpeg-patches" / "src"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.temp_dir = Path(self.temporary.name)

    def test_planted_negative_missing_config_h_reported(self) -> None:
        sample = self.temp_dir / "bad.c"
        sample.write_text(
            '#include "config_components.h"\n\n#if HAVE_UNISTD_H\n#include <unistd.h>\n#endif\n',
            encoding="utf-8",
        )
        self.assertEqual(unconfigured_conditionals(sample), ["#if HAVE_UNISTD_H"])

    def test_positive_config_h_preceding_conditionals(self) -> None:
        sample = self.temp_dir / "good.c"
        sample.write_text(
            '#include "config.h"\n#include "config_components.h"\n\n'
            "#if HAVE_UNISTD_H\n#include <unistd.h>\n#endif\n",
            encoding="utf-8",
        )
        self.assertEqual(unconfigured_conditionals(sample), [])

    def test_positive_file_without_conditionals(self) -> None:
        sample = self.temp_dir / "plain.c"
        sample.write_text("#include <stdio.h>\nint main(void) { return 0; }\n", encoding="utf-8")
        self.assertEqual(unconfigured_conditionals(sample), [])

    def test_every_src_file_includes_config_h_before_conditionals(self) -> None:
        src_files = sorted(self.PATCHES_SRC.glob("*.[ch]"))
        self.assertTrue(src_files, "no source files found in ffmpeg-patches/src")
        for src_path in src_files:
            with self.subTest(file=src_path.name):
                self.assertEqual(
                    unconfigured_conditionals(src_path),
                    [],
                    f'{src_path.name} uses HAVE_/CONFIG_ conditional before #include "config.h"',
                )


if __name__ == "__main__":
    unittest.main()

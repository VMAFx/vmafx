#!/usr/bin/env python3
"""Replay and refresh the ordered FFmpeg series against stable release tags."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import sys
import tempfile
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import IO, TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

STABLE_TAG = re.compile(r"n(\d+)\.(\d+)(?:\.(\d+))?\Z")
PATCH_NAME = re.compile(r"\d{4}-[A-Za-z0-9_.-]+\.patch\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
CACHE_LOCK_SECONDS = 600
CACHE_FETCH_SECONDS = 180
MIRRORS = ("Dockerfile", "Dockerfile.ffmpeg", "dev/Containerfile", "docker/Dockerfile.node")
# The shared FFmpeg fix series every build applies before ffmpeg-patches/
# (ADR-3143): three pins in build-config.env, one script that fetches,
# verifies and applies it.
SHARED_SERIES_KEYS = (
    "FFMPEG_FIX_SERIES_REPO",
    "FFMPEG_FIX_SERIES_TAG",
    "FFMPEG_FIX_SERIES_SHA256",
)
SHARED_SERIES_SCRIPT = Path(__file__).with_name("ffmpeg-shared-series.sh")
SHARED_SERIES_SECONDS = 600


def stable_version(tag: str) -> tuple[int, int, int]:
    """Reject development/RC refs, even when their prefix looks released."""
    match = STABLE_TAG.fullmatch(tag)
    if not match:
        raise ValueError(f"not a stable FFmpeg release tag: {tag}")
    major, minor, patch = match.groups()
    return int(major), int(minor), int(patch or 0)


def latest_release(refs: str) -> str:
    tags = [line.split()[1].removeprefix("refs/tags/") for line in refs.splitlines()]
    stable = [tag for tag in tags if STABLE_TAG.fullmatch(tag)]
    if not stable:
        raise ValueError("upstream returned no stable FFmpeg release tags")
    return max(stable, key=stable_version)


def configuration(repo: Path) -> tuple[str, str, str]:
    """Return the remote, the release tag and the upstream commit the tag must name."""
    values = {}
    for line in (repo / "build-config.env").read_text().splitlines():
        key, sep, value = line.partition("=")
        if sep and key in {"FFMPEG_REMOTE", "FFMPEG_TAG", "FFMPEG_COMMIT"}:
            parts = shlex.split(value, comments=True)
            if len(parts) != 1 or key in values:
                raise ValueError(f"invalid or duplicate build-config.env {key}")
            values[key] = parts[0]
    remote, tag, commit = values["FFMPEG_REMOTE"], values["FFMPEG_TAG"], values["FFMPEG_COMMIT"]
    stable_version(tag)
    if not COMMIT.fullmatch(commit):
        raise ValueError(f"FFMPEG_COMMIT is not a full lowercase commit id: {commit}")
    return remote, tag, commit


def shared_series_tag(repo: Path) -> str | None:
    """The pinned shared fix series, or None when build-config.env pins none.

    A partial pin is an error: the series is applied with all three keys or
    not at all.
    """
    text = (repo / "build-config.env").read_text()
    present = [key for key in SHARED_SERIES_KEYS if re.search(rf"(?m)^{key}=", text)]
    if not present:
        return None
    if len(present) != len(SHARED_SERIES_KEYS):
        missing = sorted(set(SHARED_SERIES_KEYS) - set(present))
        raise ValueError(f"build-config.env pins the shared FFmpeg series without {missing}")
    match = re.search(r'(?m)^FFMPEG_FIX_SERIES_TAG="?([^"\s#]+)"?', text)
    if match is None:
        raise ValueError("FFMPEG_FIX_SERIES_TAG has no value")
    return match.group(1)


def series(repo: Path) -> list[str]:
    directory = repo / "ffmpeg-patches"
    entries = [
        line.partition("#")[0].strip()
        for line in (directory / "series.txt").read_text().splitlines()
    ]
    entries = [entry for entry in entries if entry]
    if not entries or len(entries) != len(set(entries)):
        raise ValueError("series.txt must contain a nonempty, unique patch list")
    for entry in entries:
        path = directory / entry
        if not PATCH_NAME.fullmatch(entry) or not path.is_file() or path.is_symlink():
            raise ValueError(f"invalid or missing series entry: {entry}")
    actual = {path.name for path in directory.glob("*.patch")}
    if actual != set(entries):
        raise ValueError(f"patches outside series.txt: {sorted(actual - set(entries))}")
    return entries


class Replay:
    """Own only a disposable checkout; never clean an existing workspace."""

    def __init__(self, checkout: Path, output: Path):
        self.checkout = checkout
        self.output = output
        # Hooks export repository/index/object-store variables. Even `git -C`
        # cannot override them: none may escape into the disposable checkout.
        self.environment = {
            key: value for key, value in os.environ.items() if not key.startswith("GIT_")
        }
        self.environment.update(
            LC_ALL="C",
            GIT_TERMINAL_PROMPT="0",
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
        )
        # A hook in the caller's Git configuration must not execute here.
        git = shutil.which("git")
        if git is None:
            raise RuntimeError("git is required to replay the FFmpeg patch stack")
        self.prefix = [
            str(Path(git).resolve(strict=True)),
            "-c",
            "core.hooksPath=/dev/null",
            "-c",
            "commit.gpgsign=false",
            "-c",
            "user.name=VMAFx patch refresh",
            "-c",
            "user.email=patch-refresh@localhost",
        ]

    def git(self, *args: str) -> str:
        return self.run_git(["-C", str(self.checkout)], *args)

    def cache_git(self, cache: Path, *args: str, timeout: int = 180) -> str:
        return self.run_git(["--git-dir", str(cache)], *args, timeout=timeout)

    def run_git(self, location: list[str], *args: str, timeout: int = 180) -> str:
        command = [*self.prefix, *location, *args]
        # Fixed Git executable/subcommands and argument lists; no shell expansion.
        result = run_command(
            command,
            allowed_executables=(self.prefix[0],),
            env=self.environment,
            text=True,
            capture_output=True,
            timeout_seconds=timeout,
            max_output_bytes=16 * 1_048_576,
        )
        with (self.output / "replay.log").open("a") as log:
            log.write(f"$ git {' '.join(args)}\n{result.stdout}{result.stderr}")
        if result.returncode:
            if args[0] in {"am", "rebase"}:
                diff = run_command(
                    [*self.prefix, "-C", str(self.checkout), "diff", "--binary"],
                    allowed_executables=(self.prefix[0],),
                    env=self.environment,
                    text=True,
                    capture_output=True,
                    timeout_seconds=30,
                    max_output_bytes=16 * 1_048_576,
                )
                (self.output / "conflict.diff").write_text(diff.stdout)
            raise RuntimeError(
                f"git {args[0]} failed: {result.stderr.strip() or result.stdout.strip()}"
            )
        return result.stdout

    def fetch(self, url: str, ref: str) -> str:
        self.git("fetch", "--no-tags", "--depth=1", "--end-of-options", url, ref)
        return self.git("rev-parse", "FETCH_HEAD^{commit}").strip()

    def apply_shared_series(self, repo: Path) -> None:
        """Apply the pinned shared fix series to the checkout, as every build does."""
        bash = shutil.which("bash")
        if bash is None:
            raise RuntimeError("bash is required to apply the shared FFmpeg fix series")
        result = run_command(
            [bash, str(SHARED_SERIES_SCRIPT), "apply", "--method", "am", str(self.checkout)],
            allowed_executables=(bash,),
            env={**self.environment, "BUILD_CONFIG": str(repo / "build-config.env")},
            text=True,
            capture_output=True,
            timeout_seconds=SHARED_SERIES_SECONDS,
            max_output_bytes=16 * 1_048_576,
        )
        with (self.output / "replay.log").open("a") as log:
            log.write(f"$ {SHARED_SERIES_SCRIPT.name} apply\n{result.stdout}{result.stderr}")
        if result.returncode:
            raise RuntimeError(
                f"shared FFmpeg fix series failed: {result.stderr.strip() or result.stdout.strip()}"
            )


def cache_root() -> Path:
    """Per-user cache directory, chosen as the other repo tools choose theirs."""
    explicit = os.environ.get("XDG_CACHE_HOME")
    if explicit:
        base = Path(explicit)
    elif sys.platform == "win32" and os.environ.get("LOCALAPPDATA"):
        base = Path(os.environ["LOCALAPPDATA"])
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path.home() / ".cache"
    return base / "vmafx" / "ffmpeg-patch-stack"


def _lock_once(handle: IO[bytes], acquire: bool) -> None:
    """Take (or drop) the byte-range lock without blocking; raise OSError when held."""
    if sys.platform == "win32":
        import msvcrt  # noqa: PLC0415 - platform-specific

        handle.seek(0)
        mode = msvcrt.LK_NBLCK if acquire else msvcrt.LK_UNLCK  # type: ignore[attr-defined]
        msvcrt.locking(handle.fileno(), mode, 1)  # type: ignore[attr-defined]
    else:
        import fcntl  # noqa: PLC0415 - platform-specific

        fcntl.flock(handle.fileno(), (fcntl.LOCK_EX | fcntl.LOCK_NB) if acquire else fcntl.LOCK_UN)


@contextmanager
def exclusive_lock(path: Path) -> Iterator[None]:
    """Cross-process lock with a bounded wait; the cache has one writer at a time."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        for _ in range(CACHE_LOCK_SECONDS * 10):
            try:
                _lock_once(handle, True)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError(f"timed out waiting for the cache lock {path}")
        try:
            yield
        finally:
            _lock_once(handle, False)


class SourceCache:
    """Bare repository of fetched release commits, one per remote, keyed by tag.

    A tag is trusted only when its commit equals the pinned FFMPEG_COMMIT; a
    mismatch is an error, never a silent use. Writers hold a lock, and a ref
    moves only after its commit was fetched and verified, so a reader never
    sees a half-written entry.
    """

    def __init__(self, replay: Replay, root: Path, remote: str):
        key = hashlib.sha256(remote.encode()).hexdigest()[:16]
        self.replay = replay
        self.directory = root / key
        self.repo = self.directory / "source.git"
        self.lock = self.directory / "source.lock"
        self.remote = remote

    @staticmethod
    def ref(tag: str) -> str:
        return f"refs/vmafx/tags/{tag}"

    def healthy(self) -> bool:
        try:
            return (
                self.replay.cache_git(self.repo, "rev-parse", "--is-bare-repository").strip()
                == "true"
            )
        except RuntimeError:
            return False

    def reset(self) -> None:
        shutil.rmtree(self.repo, ignore_errors=True)
        self.repo.parent.mkdir(parents=True, exist_ok=True)
        self.replay.run_git(
            ["-C", str(self.repo.parent)], "init", "--quiet", "--bare", "source.git"
        )

    def cached(self, tag: str) -> str | None:
        try:
            return self.replay.cache_git(
                self.repo, "rev-parse", "--verify", "--quiet", f"{self.ref(tag)}^{{commit}}"
            ).strip()
        except RuntimeError:
            return None

    def populate(self, tag: str, expected: str | None) -> str:
        incoming = f"refs/vmafx/incoming/{tag}"
        self.replay.cache_git(
            self.repo,
            "fetch",
            "--no-tags",
            "--depth=1",
            "--end-of-options",
            self.remote,
            f"+refs/tags/{tag}:{incoming}",
            timeout=CACHE_FETCH_SECONDS,
        )
        commit = self.replay.cache_git(self.repo, "rev-parse", f"{incoming}^{{commit}}").strip()
        try:
            if expected is not None and commit != expected:
                raise ValueError(
                    f"{tag} on {self.remote} names commit {commit}, the pinned FFMPEG_COMMIT is "
                    f"{expected}; the tag moved upstream or the pin is stale"
                )
            self.replay.cache_git(self.repo, "update-ref", self.ref(tag), commit)
        finally:
            self.replay.cache_git(self.repo, "update-ref", "-d", incoming)
        return commit

    def obtain(self, tag: str, expected: str | None) -> tuple[str, str]:
        """Return (commit, origin) with the commit present and verified in the cache."""
        with exclusive_lock(self.lock):
            if not self.healthy():
                self.reset()
            commit = self.cached(tag) if expected is not None else None
            if commit is None:
                label = "fetched into cache" if expected is not None else "fetched (unpinned)"
                return self.populate(tag, expected), label
            if commit != expected:
                raise ValueError(
                    f"cached {tag} is commit {commit}, the pinned FFMPEG_COMMIT is {expected}; "
                    f"clear {self.directory} if the pin was bumped on purpose"
                )
            return commit, "cache hit"

    def fetch(self, tag: str, expected: str | None) -> tuple[str, str]:
        """Fetch the tag into the replay checkout from the cache, healing a corrupt entry."""
        commit, origin = self.obtain(tag, expected)
        url = self.repo.resolve().as_uri()
        try:
            fetched = self.replay.fetch(url, self.ref(tag))
        except RuntimeError:
            if origin != "cache hit":
                raise
            with exclusive_lock(self.lock):
                self.reset()
            commit, origin = self.obtain(tag, expected)
            origin = "fetched into cache (corrupt entry dropped)"
            fetched = self.replay.fetch(url, self.ref(tag))
        if fetched != commit:
            raise RuntimeError(f"cache returned {fetched} for {tag}, expected {commit}")
        return commit, origin


def mirrors(repo: Path, remote: str, tag: str) -> dict[Path, bytes]:
    """Generate the unavoidable Docker ARG defaults from the shared owner."""
    changed = {}
    for name in MIRRORS:
        path = repo / name
        original = path.read_text()
        updated = original
        for key, value in {"FFMPEG_TAG": tag, "FFMPEG_REMOTE": remote}.items():
            updated, count = re.subn(rf"(?m)^ARG {key}=\S+$", f"ARG {key}={value}", updated)
            if count != 1:
                raise ValueError(f"expected one {key} mirror in {name}, found {count}")
        changed[path] = updated.encode()
    return changed


def replace_files(updates: dict[Path, bytes]) -> None:
    """Prepare every file first; restore original bytes if a write fails."""
    before = {path: path.read_bytes() for path in updates}
    staged = {}
    backups = {}
    temporaries = []
    written = []
    preserve_backups = False

    def stage(path: Path, content: bytes, role: str) -> Path:
        handle, name = tempfile.mkstemp(
            prefix=f".ffmpeg-refresh-{path.name}-{role}-", dir=path.parent
        )
        temporary = Path(name)
        temporaries.append(temporary)
        with os.fdopen(handle, "wb") as stream:
            stream.write(content)
        temporary.chmod(path.stat().st_mode)
        return temporary

    try:
        for path, content in updates.items():
            if before[path] == content:
                continue
            if path.is_symlink():
                raise ValueError(f"refusing to replace a symlink: {path}")
            staged[path] = stage(path, content, "candidate")
            backups[path] = stage(path, before[path], "original")
        for path, temporary in staged.items():
            temporary.replace(path)
            written.append(path)
    except BaseException as error:
        # Roll back even when Ctrl-C interrupts a multi-file replacement.
        # Keep originals if recovery itself is interrupted a second time.
        preserve_backups = True
        failures = []
        for path in reversed(written):
            try:
                backups[path].replace(path)
            except OSError:
                failures.append(f"{path} <- {backups[path]}")
        if failures:
            raise RuntimeError(
                "refresh rollback failed; original backups retained: " + "; ".join(failures)
            ) from error
        preserve_backups = False
        raise
    finally:
        for temporary in temporaries:
            if preserve_backups and temporary in backups.values():
                continue
            temporary.unlink(missing_ok=True)


class Receipt(TypedDict, total=False):
    shared_series: str
    shared_patch_count: int
    remote: str
    configured_tag: str
    patch_count: int
    applying: str
    tag: str
    upstream_commit: str
    pinned_commit: str
    source: str
    target_source: str
    cache: str
    patched_tree: str
    changed: list[str]
    status: str
    error: str


def select_target_tag(replay: Replay, remote: str, current_tag: str, latest: bool) -> str:
    """Resolve the requested stable tag without permitting a downgrade."""
    if not latest:
        return current_tag
    target_tag = latest_release(replay.git("ls-remote", "--tags", "--refs", remote, "refs/tags/n*"))
    if stable_version(target_tag) < stable_version(current_tag):
        raise ValueError("upstream latest stable release would downgrade the maintained tag")
    return target_tag


def format_patch_updates(
    repo: Path, output: Path, replay: Replay, names: list[str], commits: list[str]
) -> dict[Path, bytes]:
    """Render the replayed commits into candidate patch bytes."""
    updates = {}
    candidate = output / "patches"
    candidate.mkdir(exist_ok=True)
    for name, commit in zip(names, commits, strict=True):
        patch = replay.git(
            "format-patch",
            "-1",
            "--stdout",
            "--zero-commit",
            "--no-signature",
            "--no-numbered",
            "--full-index",
            commit,
        ).encode()
        updates[repo / "ffmpeg-patches" / name] = patch
        (candidate / name).write_bytes(patch)
    return updates


def pinned_config(text: str, tag: str, commit: str) -> str:
    """Move the tag and the commit it names together."""
    text = re.sub(r"(?m)^FFMPEG_TAG=.*$", f'FFMPEG_TAG="{tag}"', text)
    return re.sub(r"(?m)^FFMPEG_COMMIT=.*$", f'FFMPEG_COMMIT="{commit}"', text)


def apply_stack(
    repo: Path, replay: Replay, base: str, names: list[str], receipt: Receipt
) -> tuple[str | None, int]:
    """Apply the shared fix series, then ours, at *base*; return its tag and patch count."""
    replay.git("switch", "--quiet", "--detach", base)
    shared = shared_series_tag(repo)
    if shared is not None:
        receipt["applying"] = f"shared fix series {shared}"
        replay.apply_shared_series(repo)
    shared_count = len(replay.git("rev-list", f"{base}..HEAD").splitlines())
    receipt.update({"shared_series": shared or "none", "shared_patch_count": shared_count})
    for name in names:
        receipt["applying"] = name
        replay.git("am", "--3way", str(repo / "ffmpeg-patches" / name))
    return shared, shared_count


def unpinned_release(shared: str, target_tag: str, current_tag: str, output: Path) -> str:
    """Why a newer FFmpeg release cannot be taken while the series targets the old one."""
    return (
        f"FFmpeg {target_tag} is released, but the pinned shared fix series {shared} "
        f"targets {current_tag}; the rebased candidates are in {output / 'patches'}. "
        f"Pin a series release for {target_tag} (FFMPEG_FIX_SERIES_TAG and "
        "FFMPEG_FIX_SERIES_SHA256) together with FFMPEG_TAG and FFMPEG_COMMIT, "
        "then run --refresh"
    )


def maintain(
    repo: Path, output: Path, refresh: bool, latest: bool, cache_dir: Path | None = None
) -> Receipt:
    receipt: Receipt = {}
    output.mkdir(parents=True, exist_ok=True)
    (output / "replay.log").write_text("")
    try:
        remote, current_tag, pinned = configuration(repo)
        names = series(repo)
        receipt.update({"remote": remote, "configured_tag": current_tag, "patch_count": len(names)})
        receipt["pinned_commit"] = pinned
        with tempfile.TemporaryDirectory(prefix="vmafx-ffmpeg-") as temporary:
            replay = Replay(Path(temporary), output)
            replay.git("init", "--quiet")
            target_tag = select_target_tag(replay, remote, current_tag, latest)
            cache = SourceCache(replay, cache_dir or cache_root(), remote)
            receipt["cache"] = str(cache.repo)
            base, receipt["source"] = cache.fetch(current_tag, pinned)
            shared, shared_count = apply_stack(repo, replay, base, names, receipt)
            target = base
            if target_tag != current_tag:
                target, receipt["target_source"] = cache.fetch(target_tag, None)
                replay.git("rebase", "--onto", target, base)
            receipt.update(
                {
                    "tag": target_tag,
                    "upstream_commit": target,
                    "patched_tree": replay.git("rev-parse", "HEAD^{tree}").strip(),
                }
            )
            commits = replay.git("rev-list", "--reverse", f"{target}..HEAD").splitlines()
            if len(commits) != shared_count + len(names):
                raise ValueError(
                    "refresh changed the number of patches; review upstreamed/empty patches"
                )
            # The shared series' commits come first and are not ours to render.
            updates = format_patch_updates(repo, output, replay, names, commits[shared_count:])
            if shared is not None and target_tag != current_tag:
                raise ValueError(unpinned_release(shared, target_tag, current_tag, output))
            updates.update(mirrors(repo, remote, target_tag))
            config = repo / "build-config.env"
            updates[config] = pinned_config(config.read_text(), target_tag, target).encode()
            changed = [
                str(path.relative_to(repo))
                for path, data in updates.items()
                if path.read_bytes() != data
            ]
            receipt.update({"changed": changed, "status": "refreshed" if refresh else "checked"})
            if refresh:
                replace_files(updates)
            elif changed:
                raise ValueError(
                    "patch/configuration drift; run python3 scripts/ci/ffmpeg_patch_stack.py --refresh"
                )
    except (KeyError, ValueError, RuntimeError, OSError, CommandTimedOut) as error:
        receipt.update({"status": "failed", "error": str(error)})
        raise
    finally:
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="fail on stale patch or configuration mirrors (default)",
    )
    mode.add_argument(
        "--refresh",
        action="store_true",
        help="regenerate patches and configuration mirrors after complete replay",
    )
    parser.add_argument(
        "--latest",
        action="store_true",
        help="refresh onto the latest stable release, for scheduled updates",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        help="FFmpeg source cache (default: <user cache>/vmafx/ffmpeg-patch-stack)",
    )
    parser.add_argument(
        "--output-dir", type=Path, help="retain logs, candidate patches and exact upstream receipt"
    )
    args = parser.parse_args()
    if args.latest and not args.refresh:
        parser.error("--latest requires --refresh")
    repo = Path(__file__).resolve().parents[2]
    output = args.output_dir or Path(tempfile.mkdtemp(prefix="vmafx-ffmpeg-report-"))
    try:
        result = maintain(repo, output.resolve(), args.refresh, args.latest, args.cache_dir)
    except (KeyError, ValueError, RuntimeError, OSError, CommandTimedOut) as error:
        print(f"FFmpeg patch stack FAILED: {error}\nDiagnostics: {output}")
        return 1
    print(
        f"FFmpeg {result['tag']} ({result['upstream_commit']}): {result['patch_count']} patches "
        f"{result['status']} on shared fix series {result['shared_series']} "
        f"({result['shared_patch_count']} patches); source: {result['source']}"
        + (f", {result['target_source']}" if "target_source" in result else "")
        + f" [{result['cache']}]"
    )
    if args.output_dir:
        print(f"Diagnostics: {output}")
    else:
        shutil.rmtree(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

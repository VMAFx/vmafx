#!/usr/bin/env python3
"""Replay and refresh the ordered FFmpeg series against stable release tags."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import sys
import tempfile
from pathlib import Path
from typing import TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

STABLE_TAG = re.compile(r"n(\d+)\.(\d+)(?:\.(\d+))?\Z")
PATCH_NAME = re.compile(r"\d{4}-[A-Za-z0-9_.-]+\.patch\Z")
MIRRORS = ("Dockerfile", "Dockerfile.ffmpeg", "dev/Containerfile", "docker/Dockerfile.node")


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


def configuration(repo: Path) -> tuple[str, str]:
    values = {}
    for line in (repo / "build-config.env").read_text().splitlines():
        key, sep, value = line.partition("=")
        if sep and key in {"FFMPEG_REMOTE", "FFMPEG_TAG"}:
            parts = shlex.split(value, comments=True)
            if len(parts) != 1 or key in values:
                raise ValueError(f"invalid or duplicate build-config.env {key}")
            values[key] = parts[0]
    remote, tag = values["FFMPEG_REMOTE"], values["FFMPEG_TAG"]
    stable_version(tag)
    return remote, tag


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
        command = [*self.prefix, "-C", str(self.checkout), *args]
        # Fixed Git executable/subcommands and argument lists; no shell expansion.
        result = run_command(
            command,
            allowed_executables=(self.prefix[0],),
            env=self.environment,
            text=True,
            capture_output=True,
            timeout_seconds=180,
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

    def fetch(self, remote: str, tag: str) -> str:
        self.git(
            "fetch",
            "--no-tags",
            "--depth=1",
            "--end-of-options",
            remote,
            f"refs/tags/{tag}",
        )
        return self.git("rev-parse", "FETCH_HEAD^{commit}").strip()


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
    remote: str
    configured_tag: str
    patch_count: int
    applying: str
    tag: str
    upstream_commit: str
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


def maintain(repo: Path, output: Path, refresh: bool, latest: bool) -> Receipt:
    receipt: Receipt = {}
    output.mkdir(parents=True, exist_ok=True)
    (output / "replay.log").write_text("")
    try:
        remote, current_tag = configuration(repo)
        names = series(repo)
        receipt.update({"remote": remote, "configured_tag": current_tag, "patch_count": len(names)})
        with tempfile.TemporaryDirectory(prefix="vmafx-ffmpeg-") as temporary:
            replay = Replay(Path(temporary), output)
            replay.git("init", "--quiet")
            target_tag = select_target_tag(replay, remote, current_tag, latest)
            base = replay.fetch(remote, current_tag)
            replay.git("switch", "--quiet", "--detach", base)
            for name in names:
                receipt["applying"] = name
                replay.git("am", "--3way", str(repo / "ffmpeg-patches" / name))
            target = base
            if target_tag != current_tag:
                target = replay.fetch(remote, target_tag)
                replay.git("rebase", "--onto", target, base)
            receipt.update(
                {
                    "tag": target_tag,
                    "upstream_commit": target,
                    "patched_tree": replay.git("rev-parse", "HEAD^{tree}").strip(),
                }
            )
            commits = replay.git("rev-list", "--reverse", f"{target}..HEAD").splitlines()
            if len(commits) != len(names):
                raise ValueError(
                    "refresh changed the number of patches; review upstreamed/empty patches"
                )
            updates = format_patch_updates(repo, output, replay, names, commits)
            updates.update(mirrors(repo, remote, target_tag))
            config = repo / "build-config.env"
            updates[config] = re.sub(
                r"(?m)^FFMPEG_TAG=.*$", f'FFMPEG_TAG="{target_tag}"', config.read_text()
            ).encode()
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
        "--output-dir", type=Path, help="retain logs, candidate patches and exact upstream receipt"
    )
    args = parser.parse_args()
    if args.latest and not args.refresh:
        parser.error("--latest requires --refresh")
    repo = Path(__file__).resolve().parents[2]
    output = args.output_dir or Path(tempfile.mkdtemp(prefix="vmafx-ffmpeg-report-"))
    try:
        result = maintain(repo, output.resolve(), args.refresh, args.latest)
    except (KeyError, ValueError, RuntimeError, OSError, CommandTimedOut) as error:
        print(f"FFmpeg patch stack FAILED: {error}\nDiagnostics: {output}")
        return 1
    print(
        f"FFmpeg {result['tag']} ({result['upstream_commit']}): {result['patch_count']} patches {result['status']}"
    )
    if args.output_dir:
        print(f"Diagnostics: {output}")
    else:
        shutil.rmtree(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Rebase and test open PR branches against upstream Netflix/vmaf.

Inspect, validate, rebase, build, test, and push open PR branches from our
fork against upstream/master.

Usage:
    scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --prs prs.txt
    scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --discover --apply
    scripts/dev/upstream_pr_sweep.py --fork VMAFx/vmafx --discover --apply --push
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

MIN_PR_PARTS = 2
MESON_TEST_RUNNER = Path(__file__).resolve().parents[2] / "scripts" / "ci" / "run_meson_test.py"


@dataclass(frozen=True)
class PRTarget:
    number: int
    branch: str
    head_owner: str


@dataclass(frozen=True)
class SweepRow:
    number: int
    branch: str
    status: str
    old_sha: str
    new_sha: str
    unsigned: int
    detail: str

    def to_tsv(self) -> str:
        old_short = self.old_sha[:9] if self.old_sha and self.old_sha != "-" else "-"
        new_short = self.new_sha[:9] if self.new_sha and self.new_sha != "-" else "-"
        return (
            f"{self.number}\t{self.branch}\t{self.status}\t"
            f"{old_short}\t{new_short}\t{self.unsigned}\t{self.detail}"
        )


def parse_pr_file(path: Path) -> list[PRTarget]:
    targets: list[PRTarget] = []
    if not path.is_file():
        return targets
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < MIN_PR_PARTS:
            continue
        number = int(parts[0])
        branch = parts[1]
        head_owner = parts[2] if len(parts) > MIN_PR_PARTS else ""
        targets.append(PRTarget(number=number, branch=branch, head_owner=head_owner))
    return targets


def discover_prs(repo: Path) -> list[PRTarget]:
    gh_bin = shutil.which("gh")
    if gh_bin is None:
        raise RuntimeError("gh executable not found")
    result = run_command(
        [
            gh_bin,
            "pr",
            "list",
            "-R",
            "Netflix/vmaf",
            "--author",
            "@me",
            "--state",
            "open",
            "--json",
            "number,headRefName,headRepositoryOwner",
        ],
        allowed_executables=(gh_bin,),
        env=git_environment(),
        cwd=repo,
        capture_output=True,
        text=True,
        timeout_seconds=60.0,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh pr list failed: {result.stderr.strip()}")
    data = json.loads(result.stdout)
    targets: list[PRTarget] = []
    for item in data:
        owner = item.get("headRepositoryOwner")
        owner_name = owner.get("login", "") if isinstance(owner, dict) else str(owner or "")
        targets.append(
            PRTarget(
                number=int(item["number"]),
                branch=str(item["headRefName"]),
                head_owner=owner_name,
            )
        )
    return targets


# Variables that carry the operator's settings, not a repository location. Everything else
# that starts with GIT_ (GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE, GIT_OBJECT_DIRECTORY, ...)
# is dropped: a git hook exports them, and a git started with them acts on the hook's
# repository instead of the one in cwd (the core.bare leak of 2026-11-03).
GIT_SETTING_PREFIXES = ("GIT_CONFIG_", "GIT_AUTHOR_", "GIT_COMMITTER_", "GIT_SSH")
# GIT_CONFIG_PARAMETERS holds the caller's `git -c` overrides (core.bare, core.worktree,
# include.path); a child git would apply them to every target worktree. The numbered
# GIT_CONFIG_COUNT / _KEY_n / _VALUE_n settings stay: they are set on purpose.
GIT_DROPPED_SETTINGS = frozenset({"GIT_CONFIG_PARAMETERS"})


def git_environment() -> dict[str, str]:
    """Return the process environment without any variable that locates a repository."""
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
        or (key.startswith(GIT_SETTING_PREFIXES) and key not in GIT_DROPPED_SETTINGS)
    }


def git_cmd(cwd: Path, args: Sequence[str], *, timeout: float = 120.0) -> tuple[int, str, str]:
    git_bin = shutil.which("git")
    if git_bin is None:
        return 1, "", "git executable not found"
    result = run_command(
        [git_bin, *args],
        allowed_executables=(git_bin,),
        cwd=cwd,
        env=git_environment(),
        capture_output=True,
        text=True,
        timeout_seconds=timeout,
    )
    return result.returncode, result.stdout, result.stderr


def pr_worktree_path(worktree_root: Path, number: int) -> Path:
    candidates = (
        worktree_root / f"netflix-vmaf-contributions-{number}",
        worktree_root / f"pr-{number}",
        worktree_root / str(number),
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def check_worktree_clean(wt: Path) -> tuple[bool, str]:
    if not wt.exists():
        return True, ""
    rc, out, err = git_cmd(wt, ["status", "--porcelain"])
    if rc != 0:
        return False, f"git status failed: {err.strip()}"
    if out.strip():
        return False, "worktree is dirty"
    return True, ""


def check_origin_url(repo: Path, fork: str) -> tuple[bool, str]:
    rc, out, err = git_cmd(repo, ["remote", "get-url", "origin"])
    if rc != 0:
        return False, f"cannot determine origin URL: {err.strip()}"
    url = out.strip()
    if fork not in url:
        return False, f"origin remote URL '{url}' does not name fork '{fork}'"
    return True, url


def resolve_old_sha(repo: Path, branch: str) -> str | None:
    # Only the remote-tracking tip: a local branch of the same name is no proof of what the
    # fork holds, and the push lease must bind to the fork's tip.
    for ref in (f"origin/{branch}", f"refs/remotes/origin/{branch}"):
        rc, out, _ = git_cmd(repo, ["rev-parse", ref])
        if rc == 0 and out.strip():
            return out.strip()
    return None


def resolve_base_sha(repo: Path, upstream_ref: str) -> str | None:
    for ref in (upstream_ref, f"refs/remotes/{upstream_ref}", f"refs/heads/{upstream_ref}"):
        rc, out, _ = git_cmd(repo, ["rev-parse", ref])
        if rc == 0 and out.strip():
            return out.strip()
    return None


def prepare_worktree(repo: Path, wt: Path, branch: str, old_sha: str) -> tuple[bool, str]:
    if not wt.exists():
        rc, _, err = git_cmd(repo, ["worktree", "add", "--detach", str(wt), old_sha])
        if rc != 0:
            return False, f"worktree add failed: {err.strip()}"
    rc, _, err = git_cmd(wt, ["checkout", "-B", branch, old_sha])
    if rc != 0:
        return False, f"checkout -B failed: {err.strip()}"
    return True, ""


def run_rebase(wt: Path, base_sha: str) -> tuple[str | None, str]:
    """Rebase onto base_sha; return (None, "") or (CONFLICT | REBASE-FAIL, detail)."""
    rc, _, err = git_cmd(
        wt,
        ["-c", "commit.gpgsign=true", "rebase", "--gpg-sign", base_sha],
        timeout=180.0,
    )
    if rc == 0:
        return None, ""
    _, diff_out, _ = git_cmd(wt, ["diff", "--name-only", "--diff-filter=U"])
    conflict_files = ",".join(line.strip() for line in diff_out.splitlines() if line.strip())
    git_cmd(wt, ["rebase", "--abort"])
    if conflict_files:
        return "CONFLICT", f"files={conflict_files}"
    # A signing failure, a refused hook or a bad base also stops a rebase; none is a conflict.
    first_line = next((line.strip() for line in err.splitlines() if line.strip()), "")
    return "REBASE-FAIL", first_line or f"git rebase exited {rc}"


def count_unsigned(wt: Path, base_sha: str) -> int:
    rc, out, _ = git_cmd(wt, ["log", "--format=%G?", f"{base_sha}..HEAD"])
    if rc != 0:
        return 1
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    return sum(1 for line in lines if line != "G")


def _run_meson_setup(vmaf_dir: Path, log_path: Path, is_cuda: bool) -> tuple[bool, str]:
    meson_bin = shutil.which("meson")
    if meson_bin is None:
        return False, "meson executable not found"
    cuda_val = "true" if is_cuda else "false"
    with log_path.open("wb") as log_file:
        res = run_command(
            [meson_bin, "setup", "build", f"-Denable_cuda={cuda_val}", f"-Denable_nvcc={cuda_val}"],
            allowed_executables=(meson_bin,),
            env=git_environment(),
            cwd=vmaf_dir,
            stdout=log_file,
            stderr_to_stdout=True,
            timeout_seconds=300.0,
        )
    if res.returncode != 0:
        lines = [
            ln.strip() for ln in log_path.read_text(errors="replace").splitlines() if ln.strip()
        ]
        last_line = lines[-1] if lines else "unknown setup error"
        return False, f"setup: {last_line}"
    return True, ""


def _run_ninja(vmaf_dir: Path, log_path: Path, jobs: int) -> tuple[bool, str]:
    ninja_bin = shutil.which("ninja")
    if ninja_bin is None:
        return False, "ninja executable not found"
    with log_path.open("wb") as log_file:
        res = run_command(
            [ninja_bin, "-C", "build", f"-j{jobs}"],
            allowed_executables=(ninja_bin,),
            env=git_environment(),
            cwd=vmaf_dir,
            stdout=log_file,
            stderr_to_stdout=True,
            timeout_seconds=600.0,
        )
    if res.returncode != 0:
        lines = [
            ln.strip() for ln in log_path.read_text(errors="replace").splitlines() if ln.strip()
        ]
        error_lines = [ln for ln in lines if "error" in ln.lower()]
        detail = error_lines[0] if error_lines else (lines[-1] if lines else "unknown build error")
        return False, f"build: {detail}"
    return True, ""


def _run_meson_test(vmaf_dir: Path, log_path: Path, jobs: int) -> tuple[bool, str]:
    meson_bin = shutil.which("meson")
    if meson_bin is None:
        return False, "meson executable not found"
    with log_path.open("wb") as log_file:
        # The secret-stripping runner (ADR-1333) comes from this checkout:
        # upstream pull request trees do not carry it.
        res = run_command(
            [
                sys.executable,
                str(MESON_TEST_RUNNER),
                "--meson-executable",
                meson_bin,
                "--",
                "-C",
                "build",
                "--num-processes",
                str(jobs),
            ],
            allowed_executables=(sys.executable,),
            env=git_environment(),
            cwd=vmaf_dir,
            stdout=log_file,
            stderr_to_stdout=True,
            timeout_seconds=600.0,
        )
    if res.returncode != 0:
        return False, f"test rc={res.returncode}"
    return True, ""


def build_and_test(
    wt: Path, number: int, branch: str, cuda_prefix: str, jobs: int
) -> tuple[str, str]:
    vmaf_dir = wt / "libvmaf" if (wt / "libvmaf").is_dir() else wt
    if not (vmaf_dir / "meson.build").is_file():
        return "BUILD-FAIL", f"meson.build not found in {vmaf_dir}"
    build_dir = vmaf_dir / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    is_cuda = branch.startswith(cuda_prefix)
    log_dir = wt.parent
    ok, detail = _run_meson_setup(vmaf_dir, log_dir / f"{number}-setup.log", is_cuda)
    if not ok:
        return "BUILD-FAIL", detail
    ok, detail = _run_ninja(vmaf_dir, log_dir / f"{number}-build.log", jobs)
    if not ok:
        return "BUILD-FAIL", detail
    ok, detail = _run_meson_test(vmaf_dir, log_dir / f"{number}-test.log", jobs)
    if not ok:
        return "TEST-FAIL", detail
    return "OK", "build=ok test=ok"


def push_branch(wt: Path, branch: str, old_sha: str) -> tuple[bool, str]:
    refspec = f"HEAD:refs/heads/{branch}"
    lease = f"--force-with-lease=refs/heads/{branch}:{old_sha}"
    rc, out, err = git_cmd(wt, ["push", lease, "origin", refspec], timeout=60.0)
    if rc != 0:
        return False, f"push failed: {err.strip() or out.strip()}"
    return True, "pushed"


def _validate_pr_preconditions(
    target: PRTarget,
    repo: Path,
    worktree_root: Path,
    fork: str,
) -> tuple[SweepRow | None, Path | None, str | None]:
    fork_owner = fork.split("/", maxsplit=1)[0]
    if target.head_owner != fork_owner:
        row = SweepRow(
            target.number,
            target.branch,
            "REFUSED",
            "-",
            "-",
            0,
            f"not ours: head_owner '{target.head_owner}' != '{fork_owner}'",
        )
        return row, None, None
    url_ok, url_detail = check_origin_url(repo, fork)
    if not url_ok:
        return (
            SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, url_detail),
            None,
            None,
        )
    old_sha = resolve_old_sha(repo, target.branch)
    if old_sha is None:
        detail = f"cannot resolve origin/{target.branch}"
        return (
            SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, detail),
            None,
            None,
        )
    wt = pr_worktree_path(worktree_root, target.number)
    clean, clean_detail = check_worktree_clean(wt)
    if not clean:
        return (
            SweepRow(target.number, target.branch, "REFUSED", old_sha, "-", 0, clean_detail),
            None,
            None,
        )
    return None, wt, old_sha


def _execute_rebase(
    repo: Path,
    wt: Path,
    target: PRTarget,
    old_sha: str,
    base_sha: str,
) -> tuple[SweepRow | None, str | None]:
    prep_ok, prep_detail = prepare_worktree(repo, wt, target.branch, old_sha)
    if not prep_ok:
        return (
            SweepRow(target.number, target.branch, "REFUSED", old_sha, "-", 0, prep_detail),
            None,
        )
    rebase_status, rebase_detail = run_rebase(wt, base_sha)
    if rebase_status is not None:
        return (
            SweepRow(target.number, target.branch, rebase_status, old_sha, "-", 0, rebase_detail),
            None,
        )
    _, head_out, _ = git_cmd(wt, ["rev-parse", "HEAD"])
    new_sha = head_out.strip()
    unsigned = count_unsigned(wt, base_sha)
    if unsigned > 0:
        row = SweepRow(
            target.number,
            target.branch,
            "UNSIGNED",
            old_sha,
            new_sha,
            unsigned,
            "unsigned commits",
        )
        return row, None
    return None, new_sha


def _execute_build_and_push(
    wt: Path,
    target: PRTarget,
    old_sha: str,
    new_sha: str,
    *,
    build: bool,
    push: bool,
    cuda_prefix: str,
    jobs: int,
) -> SweepRow:
    if build:
        status, detail = build_and_test(wt, target.number, target.branch, cuda_prefix, jobs)
        if status != "OK":
            return SweepRow(target.number, target.branch, status, old_sha, new_sha, 0, detail)
    if push:
        push_ok, push_detail = push_branch(wt, target.branch, old_sha)
        if not push_ok:
            return SweepRow(
                target.number, target.branch, "REFUSED", old_sha, new_sha, 0, push_detail
            )
        return SweepRow(target.number, target.branch, "OK", old_sha, new_sha, 0, "pushed")
    detail_msg = "build=ok test=ok" if build else "rebased"
    return SweepRow(target.number, target.branch, "OK", old_sha, new_sha, 0, detail_msg)


def process_pr(
    target: PRTarget,
    *,
    repo: Path,
    worktree_root: Path,
    fork: str,
    base_sha: str,
    apply: bool,
    build: bool,
    push: bool,
    cuda_prefix: str,
    jobs: int,
) -> SweepRow:
    refusal, wt, old_sha = _validate_pr_preconditions(target, repo, worktree_root, fork)
    if refusal is not None:
        return refusal
    assert wt is not None and old_sha is not None
    if not apply:
        return SweepRow(target.number, target.branch, "PLANNED", old_sha, "-", 0, "dry-run")
    rebase_failure, new_sha = _execute_rebase(repo, wt, target, old_sha, base_sha)
    if rebase_failure is not None:
        return rebase_failure
    assert new_sha is not None
    return _execute_build_and_push(
        wt,
        target,
        old_sha,
        new_sha,
        build=build,
        push=push,
        cuda_prefix=cuda_prefix,
        jobs=jobs,
    )


def sweep(args: argparse.Namespace) -> list[SweepRow]:
    repo = Path(args.repo).resolve()
    worktree_root = Path(args.worktree_root).resolve()
    targets = parse_pr_file(Path(args.prs)) if args.prs else discover_prs(repo)
    if not targets:
        return []
    base_sha = resolve_base_sha(repo, args.upstream_ref)
    if base_sha is None:
        raise RuntimeError(f"cannot resolve upstream reference: {args.upstream_ref}")
    rows: list[SweepRow] = []
    for target in targets:
        row = process_pr(
            target,
            repo=repo,
            worktree_root=worktree_root,
            fork=args.fork,
            base_sha=base_sha,
            apply=args.apply,
            build=args.build,
            push=args.push,
            cuda_prefix=args.cuda_prefix,
            jobs=args.jobs,
        )
        rows.append(row)
    return rows


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="path to local repository")
    parser.add_argument(
        "--worktree-root",
        type=Path,
        default=Path.cwd().parent,
        help="root directory for PR worktrees",
    )
    parser.add_argument(
        "--fork", required=True, help="fork repository owner/name (e.g. VMAFx/vmafx)"
    )
    pr_group = parser.add_mutually_exclusive_group(required=True)
    pr_group.add_argument("--prs", type=Path, help="file containing PR numbers and branches")
    pr_group.add_argument("--discover", action="store_true", help="discover PRs using gh CLI")
    parser.add_argument(
        "--build", action="store_true", help="configure, build and run the test suite"
    )
    parser.add_argument(
        "--cuda-prefix", default="fix/cuda-", help="branch prefix triggering CUDA build options"
    )
    parser.add_argument("--apply", action="store_true", help="apply rebases, builds, or pushes")
    parser.add_argument("--push", action="store_true", help="push rebased branches with lease")
    parser.add_argument(
        "--jobs", type=int, default=4, help="parallel jobs for the build and the test suite"
    )
    parser.add_argument(
        "--upstream-ref", default="upstream/master", help="upstream reference to rebase onto"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.push and not args.apply:
        print("error: --push requires --apply", file=sys.stderr)
        return 1
    if args.jobs <= 0:
        print("error: --jobs must be positive", file=sys.stderr)
        return 1
    try:
        rows = sweep(args)
    except (RuntimeError, ValueError) as exc:
        print(f"upstream-pr-sweep: error: {exc}", file=sys.stderr)
        return 1
    for row in rows:
        print(row.to_tsv())
    if not rows:
        return 0
    all_ok = all(row.status.upper() in {"OK", "PLANNED"} for row in rows)
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

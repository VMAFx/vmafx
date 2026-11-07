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
from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

try:
    import fcntl
except ImportError:  # pragma: no cover - Windows fallback
    fcntl = None  # type: ignore[assignment]

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.run_meson_test import sanitize_process_environment
from scripts.lib import git_env
from scripts.lib.safe_subprocess import run as run_command

MIN_PR_PARTS = 2
MAX_PR_PARTS = 3
REPO_PATH_SEGMENTS = 2
MAX_PR_FILE_LINES = 10000
DISCOVER_LIMIT = 1000
MAX_DISCOVER_LIMIT = 5000
MAX_WORKTREES = 1024
MAX_PR_COMMITS = 5000
MAX_ORIGIN_URLS = 16
DEFAULT_UPSTREAM_REPO = "Netflix/vmaf"
MESON_TEST_RUNNER = Path(__file__).resolve().parents[2] / "scripts" / "ci" / "run_meson_test.py"

BUILD_ENV_ALLOWLIST = frozenset(
    {
        "PATH",
        "HOME",
        "USER",
        "LOGNAME",
        "SHELL",
        "LANG",
        "TZ",
        "TMPDIR",
        "TEMP",
        "TMP",
        "TERM",
        "CC",
        "CXX",
        "CFLAGS",
        "CXXFLAGS",
        "LDFLAGS",
        "CPPFLAGS",
        "AR",
        "AS",
        "LD",
        "NM",
        "OBJCOPY",
        "OBJDUMP",
        "RANLIB",
        "READELF",
        "STRIP",
        "WINDRES",
        "CUDA_HOME",
        "CUDA_PATH",
        "NVCC",
        "ROCM_PATH",
        "HIP_PATH",
        "PKG_CONFIG",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR",
        "CMAKE_PREFIX_PATH",
        "LIBRARY_PATH",
        "LD_LIBRARY_PATH",
        "DYLD_LIBRARY_PATH",
        "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH",
        "CCACHE_DIR",
        "CCACHE_TEMPDIR",
        "SOURCE_DATE_EPOCH",
        "PYTHONUTF8",
        "PYTHONIOENCODING",
        "PYTHONPATH",
        "VIRTUAL_ENV",
        "SYSTEMROOT",
        "WINDIR",
        "COMSPEC",
        "PATHEXT",
        "APPDATA",
        "LOCALAPPDATA",
        "USERPROFILE",
        "PROGRAMFILES",
        "PROGRAMFILES(X86)",
        "COMMONPROGRAMFILES",
        "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE",
        "VCTOOLSINSTALLDIR",
        "WINDOWSSDKDIR",
        "INCLUDE",
        "LIB",
        "LIBPATH",
    }
)
BUILD_ENV_PREFIXES = ("LC_", "MESON_", "NINJA_", "XDG_")
SECRET_KEY_MARKERS = (
    "TOKEN",
    "SECRET",
    "PASSWORD",
    "PASSWD",
    "CREDENTIAL",
    "API_KEY",
    "PRIVATE_KEY",
    "ASKPASS",
    "AUTH",
)
SECRET_KEY_PREFIXES = ("AWS_", "GITHUB_", "GH_", "SSH_", "GPG_", "GIT_", "AZURE_", "GCP_")


def _escape_tsv_field(value: str) -> str:
    return (
        value.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")
    )


@dataclass(frozen=True)
class PRTarget:
    number: int
    branch: str
    head_owner: str
    head_repo: str = ""


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
            f"{self.number}\t{_escape_tsv_field(self.branch)}\t{_escape_tsv_field(self.status)}\t"
            f"{_escape_tsv_field(old_short)}\t{_escape_tsv_field(new_short)}\t"
            f"{self.unsigned}\t{_escape_tsv_field(self.detail)}"
        )


def _parse_pr_row(path: Path, lineno: int, line: str, default_owner: str) -> PRTarget:
    parts = line.split()
    if len(parts) < MIN_PR_PARTS or len(parts) > MAX_PR_PARTS:
        raise ValueError(
            f"{path}:{lineno}: expected '<number> <branch> [head_owner]', got {line!r}"
        )
    try:
        number = int(parts[0])
    except ValueError as exc:
        raise ValueError(f"{path}:{lineno}: invalid PR number {parts[0]!r}") from exc
    if number <= 0:
        raise ValueError(f"{path}:{lineno}: PR number must be positive: {number}")
    branch = parts[1]
    raw_owner = parts[2] if len(parts) > MIN_PR_PARTS else default_owner
    if "/" in raw_owner:
        head_owner, _ = raw_owner.split("/", maxsplit=1)
        return PRTarget(number=number, branch=branch, head_owner=head_owner, head_repo=raw_owner)
    return PRTarget(number=number, branch=branch, head_owner=raw_owner, head_repo="")


def parse_pr_file(path: Path, default_owner: str = "") -> list[PRTarget]:
    if not path.is_file():
        raise RuntimeError(f"PR file not found: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) > MAX_PR_FILE_LINES:
        raise ValueError(f"PR file exceeds {MAX_PR_FILE_LINES} lines: {path}")
    targets: list[PRTarget] = []
    for lineno, raw in enumerate(lines[:MAX_PR_FILE_LINES], start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        targets.append(_parse_pr_row(path, lineno, line, default_owner))
    return targets


def _target_from_gh_item(item: dict[str, object]) -> PRTarget:
    owner = item.get("headRepositoryOwner")
    owner_name = owner.get("login", "") if isinstance(owner, dict) else str(owner or "")
    head_repo = ""
    if "headRepository" in item:
        repo_obj = item.get("headRepository")
        repo_name = repo_obj.get("name", "") if isinstance(repo_obj, dict) else str(repo_obj or "")
        head_repo = f"{owner_name}/{repo_name}" if (owner_name and repo_name) else "<unknown>"
    return PRTarget(
        number=int(str(item["number"])),
        branch=str(item["headRefName"]),
        head_owner=owner_name,
        head_repo=head_repo,
    )


def discover_prs(
    repo: Path,
    *,
    upstream_repo: str = DEFAULT_UPSTREAM_REPO,
    limit: int = DISCOVER_LIMIT,
) -> list[PRTarget]:
    if limit <= 0 or limit > MAX_DISCOVER_LIMIT:
        raise ValueError(f"discovery limit must be between 1 and {MAX_DISCOVER_LIMIT}")
    gh_bin = shutil.which("gh")
    if gh_bin is None:
        raise RuntimeError("gh executable not found")
    result = run_command(
        [
            gh_bin,
            "pr",
            "list",
            "-R",
            upstream_repo,
            "--author",
            "@me",
            "--state",
            "open",
            "--limit",
            str(limit),
            "--json",
            "number,headRefName,headRepositoryOwner,headRepository",
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
    if len(data) >= limit:
        raise RuntimeError(
            f"gh pr list returned {len(data)} PRs (limit {limit}); refusing truncated discovery"
        )
    return [_target_from_gh_item(item) for item in data[:limit]]


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
        for key, value in git_env.drop().items()
        if key.startswith(GIT_SETTING_PREFIXES) and key not in GIT_DROPPED_SETTINGS
    }


def _is_allowed_build_var(key: str) -> bool:
    upper = key.upper()
    if any(marker in upper for marker in SECRET_KEY_MARKERS):
        return False
    if upper.startswith(SECRET_KEY_PREFIXES):
        return False
    return upper in BUILD_ENV_ALLOWLIST or upper.startswith(BUILD_ENV_PREFIXES)


def build_environment() -> dict[str, str]:
    """Return a minimal build environment stripped of caller credentials."""
    raw_env = git_environment()
    sanitize_process_environment(raw_env)
    return {key: value for key, value in raw_env.items() if _is_allowed_build_var(key)}


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


@contextmanager
def worktree_lock(wt: Path) -> Iterator[tuple[bool, str]]:
    """Serialize operations on one PR worktree across concurrent sweep processes."""
    wt.parent.mkdir(parents=True, exist_ok=True)
    lock_path = wt.parent / f"{wt.name}.lock"
    if fcntl is None:
        try:
            fd = os.open(str(lock_path), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        except OSError:
            yield False, f"worktree lock busy: {lock_path.name}"
            return
        try:
            yield True, ""
        finally:
            os.close(fd)
            lock_path.unlink(missing_ok=True)
        return

    fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR, 0o600)
    locked = False
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked = True
        except OSError:
            yield False, f"worktree lock busy: {lock_path.name}"
            return
        yield True, ""
    finally:
        if locked:
            fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def registered_worktrees(repo: Path) -> tuple[bool, set[Path], str]:
    rc, out, err = git_cmd(repo, ["worktree", "list", "--porcelain"])
    if rc != 0:
        return False, set(), f"git worktree list failed: {err.strip()}"
    repo_resolved = repo.resolve()
    paths: set[Path] = set()
    for line in out.splitlines()[:MAX_WORKTREES]:
        if line.startswith("worktree "):
            candidate = Path(line[len("worktree ") :].strip()).resolve()
            if candidate != repo_resolved:
                paths.add(candidate)
    return True, paths, ""


def check_worktree_clean(wt: Path, repo: Path | None = None) -> tuple[bool, str]:
    if not wt.exists():
        return True, ""
    if repo is not None:
        ok, reg_paths, err_msg = registered_worktrees(repo)
        if not ok:
            return False, err_msg
        if wt.resolve() not in reg_paths:
            return False, f"worktree '{wt}' is not a registered worktree of '{repo}'"
    rc, out, err = git_cmd(wt, ["status", "--porcelain"])
    if rc != 0:
        return False, f"git status failed: {err.strip()}"
    if out.strip():
        return False, "worktree is dirty"
    return True, ""


def redact_remote_url(url: str) -> str:
    clean = url.strip()
    if "://" in clean:
        parsed = urlsplit(clean)
        host = parsed.hostname or ""
        port = f":{parsed.port}" if parsed.port else ""
        has_userinfo = bool(parsed.username or parsed.password or "@" in parsed.netloc)
        prefix = "<redacted>@" if has_userinfo else ""
        return f"{parsed.scheme}://{prefix}{host}{port}{parsed.path}"
    no_query = clean.split("?", maxsplit=1)[0].split("#", maxsplit=1)[0]
    if "@" in no_query and ":" in no_query.split("@", maxsplit=1)[1]:
        _, rest = no_query.split("@", maxsplit=1)
        return f"<redacted>@{rest}"
    return no_query


def _strip_git_suffix(segment: str) -> str:
    return segment[:-4] if segment.endswith(".git") else segment


def _segments_to_owner_repo(segments: Sequence[str], *, exact: bool) -> str | None:
    if exact and len(segments) != REPO_PATH_SEGMENTS:
        return None
    if not exact and len(segments) < REPO_PATH_SEGMENTS:
        return None
    owner, repo_name = segments[-2], _strip_git_suffix(segments[-1])
    return f"{owner}/{repo_name}" if (owner and repo_name) else None


def _parse_url_with_scheme(url: str) -> str | None:
    parsed = urlsplit(url)
    if parsed.query or parsed.fragment:
        return None
    segments = [seg for seg in parsed.path.split("/") if seg]
    if parsed.scheme in {"http", "https", "ssh", "git"}:
        return _segments_to_owner_repo(segments, exact=True)
    if parsed.scheme == "file":
        return _segments_to_owner_repo(segments, exact=False)
    return None


def parse_remote_repo(url: str) -> str | None:
    clean = url.strip()
    if not clean or ("?" in clean and "://" not in clean) or ("#" in clean and "://" not in clean):
        return None
    if "://" in clean:
        return _parse_url_with_scheme(clean)
    colon_idx = clean.find(":")
    slash_idx = clean.find("/")
    is_scp = (
        not clean.startswith(("/", "./", "../"))
        and colon_idx > 1
        and (slash_idx == -1 or colon_idx < slash_idx)
    )
    if is_scp:
        _, path_part = clean.split(":", maxsplit=1)
        return _segments_to_owner_repo([s for s in path_part.split("/") if s], exact=True)
    segments = [seg for seg in clean.replace("\\", "/").split("/") if seg]
    return _segments_to_owner_repo(segments, exact=False)


def _fetch_origin_urls(repo: Path) -> tuple[bool, list[str], str]:
    rc, fetch_out, err = git_cmd(repo, ["remote", "get-url", "origin"])
    if rc != 0:
        return False, [], f"cannot determine origin URL: {err.strip()}"
    rc_push, push_out, err_push = git_cmd(repo, ["remote", "get-url", "--push", "--all", "origin"])
    if rc_push != 0:
        return False, [], f"cannot determine origin push URL: {err_push.strip()}"
    urls = [u.strip() for u in [fetch_out, *push_out.splitlines()] if u.strip()]
    return True, urls, ""


def check_origin_url(repo: Path, fork: str) -> tuple[bool, str]:
    expected = fork.strip().lower()
    if expected.count("/") != 1 or not all(expected.split("/", maxsplit=1)):
        return False, f"invalid fork specification '{fork}'"
    ok, urls, err = _fetch_origin_urls(repo)
    if not ok:
        return False, err
    if len(urls) > MAX_ORIGIN_URLS:
        return False, f"origin has more than {MAX_ORIGIN_URLS} URLs; refusing"
    if not urls:
        return False, "cannot determine origin URL: empty remote URL"
    for raw_url in urls:
        parsed = parse_remote_repo(raw_url)
        if parsed is None or parsed.lower() != expected:
            safe_url = redact_remote_url(raw_url)
            return False, f"origin remote URL '{safe_url}' does not name fork '{fork}'"
    return True, redact_remote_url(urls[0])


def fetch_remote_ref(repo: Path, remote: str, branch: str) -> tuple[bool, str]:
    refspec = f"+refs/heads/{branch}:refs/remotes/{remote}/{branch}"
    rc, _, err = git_cmd(repo, ["fetch", remote, refspec])
    if rc != 0:
        return False, err.strip() or f"git fetch {remote} {branch} failed"
    return True, ""


def resolve_old_sha(repo: Path, branch: str) -> str | None:
    # Only the fully-qualified remote-tracking tip: a local ref such as
    # refs/heads/origin/<branch> is no proof of what the fork holds.
    rc, out, _ = git_cmd(repo, ["rev-parse", "--verify", f"refs/remotes/origin/{branch}"])
    if rc == 0 and out.strip():
        return out.strip()
    return None


def resolve_base_sha(repo: Path, upstream_ref: str, *, apply: bool = False) -> str | None:
    if apply and "/" in upstream_ref and not upstream_ref.startswith("refs/"):
        remote, branch = upstream_ref.split("/", maxsplit=1)
        rc_remote, _, _ = git_cmd(repo, ["remote", "get-url", remote])
        if rc_remote == 0:
            ok, err = fetch_remote_ref(repo, remote, branch)
            if not ok:
                raise RuntimeError(f"cannot fetch upstream reference {upstream_ref}: {err}")
    for ref in (f"refs/remotes/{upstream_ref}", upstream_ref, f"refs/heads/{upstream_ref}"):
        rc, out, _ = git_cmd(repo, ["rev-parse", "--verify", ref])
        if rc == 0 and out.strip():
            return out.strip()
    return None


def operator_emails(repo: Path) -> set[str]:
    emails: set[str] = set()
    rc, out, _ = git_cmd(repo, ["config", "--get-all", "user.email"])
    if rc == 0:
        for line in out.splitlines()[:64]:
            if line.strip():
                emails.add(line.strip().lower())
    for env_key in ("GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"):
        val = os.environ.get(env_key, "").strip().lower()
        if val:
            emails.add(val)
    return emails


def check_commit_authors(
    repo: Path, base_sha: str, old_sha: str, *, allow_foreign_authors: bool = False
) -> tuple[bool, str]:
    allowed = set() if allow_foreign_authors else operator_emails(repo)
    if not allowed and not allow_foreign_authors:
        return False, "no operator email configured to verify commit authorship"
    rc, out, err = git_cmd(repo, ["log", "--format=%ae%x00%ce", f"{base_sha}..{old_sha}"])
    if rc != 0:
        return False, f"cannot inspect commit authors: {err.strip()}"
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if len(lines) > MAX_PR_COMMITS:
        return False, f"range has more than {MAX_PR_COMMITS} commits; refusing"
    if allow_foreign_authors:
        return True, ""
    for line in lines[:MAX_PR_COMMITS]:
        for email in line.split("\x00")[:2]:
            normalized = email.strip().lower()
            if normalized and normalized not in allowed:
                return False, f"foreign commit identity '{email.strip()}' not in operator emails"
    return True, ""


def prepare_worktree(repo: Path, wt: Path, branch: str, old_sha: str) -> tuple[bool, str]:
    del branch
    if wt.exists():
        ok, reg_paths, err_msg = registered_worktrees(repo)
        if not ok:
            return False, err_msg
        if wt.resolve() not in reg_paths:
            return False, f"worktree '{wt}' is not a registered worktree of '{repo}'"
    else:
        rc, _, err = git_cmd(repo, ["worktree", "add", "--detach", str(wt), old_sha])
        if rc != 0:
            return False, f"worktree add failed: {err.strip()}"
    rc, _, err = git_cmd(wt, ["checkout", "--detach", old_sha])
    if rc != 0:
        return False, f"checkout --detach failed: {err.strip()}"
    return True, ""


def _is_rebase_active(wt: Path, conflict_files: str) -> bool:
    if conflict_files:
        return True
    for state_dir in ("rebase-merge", "rebase-apply"):
        rc, out, _ = git_cmd(wt, ["rev-parse", "--git-path", state_dir])
        if rc == 0 and out.strip():
            path = Path(out.strip())
            resolved = path if path.is_absolute() else (wt / path)
            if resolved.exists():
                return True
    return False


def run_rebase(wt: Path, base_sha: str) -> tuple[str | None, str]:
    """Rebase onto base_sha; return (None, "") or (status, detail)."""
    rc, _, err = git_cmd(
        wt,
        ["-c", "commit.gpgsign=true", "rebase", "--gpg-sign", base_sha],
        timeout=180.0,
    )
    if rc == 0:
        return None, ""
    diff_rc, diff_out, diff_err = git_cmd(wt, ["diff", "--name-only", "--diff-filter=U"])
    conflict_files = (
        ",".join(line.strip() for line in diff_out.splitlines()[:256] if line.strip())
        if diff_rc == 0
        else ""
    )
    active = _is_rebase_active(wt, conflict_files)
    abort_rc, _, abort_err = git_cmd(wt, ["rebase", "--abort"])
    if active and abort_rc != 0:
        abort_detail = abort_err.strip() or f"git rebase --abort exited {abort_rc}"
        return "REBASE-ABORT-FAIL", abort_detail
    if diff_rc != 0:
        diff_detail = diff_err.strip() or f"git diff exited {diff_rc}"
        return "REBASE-FAIL", f"conflict check failed: {diff_detail}"
    if conflict_files:
        return "CONFLICT", f"files={conflict_files}"
    # A signing failure, a refused hook or a bad base also stops a rebase; none is a conflict.
    first_line = next((line.strip() for line in err.splitlines()[:64] if line.strip()), "")
    return "REBASE-FAIL", first_line or f"git rebase exited {rc}"


def count_unsigned(wt: Path, base_sha: str) -> tuple[int, str]:
    rc, out, err = git_cmd(wt, ["log", "--format=%G?", f"{base_sha}..HEAD"])
    if rc != 0:
        detail = (
            f"git log signature verification failed: {err.strip()}"
            if err.strip()
            else "git log signature verification failed"
        )
        return -1, detail
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if len(lines) > MAX_PR_COMMITS:
        return -1, f"range has more than {MAX_PR_COMMITS} commits; refusing"
    return sum(1 for line in lines[:MAX_PR_COMMITS] if line != "G"), ""


def _run_meson_setup(vmaf_dir: Path, log_path: Path, is_cuda: bool) -> tuple[bool, str]:
    meson_bin = shutil.which("meson")
    if meson_bin is None:
        return False, "meson executable not found"
    cuda_val = "true" if is_cuda else "false"
    with log_path.open("wb") as log_file:
        res = run_command(
            [meson_bin, "setup", "build", f"-Denable_cuda={cuda_val}", f"-Denable_nvcc={cuda_val}"],
            allowed_executables=(meson_bin,),
            env=build_environment(),
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
            env=build_environment(),
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
            env=build_environment(),
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


def _is_object_id(value: str) -> bool:
    """True for a full SHA-1 (40) or SHA-256 (64) hex object id; refs and HEAD are refused."""
    if len(value) not in (40, 64):
        return False
    return all(ch in "0123456789abcdef" for ch in value)


def push_branch(
    wt: Path, branch: str, old_sha: str, new_sha: str, fork: str = ""
) -> tuple[bool, str]:
    """Push the verified ``new_sha``, never the symbolic HEAD, under a lease on ``old_sha``."""
    if fork:
        url_ok, url_detail = check_origin_url(wt, fork)
        if not url_ok:
            return False, f"push refused: {url_detail}"
    if not _is_object_id(new_sha):
        return False, "push refused: no verified commit SHA"
    refspec = f"{new_sha}:refs/heads/{branch}"
    lease = f"--force-with-lease=refs/heads/{branch}:{old_sha}"
    rc, out, err = git_cmd(wt, ["push", lease, "origin", refspec], timeout=60.0)
    if rc != 0:
        return False, f"push failed: {err.strip() or out.strip()}"
    return True, "pushed"


def _check_target_ownership(target: PRTarget, fork: str) -> str | None:
    fork_owner = fork.split("/", maxsplit=1)[0]
    effective_owner = target.head_owner or fork_owner
    if effective_owner.lower() != fork_owner.lower():
        return f"not ours: head_owner '{target.head_owner}' != '{fork_owner}'"
    if target.head_repo and target.head_repo.lower() != fork.strip().lower():
        return f"not ours: head_repo '{target.head_repo}' != '{fork}'"
    return None


def _check_target_and_repo_origin(target: PRTarget, repo: Path, fork: str) -> str | None:
    owner_err = _check_target_ownership(target, fork)
    if owner_err is not None:
        return owner_err
    url_ok, url_detail = check_origin_url(repo, fork)
    return None if url_ok else url_detail


def _check_existing_worktree(wt: Path, repo: Path, fork: str) -> str | None:
    clean, clean_detail = check_worktree_clean(wt, repo)
    if not clean:
        return clean_detail
    if wt.exists():
        wt_url_ok, wt_url_detail = check_origin_url(wt, fork)
        if not wt_url_ok:
            return wt_url_detail
    return None


def _validate_pr_preconditions(
    target: PRTarget,
    repo: Path,
    worktree_root: Path,
    fork: str,
    *,
    apply: bool = False,
) -> tuple[SweepRow | None, Path | None, str | None]:
    pre_err = _check_target_and_repo_origin(target, repo, fork)
    if pre_err is not None:
        return SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, pre_err), None, None
    if apply:
        fetch_ok, fetch_err = fetch_remote_ref(repo, "origin", target.branch)
        if not fetch_ok:
            detail = f"cannot resolve origin/{target.branch}: {fetch_err}"
            return (
                SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, detail),
                None,
                None,
            )
    old_sha = resolve_old_sha(repo, target.branch)
    if old_sha is None:
        detail = f"cannot resolve origin/{target.branch}"
        return SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, detail), None, None
    wt = pr_worktree_path(worktree_root, target.number)
    wt_err = _check_existing_worktree(wt, repo, fork)
    if wt_err is not None:
        return (
            SweepRow(target.number, target.branch, "REFUSED", old_sha, "-", 0, wt_err),
            None,
            None,
        )
    return None, wt, old_sha


def _verify_rebased_head(
    wt: Path, target: PRTarget, old_sha: str, base_sha: str
) -> tuple[SweepRow | None, str | None]:
    head_rc, head_out, head_err = git_cmd(wt, ["rev-parse", "HEAD"])
    new_sha = head_out.strip() if head_rc == 0 else ""
    if head_rc != 0 or not new_sha:
        detail = f"rev-parse HEAD failed: {head_err.strip()}" if head_rc != 0 else "empty HEAD SHA"
        return SweepRow(target.number, target.branch, "REBASE-FAIL", old_sha, "-", 0, detail), None
    unsigned, unsigned_detail = count_unsigned(wt, base_sha)
    if unsigned < 0:
        status = "REFUSED" if "refusing" in unsigned_detail else "REBASE-FAIL"
        return (
            SweepRow(
                target.number,
                target.branch,
                status,
                old_sha,
                new_sha,
                0,
                unsigned_detail,
            ),
            None,
        )
    if unsigned > 0:
        return (
            SweepRow(
                target.number,
                target.branch,
                "UNSIGNED",
                old_sha,
                new_sha,
                unsigned,
                "unsigned commits",
            ),
            None,
        )
    return None, new_sha


def _execute_rebase(
    repo: Path,
    wt: Path,
    target: PRTarget,
    old_sha: str,
    base_sha: str,
    *,
    allow_foreign_authors: bool = False,
) -> tuple[SweepRow | None, str | None]:
    authors_ok, authors_detail = check_commit_authors(
        repo, base_sha, old_sha, allow_foreign_authors=allow_foreign_authors
    )
    if not authors_ok:
        return (
            SweepRow(target.number, target.branch, "REFUSED", old_sha, "-", 0, authors_detail),
            None,
        )
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
    return _verify_rebased_head(wt, target, old_sha, base_sha)


def _execute_build_and_push(
    wt: Path,
    target: PRTarget,
    old_sha: str,
    new_sha: str,
    *,
    fork: str = "",
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
        push_ok, push_detail = push_branch(wt, target.branch, old_sha, new_sha, fork=fork)
        if not push_ok:
            return SweepRow(
                target.number, target.branch, "REFUSED", old_sha, new_sha, 0, push_detail
            )
        return SweepRow(target.number, target.branch, "OK", old_sha, new_sha, 0, "pushed")
    detail_msg = "build=ok test=ok" if build else "rebased"
    return SweepRow(target.number, target.branch, "OK", old_sha, new_sha, 0, detail_msg)


def _apply_pr_changes(
    repo: Path,
    wt: Path,
    target: PRTarget,
    old_sha: str,
    base_sha: str,
    *,
    fork: str,
    build: bool,
    push: bool,
    cuda_prefix: str,
    jobs: int,
    allow_foreign_authors: bool,
) -> SweepRow:
    rebase_failure, new_sha = _execute_rebase(
        repo,
        wt,
        target,
        old_sha,
        base_sha,
        allow_foreign_authors=allow_foreign_authors,
    )
    if rebase_failure is not None:
        return rebase_failure
    if not new_sha:
        return SweepRow(
            target.number,
            target.branch,
            "REBASE-FAIL",
            old_sha,
            "-",
            0,
            "missing rebased HEAD SHA",
        )
    return _execute_build_and_push(
        wt,
        target,
        old_sha,
        new_sha,
        fork=fork,
        build=build,
        push=push,
        cuda_prefix=cuda_prefix,
        jobs=jobs,
    )


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
    allow_foreign_authors: bool = False,
) -> SweepRow:
    candidate_wt = pr_worktree_path(worktree_root, target.number)
    with worktree_lock(candidate_wt) as (locked, lock_detail):
        if not locked:
            return SweepRow(target.number, target.branch, "REFUSED", "-", "-", 0, lock_detail)
        refusal, wt, old_sha = _validate_pr_preconditions(
            target, repo, worktree_root, fork, apply=apply
        )
        if refusal is not None:
            return refusal
        if wt is None or not old_sha:
            return SweepRow(
                target.number,
                target.branch,
                "REFUSED",
                old_sha or "-",
                "-",
                0,
                "missing worktree or pre-rebase SHA",
            )
        if not apply:
            return SweepRow(
                target.number, target.branch, "PLANNED", old_sha, "-", 0, "dry-run (local refs)"
            )
        return _apply_pr_changes(
            repo,
            wt,
            target,
            old_sha,
            base_sha,
            fork=fork,
            build=build,
            push=push,
            cuda_prefix=cuda_prefix,
            jobs=jobs,
            allow_foreign_authors=allow_foreign_authors,
        )


def sweep(args: argparse.Namespace) -> list[SweepRow]:
    repo = Path(args.repo).resolve()
    worktree_root = Path(args.worktree_root).resolve()
    fork_owner = args.fork.split("/", maxsplit=1)[0]
    upstream_repo = getattr(args, "upstream_repo", DEFAULT_UPSTREAM_REPO)
    targets = (
        parse_pr_file(Path(args.prs), default_owner=fork_owner)
        if args.prs
        else discover_prs(repo, upstream_repo=upstream_repo)
    )
    if not targets:
        return []
    base_sha = resolve_base_sha(repo, args.upstream_ref, apply=args.apply)
    if base_sha is None:
        raise RuntimeError(f"cannot resolve upstream reference: {args.upstream_ref}")
    allow_foreign = bool(getattr(args, "allow_foreign_authors", False))
    rows: list[SweepRow] = []
    for target in targets[:MAX_PR_FILE_LINES]:
        try:
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
                allow_foreign_authors=allow_foreign,
            )
        except Exception as exc:
            row = SweepRow(
                target.number,
                target.branch,
                "ERROR",
                "-",
                "-",
                0,
                f"{type(exc).__name__}: {exc}",
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
    parser.add_argument(
        "--upstream-repo",
        default=DEFAULT_UPSTREAM_REPO,
        help="upstream repository owner/name for PR discovery (default: Netflix/vmaf)",
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
        "--allow-foreign-authors",
        action="store_true",
        help="allow rebasing and signing commits authored or committed by non-operator emails",
    )
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
    for row in rows[:MAX_PR_FILE_LINES]:
        print(row.to_tsv())
    if not rows:
        return 0
    all_ok = all(row.status.upper() in {"OK", "PLANNED"} for row in rows[:MAX_PR_FILE_LINES])
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check a local upstream contribution range before publishing it, without network I/O."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, cast

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.dev.upstream_pr_sweep import MAX_PR_COMMITS, count_unsigned, git_cmd
from scripts.lib.safe_subprocess import CommandOutputLimitExceeded

CLASS_FILE = Path(__file__).with_name("upstream_review_classes.json")
MAX_LINES = 100_000
MAX_TEXT_BYTES = 4 * 1_048_576
MAX_CLASSES = 32
MAX_DRAFTS = 32
MAX_DRAFT_IDS = 10_000
MAX_MESSAGE_COLUMNS = 72
MIN_MESSAGE_LINES = 3
MAX_CONTRIBUTION_COMMITS = 3
MIN_ANSWER_LENGTH = 12
SUBJECT_RE = re.compile(r"^[a-z0-9_.+/-]+: \S")
CONVENTIONAL_RE = re.compile(
    r"^(feat|fix|chore|docs|test|tests|ci|build|perf|refactor|style|port|revert)(\([^)]*\))?!?: "
)
TRAILER_RE = re.compile(r"^[A-Za-z0-9-]+: \S")
URL_RE = re.compile(r"https?://\S+", re.IGNORECASE)
COMMIT_ID_RE = re.compile(r"\b[0-9a-f]{7,40}\b")
FORK_RE = re.compile(
    r"VMAFx/|lusoris/vmaf|github\.com/VMAFx|\.workingdir|\.engagement/|SKIP=agent-worktree",
    re.IGNORECASE,
)
ACTION_RUN_RE = re.compile(r"https?://github\.com/([^/\s]+)/[^/\s]+/actions/runs/", re.IGNORECASE)
FORK_PATHS = (
    "docs/research",
    "docs/adr",
    "core",
    ".workingdir",
    ".engagement",
    "sycl",
    "vulkan",
    "metal",
    "mcp-server",
    "ai",
)
SOURCE_PATHS = ("libvmaf/src/", "libvmaf/tools/", "python/vmaf/")
TEST_PATHS = ("libvmaf/test/", "python/test/")
KERNEL_PATHS = (
    "libvmaf/src/feature/x86/",
    "libvmaf/src/feature/arm64/",
    "libvmaf/src/cuda/",
    "libvmaf/src/feature/cuda/",
)


class CommitInfo(Protocol):
    sha: str
    subject: str
    message: str


class Attribution(Protocol):
    LOG_FORMAT: str

    def parse_commits(self, out: str) -> list[CommitInfo]: ...
    def commit_attribution_reason(self, commit: CommitInfo) -> str | None: ...
    def line_attribution_reason(self, line: str, source: str) -> str | None: ...


@dataclass(frozen=True)
class ReviewClass:
    id: str
    slug: str
    mode: str


@dataclass(frozen=True)
class Verdict:
    status: str
    reason: str


@dataclass(frozen=True)
class CheckResult:
    id: str
    slug: str
    verdict: Verdict

    def line(self) -> str:
        text = f"{self.verdict.status} {self.id} {self.slug}: {self.verdict.reason}"
        return text.replace("\r", "\\r").replace("\n", "\\n")


@dataclass(frozen=True)
class Context:
    repo: Path
    base: str
    head: str
    commits: list[CommitInfo]
    paths: list[str]
    diff: list[str]
    drafts: dict[Path, list[str]]
    answers: dict[str, str]
    mechanical_only: str | None
    attribution: Attribution


def bounded_lines(text: str, source: str) -> list[str]:
    if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
        raise ValueError(f"{source}: exceeds {MAX_TEXT_BYTES} bytes")
    lines = text.splitlines()
    if len(lines) > MAX_LINES:
        raise ValueError(f"{source}: exceeds {MAX_LINES} lines")
    return lines


def read_text(path: Path) -> str:
    with path.open("rb") as stream:
        raw = stream.read(MAX_TEXT_BYTES + 1)
    if len(raw) > MAX_TEXT_BYTES:
        raise ValueError(f"{path}: exceeds {MAX_TEXT_BYTES} bytes")
    text = raw.decode("utf-8")
    bounded_lines(text, str(path))
    return text


def load_attribution() -> Attribution:
    name = "_upstream_gate_attribution"
    path = Path(__file__).resolve().parents[1] / "ci" / "check-dco.py"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError("cannot load shared attribution checks")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return cast(Attribution, module)


def load_classes() -> list[ReviewClass]:
    data = json.loads(read_text(CLASS_FILE))
    if (
        not isinstance(data, dict)
        or data.get("schema") != 1
        or data.get("upstream") != "Netflix/vmaf"
    ):
        raise ValueError("invalid upstream review class schema")
    rows = data.get("classes")
    if not isinstance(rows, list) or len(rows) > MAX_CLASSES:
        raise ValueError("invalid or oversized class list")
    classes: list[ReviewClass] = []
    seen: set[str] = set()
    for row in rows[:MAX_CLASSES]:
        if not isinstance(row, dict) or not all(
            isinstance(row.get(key), str) and row[key].strip()
            for key in ("id", "slug", "mode", "check")
        ):
            raise ValueError("invalid class entry")
        cid, mode = row["id"], row["mode"]
        if cid in seen or not re.fullmatch(r"C[0-9]{2}", cid):
            raise ValueError(f"duplicate or invalid class id: {cid}")
        seen.add(cid)
        if type(row.get("hits")) is not int or row["hits"] < 0:
            raise ValueError(f"{cid}: invalid hit count")
        if mode not in {"mechanical", "self-review", "online", "non-actionable"}:
            raise ValueError(f"{cid}: invalid mode")
        if mode == "self-review" and not isinstance(row.get("question"), str):
            raise ValueError(f"{cid}: missing self-review question")
        if mode in {"mechanical", "self-review"} and cid not in CHECKS:
            raise ValueError(f"{cid}: no implementation")
        if cid in CHECKS and mode != CHECKS[cid][0]:
            raise ValueError(f"{cid}: implementation mode mismatch")
        classes.append(ReviewClass(cid, row["slug"], mode))
    missing = CHECKS.keys() - seen
    if missing:
        raise ValueError(f"implemented classes missing: {', '.join(sorted(missing))}")
    return classes


def resolve_commit(repo: Path, revision: str) -> str:
    rc, out, _err = git_cmd(
        repo, ["rev-parse", "--verify", "--end-of-options", f"{revision}^{{commit}}"]
    )
    if rc != 0:
        raise ValueError(f"cannot resolve commit: {revision}")
    return out.strip()


def read_answers(path: Path | None) -> dict[str, str]:
    answers: dict[str, str] = {}
    if path is None or not path.exists():
        return answers
    for lineno, line in enumerate(bounded_lines(read_text(path), str(path)), 1):
        if not line.strip():
            continue
        match = re.fullmatch(r"(C[0-9]{2}):\s*(.*)", line)
        if match is None or match[1] in answers:
            raise ValueError(f"{path}:{lineno}: malformed or duplicate review answer")
        answers[match[1]] = match[2].strip()
    return answers


def read_commits(repo: Path, base: str, head: str, attribution: Attribution) -> list[CommitInfo]:
    rc, out, _err = git_cmd(
        repo, ["rev-list", f"--max-count={MAX_PR_COMMITS + 1}", f"{base}..{head}"]
    )
    shas = bounded_lines(out, "commit list")
    if rc != 0 or len(shas) > MAX_PR_COMMITS:
        raise ValueError(f"cannot read range within {MAX_PR_COMMITS} commits")
    commits: list[CommitInfo] = []
    for sha in shas[:MAX_PR_COMMITS]:
        rc, out, _err = git_cmd(
            repo, ["log", "-z", "-1", f"--format={attribution.LOG_FORMAT}", sha]
        )
        bounded_lines(out, f"commit {sha}")
        if rc != 0:
            raise ValueError(f"cannot read commit {sha}")
        commits.extend(attribution.parse_commits(out))
    return commits


def read_diff(repo: Path, base: str, head: str) -> tuple[list[str], list[str]]:
    rc, names, _err = git_cmd(repo, ["diff", "--no-renames", "--name-only", "-z", base, head, "--"])
    paths = names.rstrip("\0").split("\0") if names else []
    if rc != 0 or len(paths) > MAX_LINES:
        raise ValueError("cannot read bounded changed paths")
    rc, diff, _err = git_cmd(
        repo,
        [
            "diff",
            "--no-ext-diff",
            "--no-textconv",
            "--no-renames",
            "--no-color",
            "--unified=0",
            "--src-prefix=a/",
            "--dst-prefix=b/",
            base,
            head,
            "--",
        ],
    )
    if rc != 0:
        raise ValueError("cannot read added diff lines")
    return paths, bounded_lines(diff, "diff")


def collect_context(args: argparse.Namespace) -> Context:
    base, head = resolve_commit(args.repo, args.base), resolve_commit(args.repo, args.head)
    attribution = load_attribution()
    commits = read_commits(args.repo, base, head, attribution)
    paths, diff = read_diff(args.repo, base, head)
    if len(args.draft) > MAX_DRAFTS:
        raise ValueError(f"at most {MAX_DRAFTS} drafts allowed")
    drafts = {path: bounded_lines(read_text(path), str(path)) for path in args.draft[:MAX_DRAFTS]}
    return Context(
        args.repo,
        base,
        head,
        commits,
        paths,
        diff,
        drafts,
        read_answers(args.self_review),
        args.mechanical_only,
        attribution,
    )


def judge(condition: bool, passed: str, failed: str) -> Verdict:
    return Verdict("PASS", passed) if condition else Verdict("FAIL", failed)


def body_shape_reason(lines: list[str]) -> str | None:
    if (
        len(lines) < MIN_MESSAGE_LINES
        or lines[1]
        or not any(line.strip() for line in lines[2:MAX_LINES])
    ):
        return "blank second line and non-empty body required"
    last_paragraph = (
        max((i for i, line in enumerate(lines[:MAX_LINES]) if not line.strip()), default=1) + 1
    )
    for lineno, line in enumerate(lines[2:MAX_LINES], 3):
        exempt = (
            URL_RE.fullmatch(line)
            or line.startswith(("    ", "\t"))
            or (lineno > last_paragraph and TRAILER_RE.match(line))
        )
        if len(line) > MAX_MESSAGE_COLUMNS and not exempt:
            return f"line {lineno}: body exceeds {MAX_MESSAGE_COLUMNS} columns"
    return None


def check_shape(ctx: Context) -> Verdict:
    for commit in ctx.commits[:MAX_PR_COMMITS]:
        subject = commit.subject
        if (
            len(subject) > MAX_MESSAGE_COLUMNS
            or not SUBJECT_RE.match(subject)
            or CONVENTIONAL_RE.match(subject)
        ):
            return Verdict(
                "FAIL",
                f"commit {commit.sha}: subject needs subsystem shape and at most 72 characters",
            )
        reason = body_shape_reason(bounded_lines(commit.message, commit.sha))
        if reason:
            return Verdict("FAIL", f"commit {commit.sha}: {reason}")
    return Verdict("PASS", "subjects and wrapped bodies follow upstream shape")


def has_reason(ctx: Context, cid: str) -> bool:
    return len(ctx.answers.get(cid, "")) >= MIN_ANSWER_LENGTH


def check_tests(ctx: Context) -> Verdict:
    code = any(path.startswith(SOURCE_PATHS) for path in ctx.paths[:MAX_LINES])
    tests = any(
        path.startswith(TEST_PATHS)
        or (Path(path).name.startswith("test_") and path.endswith(".py"))
        for path in ctx.paths[:MAX_LINES]
    )
    return judge(
        not code or tests or has_reason(ctx, "C04"),
        "tests changed or C04 reason supplied; no uncovered source change",
        "source changed without tests or a C04 self-review reason",
    )


def fork_reference(line: str) -> bool:
    return bool(FORK_RE.search(line)) or any(
        match[1].lower() != "netflix" for match in list(ACTION_RUN_RE.finditer(line))[:MAX_LINES]
    )


def line_leak(ctx: Context, lines: list[str], source: str, *, attribution: bool) -> Verdict | None:
    for lineno, line in enumerate(lines[:MAX_LINES], 1):
        if fork_reference(line) or (
            attribution and ctx.attribution.line_attribution_reason(line, source)
        ):
            return Verdict("FAIL", f"{source}:{lineno}: forbidden attribution or fork reference")
    return None


def diff_leak(ctx: Context) -> Verdict | None:
    path, lineno = "diff", 0
    for line in ctx.diff[:MAX_LINES]:
        if line.startswith("+++ "):
            path = line[4:].removeprefix("b/")
        elif line.startswith("@@ "):
            match = re.search(r"\+(\d+)", line)
            lineno = int(match[1]) if match else 0
        elif line.startswith("+"):
            if fork_reference(line[1:]) or ctx.attribution.line_attribution_reason(line[1:], path):
                return Verdict("FAIL", f"{path}:{lineno}: forbidden attribution or fork reference")
            lineno += 1
        elif line.startswith(" "):
            lineno += 1
    return None


def check_leaks(ctx: Context) -> Verdict:
    for commit in ctx.commits[:MAX_PR_COMMITS]:
        lines = bounded_lines(commit.message, commit.sha)
        reason = ctx.attribution.commit_attribution_reason(commit)
        if reason:
            for lineno, line in enumerate(lines[:MAX_LINES], 1):
                if ctx.attribution.line_attribution_reason(line, "message"):
                    return Verdict("FAIL", f"commit {commit.sha}:{lineno}: forbidden attribution")
            return Verdict("FAIL", f"commit {commit.sha}:1: forbidden identity or attribution")
        leak = line_leak(ctx, lines, f"commit {commit.sha}", attribution=False)
        if leak:
            return leak
    for path, lines in list(ctx.drafts.items())[:MAX_DRAFTS]:
        leak = line_leak(ctx, lines, str(path), attribution=True)
        if leak:
            return leak
    return diff_leak(ctx) or Verdict(
        "PASS", "no attribution or fork artifacts in messages, drafts or added lines"
    )


def check_merge(ctx: Context) -> Verdict:
    rc, out, _err = git_cmd(
        ctx.repo, ["merge-tree", "--write-tree", "--no-messages", "--name-only", ctx.base, ctx.head]
    )
    if rc == 0:
        return Verdict("PASS", "head merges cleanly onto the local upstream ref")
    paths = bounded_lines(out, "merge-tree")[1:MAX_LINES]
    return Verdict(
        "FAIL", f"merge-tree exited {rc}; conflicting paths: {', '.join(paths) or 'unavailable'}"
    )


def check_concern(ctx: Context) -> Verdict:
    return judge(
        len(ctx.commits) <= 1 or has_reason(ctx, "C07"),
        "one commit or a C07 split reason supplied",
        "multiple commits require a C07 self-review split reason",
    )


def draft_ids(ctx: Context) -> list[str]:
    ids: list[str] = []
    for lines in list(ctx.drafts.values())[:MAX_DRAFTS]:
        for line in lines[:MAX_LINES]:
            # The byte bound also bounds matches; reject rather than truncate IDs.
            ids.extend(COMMIT_ID_RE.findall(line))
            if len(ids) > MAX_DRAFT_IDS:
                raise ValueError(f"drafts exceed {MAX_DRAFT_IDS} commit tokens")
    return ids


def check_stale_ids(ctx: Context) -> Verdict:
    if not ctx.drafts:
        return Verdict("SKIP", "no draft files supplied")
    for token in draft_ids(ctx)[:MAX_DRAFT_IDS]:
        rc, out, _err = git_cmd(
            ctx.repo, ["rev-parse", "--verify", "--quiet", f"{token}^{{commit}}"]
        )
        if rc == 1:
            continue
        if rc != 0:
            return Verdict("FAIL", f"cannot resolve draft commit token {token}")
        sha = out.strip()
        statuses = [
            git_cmd(ctx.repo, ["merge-base", "--is-ancestor", sha, tip])[0]
            for tip in (ctx.head, ctx.base)
        ]
        if any(status not in (0, 1) for status in statuses):
            return Verdict("FAIL", f"cannot check reachability of draft commit {token}")
        if 0 not in statuses:
            return Verdict("FAIL", f"draft commit {token} is unreachable from head or upstream ref")
    return Verdict("PASS", "all resolved draft commit IDs are reachable")


def check_signatures(ctx: Context) -> Verdict:
    count, detail = count_unsigned(ctx.repo, ctx.base, ctx.head)
    return judge(
        count == 0,
        "every contribution commit has signature status G",
        detail or f"{count} commits lack signature status G",
    )


def check_target(ctx: Context) -> Verdict:
    for path in ctx.paths[:MAX_LINES]:
        if any(path == prefix or path.startswith(prefix + "/") for prefix in FORK_PATHS):
            return Verdict("FAIL", f"fork-only path: {path}")
    return judge(
        len(ctx.commits) <= MAX_CONTRIBUTION_COMMITS,
        "no fork-only paths; range at most 3 commits",
        f"range has {len(ctx.commits)} commits; maximum is 3",
    )


def check_review(ctx: Context, cid: str) -> Verdict:
    if ctx.mechanical_only is not None:
        return Verdict("SKIP", f"--mechanical-only: {ctx.mechanical_only}")
    answer = ctx.answers.get(cid, "")
    if len(answer) < MIN_ANSWER_LENGTH:
        return Verdict(
            "FAIL",
            f"self-review {cid} requires at least {MIN_ANSWER_LENGTH} characters of answer and evidence",
        )
    kernel = any(
        path.startswith(KERNEL_PATHS)
        or any(token in Path(path).name.lower() for token in ("avx", "neon"))
        for path in ctx.paths[:MAX_LINES]
    )
    if cid in {"C08", "C16"} and kernel and answer.lower().startswith("n/a"):
        return Verdict(
            "FAIL", "kernel/SIMD changes require applicable measurement or bit-depth evidence"
        )
    return Verdict("PASS", "self-review answer and evidence supplied")


CHECKS: dict[str, tuple[str, Callable[[Context], Verdict]]] = {
    "C03": ("mechanical", check_shape),
    "C04": ("mechanical", check_tests),
    "C05": ("mechanical", check_leaks),
    "C06": ("mechanical", check_merge),
    "C07": ("mechanical", check_concern),
    "C08": ("self-review", lambda ctx: check_review(ctx, "C08")),
    "C09": ("mechanical", check_stale_ids),
    "C11": ("self-review", lambda ctx: check_review(ctx, "C11")),
    "C12": ("self-review", lambda ctx: check_review(ctx, "C12")),
    "C13": ("self-review", lambda ctx: check_review(ctx, "C13")),
    "C14": ("mechanical", check_signatures),
    "C15": ("self-review", lambda ctx: check_review(ctx, "C15")),
    "C16": ("self-review", lambda ctx: check_review(ctx, "C16")),
    "C17": ("mechanical", check_target),
}


def run_gate(
    repo: Path,
    *,
    base: str = "upstream/master",
    head: str = "HEAD",
    drafts: Sequence[Path] = (),
    self_review: Path | None = None,
    mechanical_only: str | None = None,
) -> list[CheckResult]:
    if mechanical_only is not None and not mechanical_only.strip():
        raise ValueError("--mechanical-only requires a non-empty reason")
    classes = load_classes()
    args = argparse.Namespace(
        repo=repo,
        base=base,
        head=head,
        draft=drafts,
        self_review=self_review,
        mechanical_only=mechanical_only,
    )
    ctx = collect_context(args)
    results: list[CheckResult] = []
    for entry in classes[:MAX_CLASSES]:
        if entry.id in CHECKS:
            try:
                verdict = CHECKS[entry.id][1](ctx)
            except (OSError, ValueError, RuntimeError) as exc:
                verdict = Verdict("FAIL", f"check could not complete ({type(exc).__name__})")
        else:
            reason = (
                "online check deferred to a later round"
                if entry.mode == "online"
                else "catalogued only; outside author cannot request reviewers"
            )
            verdict = Verdict("SKIP", reason)
        results.append(CheckResult(entry.id, entry.slug, verdict))
    return results


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--base", default="upstream/master")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--draft", action="extend", nargs="+", type=Path, default=[])
    parser.add_argument("--self-review", type=Path)
    parser.add_argument("--mechanical-only", metavar="REASON")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        results = run_gate(
            args.repo,
            base=args.base,
            head=args.head,
            drafts=args.draft,
            self_review=args.self_review,
            mechanical_only=args.mechanical_only,
        )
    except (OSError, ValueError, RuntimeError, CommandOutputLimitExceeded) as exc:
        print(f"upstream-push-gate: error: {exc}", file=sys.stderr)
        return 2
    for result in results[:MAX_CLASSES]:
        print(result.line())
    return int(any(result.verdict.status == "FAIL" for result in results[:MAX_CLASSES]))


if __name__ == "__main__":
    raise SystemExit(main())

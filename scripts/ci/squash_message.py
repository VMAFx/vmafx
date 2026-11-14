#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compose and check squash commit messages for HISS-14 compliance (issue 2435).

Defect class:
When a breaking-change pull request lands as a squash commit without a
'Migration:' footer, it violates HISS-14. In repositories where the GitHub
setting `squash_merge_commit_message` is set to BLANK, the lander previously
passed only `--body "Signed-off-by: ..."` or no body/subject flags at all,
dropping any footer lines from the PR body.

Rule enforcement:
Rather than re-implementing commit message policy checks in Python (violating
HISS-19), this module invokes the pinned Praetor engine via:
    sh .config/lefthook/engine.sh forge check-message <file>
which provides the authoritative evaluation for conventional commit format,
breaking change markers ('!' or 'BREAKING CHANGE:'), mandatory 'Migration:'
footers, and sign-off trailers. Two repository-side gaps stay out of the
engine's reach and are checked here, in subject_policy_error(): the engine
only ever sees the composed subject '<title> (#<number>)', so a title whose
conventional subject is empty (a bare 'fix:') passes once the suffix fills
the description slot; and the engine's type list is upstream's, so a type
the hook refuses because .pre-commit-config.yaml does not declare it (such
as 'style') would otherwise pass. Anything else remains the engine's call.

Fork commit types and engine compatibility:
The engine's `forge check-message` enforces a fixed type list (build, chore,
ci, docs, feat, fix, perf, refactor, revert, style, test). This repository
declares additional conventional types (port, sycl, cuda, simd) in
.pre-commit-config.yaml. To allow these types while maintaining strict
validation of breaking changes and 'Migration:' footers, check_message()
maps declared non-engine types to 'chore' for the engine check. The original
type is preserved in the squash commit subject. Removal trigger: remove this
substitution once cordanaLLM/praetor issue 1861 lands and the engine natively
accepts repository-declared types.

Functions:
- compose(title, number, body, identity): Returns the full squash message.
- subject(title, number): Returns line 1 ('<title> (#<number>)').
- bare_subject(title, number): Returns line 1 without the appended ' (#<number>)'.
- body(body_text, identity): Returns the body content after the blank line.
- parse_subject_prefix(bare): Returns (type, description) or None without a prefix.
- subject_policy_error(bare, declared): Repository-side subject refusal, or ''.
- check_message(title, number, body, identity, ...): Checks message with engine.
- declared_types(repo_root): Extracts conventional commit types from config.
- engine_subject(subject, declared): Maps non-engine declared types to 'chore'.
- engine_available(root): Checks if the pinned Praetor engine wrapper is runnable.
- engine_wrapper_argv(root): Returns the absolute [sh, wrapper] prefix, or an error.

CLI:
- check: Reads inputs (--title, --number, --body/--body-file, --identity, --timeout) from flags
  or environment variables, runs the engine check.
- body: Prints the composed commit body for a lander.
- emit-label: Prints the type:<type> label for a pull-request title and refuses
  titles without a declared type prefix (--title or PR_TITLE).
"""

from __future__ import annotations

import argparse
import functools
import os
import re
import shutil
import subprocess  # argv-list calls only, no shell  # nosec B404
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[2]
MAX_FOOTER_LINES = 20

# GitHub caps PR bodies at 65,536 characters; 65,537 lines is the maximum
# possible line count even for single-character lines. Raising on excess
# prevents silent truncation of footers.
MAX_BODY_LINES = 65_537

DEFAULT_TIMEOUT_SECONDS = 600.0
# Engine install bound 300 s plus 60 s, matching CI check --timeout 360 (rule-enforcement.yml:524).
ENGINE_PROBE_TIMEOUT_SECONDS = 360.0
DEFAULT_LANDING_IDENTITY = "lusoris <lusoris@proton.me>"
FOOTER_RE = re.compile(r"^(?:BREAKING CHANGE|BREAKING-CHANGE|Migration): \S")
CONVENTIONAL_SUBJECT_RE = re.compile(r"^([a-z]+)(\([^)]*\))?(!)?: ")
# Repository-side subject policy (issue 2750): the description slot may be
# empty before the appended ' (#N)' suffix, so the engine never sees it.
SUBJECT_PREFIX_RE = re.compile(r"^([a-z]+)(\([^)]*\))?(!)?:\s*(.*)$")
SQUASH_SUFFIX_RE = re.compile(r" \(#\d+\)$")

# Fixed commit subject types supported by praetor internal/forge/commit_message.go
# (commitSubjectTypes) at pin 4ae64cb27e56.
# Removal trigger: remove ENGINE_TYPES and substitution when cordanaLLM/praetor 1861 lands.
ENGINE_TYPES: tuple[str, ...] = (
    "build",
    "chore",
    "ci",
    "docs",
    "feat",
    "fix",
    "perf",
    "refactor",
    "revert",
    "style",
    "test",
)


def find_conventional_hook_args(data: dict[str, object]) -> list[object] | None:
    """Locate args list for conventional-pre-commit hook in parsed YAML."""
    repos = data.get("repos")
    if not isinstance(repos, list):
        return None
    for repo in repos:
        if isinstance(repo, dict):
            hooks = repo.get("hooks")
            if isinstance(hooks, list):
                for hook in hooks:
                    if isinstance(hook, dict) and hook.get("id") == "conventional-pre-commit":
                        args = hook.get("args")
                        return args if isinstance(args, list) else None
    return None


def declared_types(repo_root: Path | None = None) -> list[str]:
    """Load conventional commit types declared in .pre-commit-config.yaml.

    Raises ValueError naming .pre-commit-config.yaml if the configuration file,
    the conventional-pre-commit hook, or its declared types are missing.
    """
    root = repo_root or REPO_ROOT
    config_path = root / ".pre-commit-config.yaml"
    if not config_path.is_file():
        raise ValueError(f".pre-commit-config.yaml not found at {config_path}")

    try:
        data = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f"failed to load .pre-commit-config.yaml: {exc}") from exc

    if not isinstance(data, dict):
        raise ValueError(f"invalid .pre-commit-config.yaml structure in {config_path}")

    args = find_conventional_hook_args(data)
    if args is None:
        raise ValueError(
            f"conventional-pre-commit hook or args missing in .pre-commit-config.yaml: {config_path}"
        )

    types = [str(arg) for arg in args if isinstance(arg, str) and not arg.startswith("--")]
    if not types:
        raise ValueError(
            f"no conventional commit types declared in .pre-commit-config.yaml: {config_path}"
        )
    return types


def engine_subject(subject: str, declared: Sequence[str]) -> str:
    """Map declared fork types to 'chore' for the engine check until upstream #1861.

    When the subject's conventional commit type is declared in .pre-commit-config.yaml
    and not yet in ENGINE_TYPES, replace only the type with 'chore', keeping scope,
    '!' and the rest byte for byte. Any other subject remains unchanged.
    """
    m = CONVENTIONAL_SUBJECT_RE.match(subject)
    if not m:
        return subject
    commit_type = m.group(1)
    if commit_type in declared and commit_type not in ENGINE_TYPES:
        return f"chore{subject[m.end(1) :]}"
    return subject


def engine_message(message: str, declared: Sequence[str]) -> str:
    """Return `message` with line 1 passed through engine_subject().

    Prints one notice to stderr when the type was substituted, so the run log
    says which type the engine judged as 'chore'.
    """
    line1, sep, rest = message.partition("\n")
    judged = engine_subject(line1, declared)
    if judged != line1:
        commit_type = line1.split(":", 1)[0].split("(", 1)[0].rstrip("!")
        print(
            f"squash_message: type '{commit_type}' (declared in .pre-commit-config.yaml) "
            "judged as 'chore' until cordanaLLM/praetor 1861 lands",
            file=sys.stderr,
        )
    return f"{judged}{sep}{rest}"


def extract_footers(body_text: str) -> list[str]:
    """Extract breaking-change and migration footers from the PR body."""
    lines = body_text.split("\n")
    if len(lines) > MAX_BODY_LINES:
        raise ValueError(
            f"body line count ({len(lines)}) exceeds maximum allowed limit ({MAX_BODY_LINES})"
        )
    footers: list[str] = []
    for line in lines:
        clean_line = line[:-1] if line.endswith("\r") else line
        if FOOTER_RE.match(clean_line):
            if len(footers) >= MAX_FOOTER_LINES:
                raise ValueError(f"exceeded maximum allowed footer lines ({MAX_FOOTER_LINES})")
            footers.append(clean_line)
    return footers


def subject(title: str, number: int) -> str:
    """Format line 1 of the squash commit message."""
    clean_title = title.strip()
    return f"{clean_title} (#{number})"


def bare_subject(title: str, number: int) -> str:
    """Return line 1 of the squash message without the appended ' (#<number>)'."""
    return SQUASH_SUFFIX_RE.sub("", subject(title, number), count=1)


def parse_subject_prefix(bare_subject: str) -> tuple[str, str] | None:
    """Parse '<type>(<scope>)!<description>' from a subject without the suffix.

    Returns (type, description), or None when the subject carries no
    conventional prefix — those stay the engine's to judge.
    """
    match = SUBJECT_PREFIX_RE.match(bare_subject)
    if not match:
        return None
    return match.group(1), match.group(4)


def subject_policy_error(bare_subject: str, declared: Sequence[str]) -> str:
    """Return the repository-side subject refusal for bare_subject, or ''.

    Covers exactly the two gaps the pinned engine cannot see (issue 2750):
    a description that is empty before the appended ' (#N)' suffix, and a
    type the commit-msg hook refuses because .pre-commit-config.yaml does
    not declare it (such as 'style'). A subject without a conventional
    prefix returns '' and remains the engine's call.
    """
    parsed = parse_subject_prefix(bare_subject)
    if parsed is None:
        return ""
    commit_type, description = parsed
    if commit_type not in declared:
        return (
            f"type '{commit_type}' is not declared in .pre-commit-config.yaml "
            "(the commit-msg hook refuses it)"
        )
    if not description.strip():
        return "the conventional subject is empty before the squash ' (#N)' suffix"
    return ""


def body(body_text: str, identity: str) -> str:
    """Return the squash commit message body (after the subject and blank line)."""
    footers = extract_footers(str(body_text or ""))
    if footers:
        return "\n".join(footers) + f"\nSigned-off-by: {identity}\n"
    return f"Signed-off-by: {identity}\n"


def compose(title: str, number: int, body_text: str, identity: str) -> str:
    """Return the full squash commit message."""
    subj = subject(title, number)
    b = body(body_text, identity)
    return f"{subj}\n\n{b}"


def engine_wrapper_argv(root: Path) -> tuple[list[str] | None, str]:
    """Return the absolute [sh, wrapper] prefix for the pinned engine, or an error."""
    resolved_sh = shutil.which("sh")
    if not resolved_sh:
        return None, "sh executable not found on PATH\n"
    # absolute(), not resolve(): a multi-call shell (busybox) dispatches on argv[0].
    abs_sh = Path(resolved_sh).absolute()
    if not abs_sh.is_file():
        return None, f"sh executable not found or not a regular file: {abs_sh}\n"
    wrapper = root / ".config/lefthook/engine.sh"
    if not wrapper.is_file():
        return None, f"engine wrapper not found or not a regular file: {wrapper}\n"
    return [str(abs_sh), str(wrapper)], ""


@functools.cache
def _engine_available_cached(root: Path) -> bool:
    """Probe the pinned Praetor engine wrapper at root and cache the result."""
    prefix, _ = engine_wrapper_argv(root)
    if prefix is None:
        return False
    try:
        res = subprocess.run(  # noqa: S603 -- engine version probe, no shell  # nosec B603
            [*prefix, "version"],
            cwd=str(root),
            capture_output=True,
            text=True,
            timeout=ENGINE_PROBE_TIMEOUT_SECONDS,
            check=False,
        )
        return res.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def engine_available(root: Path | None = None) -> bool:
    """Check if the pinned Praetor engine wrapper is runnable."""
    resolved_root = (Path(root) if root is not None else REPO_ROOT).resolve()
    return _engine_available_cached(resolved_root)


def resolve_engine_cmd(root: Path, engine_argv: list[str] | None) -> tuple[list[str] | None, str]:
    """Resolve and validate the engine command argv prefix."""
    if engine_argv is None:
        prefix, err = engine_wrapper_argv(root)
        if prefix is None:
            return None, err
        return [*prefix, "forge", "check-message"], ""

    if (
        not isinstance(engine_argv, list)
        or not engine_argv
        or not all(isinstance(x, str) for x in engine_argv)
        or not Path(engine_argv[0]).is_absolute()
    ):
        return (
            None,
            "engine_argv must be a non-empty list of strings starting with an absolute path\n",
        )
    return list(engine_argv), ""


def check_message(
    title: str,
    number: int,
    body_text: str,
    identity: str,
    *,
    engine_argv: list[str] | None = None,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    repo_root: Path | None = None,
) -> tuple[int, str]:
    """Check the composed squash message against commit policy.

    Returns a tuple of (exit_status, output_text):
      0: message passes policy
      1: message refused by engine policy check
      2: usage error, engine execution failure, timeout, or missing binary
    """
    root = repo_root or REPO_ROOT
    try:
        declared = declared_types(root)
        msg = engine_message(compose(title, number, body_text, identity), declared)
    except ValueError as exc:
        return 2, f"composition error: {exc}\n"

    policy_reason = subject_policy_error(bare_subject(title, number), declared)
    if policy_reason:
        return 1, f"refusing subject: {policy_reason}\n"

    cmd_prefix, err = resolve_engine_cmd(root, engine_argv)
    if cmd_prefix is None:
        return 2, err

    temp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            delete=False,
            prefix="squash-msg-",
            suffix=".txt",
        ) as tmp:
            tmp.write(msg)
            temp_path = Path(tmp.name)

        run_args = [*cmd_prefix, str(temp_path)]
        res = subprocess.run(  # noqa: S603 -- engine wrapper invocation, no shell  # nosec B603
            run_args,
            cwd=str(root),
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
        combined_output = res.stdout
        if res.stderr:
            combined_output += res.stderr
        if res.returncode in (0, 1):
            return res.returncode, combined_output
        return 2, f"engine returned unexpected exit code {res.returncode}:\n{combined_output}"
    except (subprocess.TimeoutExpired, OSError) as exc:
        return 2, f"engine execution failed: {exc}\n"
    finally:
        if temp_path and temp_path.exists():
            temp_path.unlink(missing_ok=True)


def resolve_body(args: argparse.Namespace) -> str:
    """Resolve body text from either a file or CLI/env string."""
    if args.body_file:
        return Path(args.body_file).read_text(encoding="utf-8")
    return str(args.body or "")


def run_check_cmd(args: argparse.Namespace) -> int:
    """Handle the 'check' CLI subcommand."""
    title = str(args.title or "").strip()
    if not title:
        print("squash_message: PR title is required", file=sys.stderr)
        return 2

    raw_num = str(args.number or "").strip()
    if not raw_num.isdigit():
        print("squash_message: valid PR number is required", file=sys.stderr)
        return 2
    number = int(raw_num)

    identity = str(args.identity or "").strip()
    if not identity:
        print("squash_message: landing identity is required", file=sys.stderr)
        return 2

    try:
        body_text = resolve_body(args)
    except OSError as exc:
        print(f"squash_message: failed to read body-file: {exc}", file=sys.stderr)
        return 2

    code, output = check_message(
        title=title,
        number=number,
        body_text=body_text,
        identity=identity,
        timeout_seconds=args.timeout,
    )
    if output:
        if code == 0:
            sys.stdout.write(output)
        else:
            sys.stderr.write(output)
    return code


def run_body_cmd(args: argparse.Namespace) -> int:
    """Handle the 'body' CLI subcommand."""
    try:
        body_text = resolve_body(args)
    except OSError as exc:
        print(f"squash_message: failed to read body-file: {exc}", file=sys.stderr)
        return 2

    identity = str(args.identity or "").strip()
    if not identity:
        print("squash_message: landing identity is required", file=sys.stderr)
        return 2

    try:
        composed_body = body(body_text, identity)
    except ValueError as exc:
        print(f"squash_message: {exc}", file=sys.stderr)
        return 2
    sys.stdout.write(composed_body)
    return 0


def run_emit_label_cmd(args: argparse.Namespace) -> int:
    """Handle the 'emit-label' CLI subcommand."""
    title = str(args.title or "").strip()
    if not title:
        print("squash_message: PR title is required", file=sys.stderr)
        return 2

    try:
        declared = declared_types()
    except ValueError as exc:
        print(f"squash_message: {exc}", file=sys.stderr)
        return 2

    reason = subject_policy_error(title, declared)
    parsed = parse_subject_prefix(title)
    if reason or parsed is None:
        print(
            f"squash_message: {reason or 'title has no conventional-commit prefix'}",
            file=sys.stderr,
        )
        return 1
    print(f"type:{parsed[0]}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    """Construct CLI argument parser with subcommands."""
    env = os.environ.get
    parser = argparse.ArgumentParser(
        description="Compose and check squash commit messages (issue 2435)."
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    check_p = subparsers.add_parser("check", help="Check composed squash message")
    check_p.add_argument("--title", default=env("PR_TITLE", ""))
    check_p.add_argument("--number", default=env("PR_NUMBER", ""))
    check_p.add_argument("--body", default=env("PR_BODY", ""))
    check_p.add_argument("--body-file", default="")
    check_p.add_argument(
        "--identity",
        default=env("LANDING_IDENTITY", DEFAULT_LANDING_IDENTITY),
    )
    check_p.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
    )

    body_p = subparsers.add_parser("body", help="Print squash commit message body")
    body_p.add_argument("--title", default=env("PR_TITLE", ""))
    body_p.add_argument("--number", default=env("PR_NUMBER", ""))
    body_p.add_argument("--body", default=env("PR_BODY", ""))
    body_p.add_argument("--body-file", default="")
    body_p.add_argument(
        "--identity",
        default=env("LANDING_IDENTITY", DEFAULT_LANDING_IDENTITY),
    )

    label_p = subparsers.add_parser(
        "emit-label", help="Print the type:* label for a pull-request title"
    )
    label_p.add_argument("--title", default=env("PR_TITLE", ""))
    return parser


def main(argv: list[str] | None = None) -> int:
    """CLI entry point returning exit code."""
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code != 0 else 0

    if args.subcommand == "check":
        return run_check_cmd(args)
    if args.subcommand == "body":
        return run_body_cmd(args)
    if args.subcommand == "emit-label":
        return run_emit_label_cmd(args)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

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
footers, and sign-off trailers.

Functions:
- compose(title, number, body, identity): Returns the full squash message.
- subject(title, number): Returns line 1 ('<title> (#<number>)').
- body(body_text, identity): Returns the body content after the blank line.
- check_message(title, number, body, identity, ...): Checks message with engine.

CLI:
- check: Reads inputs from flags or environment variables, runs the engine check.
- body: Prints the composed commit body for a lander.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MAX_FOOTER_LINES = 20
MAX_BODY_LINES = 50_000
DEFAULT_ENGINE_ARGV = ["sh", ".config/lefthook/engine.sh", "forge", "check-message"]
DEFAULT_TIMEOUT_SECONDS = 600.0
DEFAULT_LANDING_IDENTITY = "lusoris <lusoris@proton.me>"
FOOTER_RE = re.compile(r"^(?:BREAKING CHANGE|BREAKING-CHANGE|Migration): \S")


def extract_footers(body_text: str) -> list[str]:
    """Extract breaking-change and migration footers from the PR body."""
    footers: list[str] = []
    lines = body_text.split("\n")
    bound = min(len(lines), MAX_BODY_LINES)
    for i in range(bound):
        line = lines[i]
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
    try:
        msg = compose(title, number, body_text, identity)
    except ValueError as exc:
        return 2, f"composition error: {exc}\n"

    cmd_prefix = list(engine_argv) if engine_argv is not None else list(DEFAULT_ENGINE_ARGV)
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
        res = subprocess.run(  # noqa: S603 - fixed argv, no shell
            run_args,
            cwd=str(repo_root or REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
        combined_output = res.stdout
        if res.stderr:
            combined_output += res.stderr
        if res.returncode == 0:
            return 0, combined_output
        if res.returncode == 1:
            return 1, combined_output
        return 2, f"engine returned unexpected exit code {res.returncode}:\n{combined_output}"
    except (subprocess.TimeoutExpired, OSError, Exception) as exc:
        return 2, f"engine execution failed: {exc}\n"
    finally:
        if temp_path and temp_path.exists():
            temp_path.unlink(missing_ok=True)


def resolve_body(args: argparse.Namespace) -> str:
    """Resolve body text from either a file or CLI/env string."""
    if getattr(args, "body_file", ""):
        return Path(args.body_file).read_text(encoding="utf-8")
    return str(args.body or "")


def parse_engine_argv(raw: str | None) -> list[str] | None:
    """Parse JSON list override for engine argv."""
    if not raw:
        return None
    parsed = json.loads(raw)
    if not isinstance(parsed, list) or not all(isinstance(x, str) for x in parsed):
        raise ValueError("--engine-argv must be a JSON list of strings")
    return parsed


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
        engine_argv = parse_engine_argv(args.engine_argv)
    except Exception as exc:
        print(f"squash_message: invalid --engine-argv: {exc}", file=sys.stderr)
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
        engine_argv=engine_argv,
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
    check_p.add_argument("--engine-argv", default=None)
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
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

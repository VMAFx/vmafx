#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep test, coverage, benchmark, and scan failures observable in CI."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Callable
from functools import partial
from pathlib import Path
from unittest import mock

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment, as in scripts/ci/check_composite_actions.py

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

# Hook runs keep the staged index for this checkout; fixtures drop caller GIT_* (issue #3324).
from scripts.ci.check_agent_copyright_rule import git_environment  # noqa: E402
from scripts.lib.renovate_regex import to_python  # noqa: E402
from scripts.lib.safe_subprocess import CommandFailed, TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

WORKFLOWS = ROOT / ".github" / "workflows"
SCRIPT_TIMEOUT_S = 60
# 466 tracked source files at introduction; allow fourfold growth.
MAX_CI_TEXT_FILES = 1_864
MAX_CONTINUATION_LINES = 200
MAX_APT_WORK_ITEMS = 8
MAX_YAML_NODES = 16_384
# File names the dev images copy into their build stage. The snapshot checks
# take the staging directory from the Dockerfile's own COPY destination.
IMAGE_BUILD_CONFIG_NAME = "build-config.env"
IMAGE_INSTALLER_NAMES = frozenset(("ubuntu.sh", "install-cuda-toolkit.sh"))
DOCKERFILE_INSTRUCTIONS = frozenset(("COPY", "FROM"))
PPA_SOURCE_TEXT = re.compile(
    r"add-apt-repository|apt-add-repository|\bppa:|ppa\.launchpad(?:content)?\.net",
    re.IGNORECASE,
)
APT_UPDATE_LINE = re.compile(
    r"(?:^|[\s;|&()'\"=/`])(?-i:-[A-Za-z]*S)?apt(?:-get)?(?=$|[\s;|&()'\"#\\])[\s\S]*\bupdate\b",
    re.IGNORECASE,
)
POSITIVE_INTEGER_RE = re.compile(r"[1-9]\d*")
ENV_ASSIGNMENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=")
CONTAINER_EXEC_RUN_RE = re.compile(r"\s*run(?=\s)", re.IGNORECASE)
CONTAINER_RUN_FLAG_RE = re.compile(r"--[A-Za-z0-9][A-Za-z0-9_-]*(?:=\S*)?\Z")
SHELL_OPERATOR_CHARS = frozenset(";&|()")
SHELL_BASENAMES = frozenset({"bash", "sh", "dash", "zsh", "ksh"})
SUDO_ARG_FLAGS = frozenset({"-u", "-g", "-C", "-D", "-h", "-p", "-r", "-t", "-U", "-T"})
SUDO_LONG_ARG_FLAGS = frozenset(
    {
        "--user",
        "--group",
        "--close-from",
        "--chdir",
        "--host",
        "--prompt",
        "--role",
        "--type",
        "--other-user",
        "--command-timeout",
    }
)
ENV_SHORT_ARG_FLAGS = frozenset({"-u", "-C", "-a"})
ENV_LONG_ARG_FLAGS = frozenset({"--unset", "--chdir", "--argv0", "--env0-from"})
APT_UPDATE_MESSAGE = (
    "apt-get update must fail on index errors: add --error-on=any -o Acquire::Retries=3 (ADR-3537)"
)
APT_UNBALANCED_QUOTE_MESSAGE = (
    "apt refresh must sit on one logical line with balanced quotes (ADR-3537)"
)
APT_CONTINUATION_MESSAGE = (
    f"continuation exceeds {MAX_CONTINUATION_LINES} physical lines; "
    "keep each command bounded (ADR-3537)"
)
APT_NESTING_MESSAGE = (
    f"nested shell command exceeds {MAX_APT_WORK_ITEMS} items on one logical line (ADR-3537)"
)
HOSTED_APT_UPDATE_MESSAGE = (
    "hosted-runner apt refresh must run through scripts/ci/apt-install-cached.sh refresh or "
    "install (it drops the runner's Microsoft/Azure sources first; ADR-3537)"
)
FOLDED_RUN_APT_MESSAGE = (
    "multi-line run scalar holding an apt refresh must be a literal block (run: |) "
    "so each command sits on its own line (ADR-3537)"
)
HOSTED_YAML_PARSE_MESSAGE = (
    "hosted workflow or action YAML does not parse; "
    "the apt refresh contract cannot read its run blocks (ADR-3537)"
)
HOSTED_YAML_DUPLICATE_KEY_MESSAGE = (
    "hosted workflow or action YAML contains a duplicate mapping key (ADR-3537)"
)
HOSTED_YAML_NODE_BOUND_MESSAGE = (
    f"hosted workflow or action YAML exceeds {MAX_YAML_NODES} nodes (ADR-3537)"
)
NON_REGULAR_CI_TEXT_MESSAGE = (
    "tracked CI text path is not a regular file; the contract cannot read it"
)


def workflow_step(text: str, name: str) -> str:
    """Return one top-level step block by its exact display name."""
    match = re.search(
        rf"(?ms)^      - name: {re.escape(name)}\n(?P<body>.*?)(?=^      - |\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow step not found: {name}")
    return match.group(0)


def executable_body(step: str) -> str:
    """Return a step with its full-line YAML/shell comments removed.

    The fail-open checks below look for literal strings like ``|| true`` in a
    step's text. A comment is not executed, so a comment that *names* the
    construct — for instance one recording why a suffix was removed — is not a
    fail-open and must not be reported as one. Only lines whose first
    non-whitespace character is ``#`` are dropped; an inline trailing comment
    stays, because the command in front of it runs.
    """
    return "\n".join(line for line in step.splitlines() if not line.lstrip().startswith("#"))


def step_script(step: str) -> str:
    """Return the dedented `run: |` script of one workflow step."""
    match = re.search(r"(?ms)^        run: \|\n(?P<body>(?:^          .*\n|^\n)+)", step)
    if match is None:
        raise AssertionError("step has no `run: |` block")
    return "".join(line[10:] if line.strip() else "\n" for line in match["body"].splitlines(True))


def workflow_job(text: str, job_id: str) -> str:
    """Return one top-level job block by its identifier."""
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow job not found: {job_id}")
    return match.group(0)


def python_lint_job_problems(job: str) -> list[str]:
    """Return fail-open defects in the required Python Lint job."""
    problems: list[str] = []
    if "fetch-depth: 0" not in job:
        problems.append("Python Lint checkout must fetch merge-base history")
    if "pip install --require-hashes -r requirements/locks/mypy.txt" not in job:
        problems.append("Python Lint must install the hash-locked mypy toolchain")
    try:
        check = workflow_step(job, "Run merge-base mypy gate")
    except AssertionError:
        problems.append("Python Lint must invoke the merge-base mypy gate")
        return problems
    body = executable_body(check)
    if (
        "VMAFX_MYPY_BASE_REF:" not in check
        or "github.event.before" not in check
        or "origin/master" not in check
    ):
        problems.append("Python Lint must select a PR or push merge-base authority")
    if "python3 scripts/git-hooks/pre-push-mypy.py" not in body:
        problems.append("Python Lint must use the canonical merge-base mypy gate")
    if "continue-on-error:" in check or re.search(r"(?m)^\s*run:.*\|\|", body):
        problems.append("Python Lint must propagate the mypy gate status")
    if re.search(r"\bmypy\s+(?:ai/|scripts/)", body):
        problems.append("Python Lint must not bypass canonical module-identity splitting")
    return problems


def ppa_text_problems(path: Path, text: str) -> list[str]:
    """Reject source-registration text once per matching physical line, including data."""
    problems: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        match = PPA_SOURCE_TEXT.search(line)
        if match is not None:
            problems.append(
                f"{path.as_posix()}:{number}: {match.group(0)} registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            )
    return problems


def listing_git_environment(root: Path) -> dict[str, str]:
    """Return git's environment for listing `root`.

    This checkout keeps the caller's environment, so a commit hook lists the staged index.
    Any other tree (a test fixture) drops every inherited GIT_* variable and reads neither
    system nor global configuration, so a caller's ~/.gitconfig cannot change the listing.
    """
    env = git_environment(root)
    if root.resolve() != ROOT.resolve():
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return env


def _is_containerfile_path(path: Path) -> bool:
    """Return True when `path` selects a Dockerfile or Containerfile source."""
    return path.suffix in (".Dockerfile", ".Containerfile") or path.name.startswith(
        ("Dockerfile", "Containerfile")
    )


def ci_text_paths(root: Path) -> list[Path]:
    """List tracked YAML, shell and container source paths throughout the repository."""
    result = run_command(
        ["git", "-C", str(root), "ls-files", "-z"],
        allowed_executables=("git",),
        env=listing_git_environment(root),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=SCRIPT_TIMEOUT_S,
    )
    paths = [
        root / name
        for name in result.stdout.split("\0")
        if name
        and (
            Path(name).suffix in (".yml", ".yaml", ".sh", ".bash")
            or _is_containerfile_path(Path(name))
        )
    ]
    if len(paths) > MAX_CI_TEXT_FILES:
        raise AssertionError(f"CI text exceeds {MAX_CI_TEXT_FILES} files")
    return sorted(paths)


def _logical_ci_lines(text: str) -> list[tuple[int, str, bool]]:
    """Join backslash-continued physical lines up to MAX_CONTINUATION_LINES."""
    logical: list[tuple[int, str, bool]] = []
    parts: list[str] = []
    start_line = 1
    count = 0
    for number, raw in enumerate(text.splitlines(), 1):
        if count == 0:
            start_line = number
        count += 1
        stripped = raw.rstrip()
        continues = stripped.endswith("\\")
        if count <= MAX_CONTINUATION_LINES:
            parts.append(stripped[:-1] if continues else raw)
        if not continues:
            logical.append((start_line, "\n".join(parts), count > MAX_CONTINUATION_LINES))
            parts = []
            count = 0
    if count > 0:
        logical.append((start_line, "\n".join(parts), count > MAX_CONTINUATION_LINES))
    return logical


def _is_hosted_workflow_or_action(path: Path) -> bool:
    """Return True for GitHub workflow files and composite action definitions."""
    match path.parts:
        case (".github", "workflows", _):
            return path.suffix in (".yml", ".yaml")
        case (".github", "actions", *middle, "action.yml" | "action.yaml"):
            return bool(middle)
        case _:
            return False


def _is_apt_token(token: str) -> bool:
    """Return True when `token` names apt or apt-get by basename."""
    return posixpath.basename(token).lower() in ("apt", "apt-get")


def _is_sudo_token(token: str) -> bool:
    """Return True when `token` names sudo by basename."""
    return posixpath.basename(token) == "sudo"


def _is_operator_token(token: str) -> bool:
    """Return True when `token` consists solely of shell control operators."""
    return bool(token) and set(token) <= SHELL_OPERATOR_CHARS


def _tokenise_shell_item(text: str) -> list[str]:
    """Tokenise one logical shell line or nested command string with shlex."""
    lexer = shlex.shlex(text, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    lexer.commenters = "#"
    return list(lexer)


def _shell_clauses_with_operators(tokens: list[str]) -> list[tuple[list[str], str]]:
    """Pair each non-empty command clause with the operator token that ends it ("" at the end)."""
    pairs: list[tuple[list[str], str]] = []
    current: list[str] = []
    for token in tokens:
        if not _is_operator_token(token):
            current.append(token)
        elif current:
            pairs.append((current, token))
            current = []
    if current:
        pairs.append((current, ""))
    return pairs


def _split_shell_clauses(tokens: list[str]) -> list[list[str]]:
    """Split shell tokens on operator boundaries into non-empty command clauses."""
    return [clause for clause, _ in _shell_clauses_with_operators(tokens)]


def _parse_apt_option(raw: str) -> tuple[str, str]:
    """Split one `-o` / `--option` payload into a lowercase key and raw value."""
    key, sep, value = raw.partition("=")
    return key.strip().lower(), value if sep else ""


def _extract_apt_options(args: list[str]) -> list[tuple[str, str]]:
    """Collect `(lowercase_key, value)` `-o` / `--option` pairs in order before `--`."""
    pairs: list[tuple[str, str]] = []
    expect_option = False
    for arg in args:
        if expect_option:
            pairs.append(_parse_apt_option(arg))
            expect_option = False
        elif arg in ("-o", "--option"):
            expect_option = True
        elif arg.startswith("-o"):
            pairs.append(_parse_apt_option(arg[2:]))
        elif arg.startswith("--option="):
            pairs.append(_parse_apt_option(arg[9:]))
    return pairs


def _args_before_terminator(command: list[str]) -> list[str]:
    """Return command arguments appearing before a `--` option terminator."""
    args = command[1:]
    return args[: args.index("--")] if "--" in args else args


def _is_compliant_refresh_command(command: list[str]) -> bool:
    """Return True when an apt-get refresh sets --error-on=any, retries > 0, no Error-Mode."""
    if posixpath.basename(command[0]) != "apt-get" or "update" not in command[1:]:
        return False
    options = _args_before_terminator(command)
    error_on = [arg for arg in options if arg == "--error-on" or arg.startswith("--error-on=")]
    if not error_on or any(arg != "--error-on=any" for arg in error_on):
        return False
    option_pairs = _extract_apt_options(options)
    if any(key == "apt::update::error-mode" for key, _ in option_pairs):
        return False
    retries = [value for key, value in option_pairs if key == "acquire::retries"]
    return bool(retries) and POSITIVE_INTEGER_RE.fullmatch(retries[-1]) is not None


def _flag_takes_next_arg(arg: str, short_flags: frozenset[str], long_flags: frozenset[str]) -> bool:
    """Return True when `arg` is an option whose argument occupies the following token."""
    if arg.startswith("--"):
        return arg in long_flags
    for index, char in enumerate(arg[1:], 1):
        if f"-{char}" in short_flags:
            return index == len(arg) - 1
    return False


def _drop_env_assignments(args: list[str]) -> list[str]:
    """Return `args` after dropping any leading `NAME=value` words."""
    for index, arg in enumerate(args):
        if ENV_ASSIGNMENT_RE.match(arg) is None:
            return args[index:]
    return []


def _strip_wrapper_prefix(
    args: list[str], short_flags: frozenset[str], long_flags: frozenset[str]
) -> list[str]:
    """Return `args` starting at the wrapped command after flags and `NAME=value` words."""
    skip_next = False
    for index, arg in enumerate(args):
        if skip_next:
            skip_next = False
            continue
        if ENV_ASSIGNMENT_RE.match(arg) is not None:
            continue
        if arg == "--":
            return _drop_env_assignments(args[index + 1 :])
        if arg.startswith("-") and len(arg) > 1:
            skip_next = _flag_takes_next_arg(arg, short_flags, long_flags)
            continue
        return args[index:]
    return []


def _clause_starts_sudo_shell(clause: list[str]) -> bool:
    """Return True when `clause` starts a shell (`bash`, `sh`, `dash`, `zsh`, `ksh`) via `sudo`."""
    sudo_indices = [index for index, token in enumerate(clause) if _is_sudo_token(token)]
    if not sudo_indices:
        return False
    rest = _strip_wrapper_prefix(clause[sudo_indices[0] + 1 :], SUDO_ARG_FLAGS, SUDO_LONG_ARG_FLAGS)
    for _ in range(MAX_APT_WORK_ITEMS):
        if not rest:
            return False
        head = posixpath.basename(rest[0])
        if head == "env":
            rest = _strip_wrapper_prefix(rest[1:], ENV_SHORT_ARG_FLAGS, ENV_LONG_ARG_FLAGS)
        elif head == "sudo":
            rest = _strip_wrapper_prefix(rest[1:], SUDO_ARG_FLAGS, SUDO_LONG_ARG_FLAGS)
        else:
            break
    return bool(rest) and posixpath.basename(rest[0]) in SHELL_BASENAMES


def _clause_apt_problem(
    clause: list[str], hosted_surface: bool, inherited_sudo: bool = False
) -> tuple[bool, str | None]:
    """Inspect one simple shell clause for apt refresh compliance and hosted sudo usage."""
    apt_indices = [index for index, token in enumerate(clause) if _is_apt_token(token)]
    if not apt_indices:
        return False, None
    start = apt_indices[0]
    command = clause[start:]
    if not any(token.lower() == "update" for token in command[1:]):
        return True, None
    if not _is_compliant_refresh_command(command):
        return True, APT_UPDATE_MESSAGE
    has_sudo = inherited_sudo or any(_is_sudo_token(token) for token in clause[:start])
    if hosted_surface and has_sudo:
        return True, HOSTED_APT_UPDATE_MESSAGE
    return True, None


def _is_nested_apt_token(item: str, token: str) -> bool:
    """Return True when `token` is a nested command string mentioning an apt refresh."""
    if token == item or _is_apt_token(token):
        return False
    return APT_UPDATE_LINE.search(token) is not None


def _enqueue_nested_apt_tokens(
    item: str, clause: list[str], inherited_sudo: bool, work: list[tuple[str, bool]]
) -> bool:
    """Queue nested apt command tokens from `clause`; return False if the bound overflows."""
    child_sudo = inherited_sudo or _clause_starts_sudo_shell(clause)
    for token in clause:
        if not _is_nested_apt_token(item, token):
            continue
        if len(work) >= MAX_APT_WORK_ITEMS:
            return False
        work.append((token, child_sudo))
    return True


def _is_env_option_boundary(token: str) -> bool:
    """Return True when `token` ends `env` option parsing (`-`, `--`, `K=V`, or command)."""
    return (
        token in ("-", "--")
        or not token.startswith("-")
        or ENV_ASSIGNMENT_RE.match(token) is not None
    )


def _short_env_split_option(arg: str, next_arg: str | None) -> tuple[list[str], str | None, int]:
    """Extract `(prefix_tokens, split_text, consumed_count)` from a short `env` flag cluster."""
    for offset, char in enumerate(arg[1:], 1):
        if char == "S":
            prefix = [f"-{arg[1:offset]}"] if offset > 1 else []
            if offset < len(arg) - 1:
                return prefix, arg[offset + 1 :], 1
            if next_arg is None:
                return [], None, 1
            return prefix, next_arg, 2
        if f"-{char}" in ENV_SHORT_ARG_FLAGS:
            return [], None, 1
    return [], None, 1


def _env_split_option(arg: str, next_arg: str | None) -> tuple[list[str], str | None, int]:
    """Extract `(prefix_tokens, split_text, consumed_count)` from one `env` option token."""
    if arg == "--split-string":
        return ([], next_arg, 2) if next_arg is not None else ([], None, 1)
    if arg.startswith("--split-string="):
        return [], arg.split("=", 1)[1], 1
    if arg.startswith("--"):
        return [], None, 1
    return _short_env_split_option(arg, next_arg)


def _env_tokens_option_state(tokens: list[str]) -> tuple[bool, bool]:
    """Return `(ended_options, takes_following_arg)` after scanning expanded `env -S` tokens."""
    skip_next = False
    for token in tokens:
        if skip_next:
            skip_next = False
            continue
        if _is_env_option_boundary(token):
            return True, False
        skip_next = _flag_takes_next_arg(token, ENV_SHORT_ARG_FLAGS, ENV_LONG_ARG_FLAGS)
    return False, skip_next


def _expand_one_env_token(clause: list[str], env_index: int) -> tuple[list[str], bool]:
    """Expand split-string options belonging to `clause[env_index]` in one forward pass."""
    args = clause[env_index + 1 :]
    rebuilt = list(clause[: env_index + 1])
    skip_count = 0
    changed = False
    for index, arg in enumerate(args):
        if skip_count > 0:
            skip_count -= 1
            continue
        if _is_env_option_boundary(arg):
            rebuilt.extend(args[index:])
            break
        next_arg = args[index + 1] if index + 1 < len(args) else None
        prefix, split_text, step = _env_split_option(arg, next_arg)
        if split_text is not None:
            split_tokens = _tokenise_shell_item(split_text)
            rebuilt.extend([*prefix, *split_tokens])
            changed = True
            ended, dangling_arg = _env_tokens_option_state(split_tokens)
            if ended:
                rebuilt.extend(args[index + step :])
                break
            extra = int(dangling_arg)
            rebuilt.extend(args[index + step : index + step + extra])
            skip_count = (step - 1) + extra
            continue
        width = 1 + int(_flag_takes_next_arg(arg, ENV_SHORT_ARG_FLAGS, ENV_LONG_ARG_FLAGS))
        rebuilt.extend(args[index : index + width])
        skip_count = width - 1
    return rebuilt, changed


def _next_expanded_env_clause(clause: list[str], search_from: int) -> tuple[list[str], int | None]:
    """Expand the first `env` split-string at or after `search_from`, if any."""
    for index in range(search_from, len(clause)):
        if posixpath.basename(clause[index]) != "env":
            continue
        candidate, changed = _expand_one_env_token(clause, index)
        if changed:
            return candidate, index
    return clause, None


def _expand_clause_env_splits(clause: list[str]) -> tuple[list[str], bool]:
    """Expand `env` split-string arguments once per `env` token up to MAX_APT_WORK_ITEMS."""
    current = clause
    search_from = 0
    for _ in range(MAX_APT_WORK_ITEMS):
        current, expanded_at = _next_expanded_env_clause(current, search_from)
        if expanded_at is None:
            return current, True
        search_from = expanded_at + 1
    _, remaining = _next_expanded_env_clause(current, search_from)
    return current, remaining is None


def _evaluate_shell_clause(
    item: str,
    raw_clause: list[str],
    hosted_surface: bool,
    inherited_sudo: bool,
    work: list[tuple[str, bool]],
) -> tuple[bool, str | None]:
    """Expand `env -S` splits, enqueue nested `-c` strings, and check one shell clause."""
    try:
        clause, within_bound = _expand_clause_env_splits(raw_clause)
    except ValueError:
        return True, APT_UNBALANCED_QUOTE_MESSAGE
    if not within_bound or not _enqueue_nested_apt_tokens(item, clause, inherited_sudo, work):
        return True, APT_NESTING_MESSAGE
    return _clause_apt_problem(clause, hosted_surface, inherited_sudo)


def _consume_container_run_flags(rest: str) -> str | None:
    """Strip leading `--flag[=value]` words from a containerfile `RUN` payload."""
    for _ in range(MAX_CONTINUATION_LINES):
        if not rest.startswith("--"):
            return rest
        flag, *tail = rest.split(None, 1)
        if not tail or CONTAINER_RUN_FLAG_RE.fullmatch(flag) is None:
            return None
        rest = tail[0].lstrip()
    return None


def _json_string_list(raw: str) -> list[str] | None:
    """Decode `raw` when it is a JSON array whose entries are all strings."""
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        return None
    except RecursionError:
        return None
    if not isinstance(parsed, list):
        return None
    argv = [entry for entry in parsed if isinstance(entry, str)]
    return argv if len(argv) == len(parsed) else None


def _parse_exec_run_argv(line_text: str) -> list[str] | None:
    """Parse a containerfile exec-form `RUN` instruction into a string argument vector."""
    prefix = CONTAINER_EXEC_RUN_RE.match(line_text)
    if prefix is None:
        return None
    rest = _consume_container_run_flags(line_text[prefix.end() :].lstrip())
    if rest is None or not rest.startswith("["):
        return None
    return _json_string_list(rest)


def _work_item_clauses(item: str, index: int, exec_argv: list[str] | None) -> list[list[str]]:
    """Return command clauses for one work-list entry without splitting top-level exec argv."""
    if index == 0 and exec_argv is not None:
        return [exec_argv]
    return _split_shell_clauses(_tokenise_shell_item(item))


def _logical_line_apt_problem(
    line_text: str, hosted_surface: bool, exec_argv: list[str] | None = None
) -> str | None:
    """Evaluate one prefiltered logical command with a bounded nested-command work list."""
    work: list[tuple[str, bool]] = [(line_text, False)]
    saw_apt = False
    for index in range(MAX_APT_WORK_ITEMS):
        if index >= len(work):
            break
        item, inherited_sudo = work[index]
        try:
            clauses = _work_item_clauses(item, index, exec_argv)
        except ValueError:
            return APT_UNBALANCED_QUOTE_MESSAGE
        for raw_clause in clauses:
            clause_has_apt, problem = _evaluate_shell_clause(
                item, raw_clause, hosted_surface, inherited_sudo, work
            )
            if clause_has_apt:
                saw_apt = True
            if problem is not None:
                return problem
    if not saw_apt:
        return APT_UPDATE_MESSAGE
    return None


def _line_apt_problem(logical_line: str, hosted_surface: bool, container_file: bool) -> str | None:
    """Evaluate one logical CI line, parsing exec-form `RUN` arrays in containerfiles."""
    exec_argv = _parse_exec_run_argv(logical_line) if container_file else None
    if exec_argv is not None:
        if APT_UPDATE_LINE.search(" ".join(exec_argv)) is None:
            return None
        return _logical_line_apt_problem(logical_line, hosted_surface, exec_argv)
    if APT_UPDATE_LINE.search(logical_line) is None:
        return None
    return _logical_line_apt_problem(logical_line, hosted_surface)


def _yaml_error_line(error: yaml.YAMLError) -> int:
    """Return the 1-based line number from a PyYAML error mark, or 1 when unmarked."""
    mark = getattr(error, "problem_mark", None) or getattr(error, "context_mark", None)
    return int(mark.line) + 1 if mark is not None else 1


def _is_multiline_run_scalar(node: yaml.ScalarNode, lines: list[str]) -> bool:
    """Return True when a non-literal `run` scalar spans multiple physical content lines."""
    if node.style == "|":
        return False
    if node.style == ">":
        # PyYAML sets a block scalar's start_mark on the `>` header line and end_mark
        # past the trailing newline, so count non-blank body lines directly.
        end_line = int(node.end_mark.line)
        if int(node.end_mark.column) > 0:
            end_line += 1
        body = lines[int(node.start_mark.line) + 1 : end_line]
        return sum(1 for line in body if line.strip()) > 1
    return int(node.end_mark.line) > int(node.start_mark.line)


def _mapping_duplicate_key_problems(mapping: yaml.MappingNode, path: Path) -> list[str]:
    """Return findings for repeated scalar keys within one YAML mapping node."""
    problems: list[str] = []
    seen_keys: set[str] = set()
    for key_node, _ in mapping.value:
        if not isinstance(key_node, yaml.ScalarNode):
            continue
        if key_node.value in seen_keys:
            line = int(key_node.start_mark.line) + 1
            problems.append(f"{path.as_posix()}:{line}: {HOSTED_YAML_DUPLICATE_KEY_MESSAGE}")
        else:
            seen_keys.add(key_node.value)
    return problems


def _mapping_run_problems(mapping: yaml.MappingNode, path: Path, lines: list[str]) -> list[str]:
    """Return folded-`run` findings for one YAML mapping node."""
    problems: list[str] = []
    for key_node, val_node in mapping.value:
        if not (isinstance(key_node, yaml.ScalarNode) and key_node.value == "run"):
            continue
        if not isinstance(val_node, yaml.ScalarNode):
            continue
        if not _is_multiline_run_scalar(val_node, lines):
            continue
        if APT_UPDATE_LINE.search(val_node.value) is not None:
            line = int(val_node.start_mark.line) + 1
            problems.append(f"{path.as_posix()}:{line}: {FOLDED_RUN_APT_MESSAGE}")
    return problems


def _push_child_collections(node: yaml.Node, stack: list[yaml.Node], seen: set[int]) -> None:
    """Push unvisited child collection nodes onto `stack` in reverse source order."""
    children: list[yaml.Node] = []
    if isinstance(node, yaml.SequenceNode):
        children = list(node.value)
    elif isinstance(node, yaml.MappingNode):
        for key_node, val_node in node.value:
            children.extend((key_node, val_node))
    for child in reversed(children):
        if isinstance(child, yaml.CollectionNode) and id(child) not in seen:
            seen.add(id(child))
            stack.append(child)


def _hosted_yaml_run_problems(path: Path, text: str) -> list[str]:
    """Validate YAML parseability, duplicate keys, and folded multi-line `run` apt refreshes."""
    try:
        root = yaml.compose(text, Loader=yaml.SafeLoader)
    except yaml.YAMLError as error:
        return [f"{path.as_posix()}:{_yaml_error_line(error)}: {HOSTED_YAML_PARSE_MESSAGE}"]
    if root is None or not isinstance(root, yaml.CollectionNode):
        return []
    lines = text.splitlines()
    problems: list[str] = []
    stack: list[yaml.Node] = [root]
    seen: set[int] = {id(root)}
    for _ in range(MAX_YAML_NODES):
        if not stack:
            return problems
        current = stack.pop()
        if isinstance(current, yaml.MappingNode):
            problems.extend(_mapping_duplicate_key_problems(current, path))
            problems.extend(_mapping_run_problems(current, path, lines))
        _push_child_collections(current, stack, seen)
    return [*problems, f"{path.as_posix()}:1: {HOSTED_YAML_NODE_BOUND_MESSAGE}"]


def apt_update_problems(path: Path, text: str) -> list[str]:
    """Reject unsafe apt refreshes, folded hosted `run` blocks, and hosted sudo refreshes."""
    problems: list[str] = []
    hosted_surface = _is_hosted_workflow_or_action(path)
    container_file = _is_containerfile_path(path)
    if hosted_surface:
        problems.extend(_hosted_yaml_run_problems(path, text))
    for line_number, logical_line, overflowed in _logical_ci_lines(text):
        if overflowed:
            problems.append(f"{path.as_posix()}:{line_number}: {APT_CONTINUATION_MESSAGE}")
            continue
        problem = _line_apt_problem(logical_line, hosted_surface, container_file)
        if problem is not None:
            problems.append(f"{path.as_posix()}:{line_number}: {problem}")
    return problems


def _scan_ci_text(root: Path, check: Callable[[Path, str], list[str]]) -> list[str]:
    """Run one pure text check across tracked CI files; fail closed on non-regular paths."""
    problems: list[str] = []
    for path in ci_text_paths(root):
        relative = path.relative_to(root)
        if not path.is_file(follow_symlinks=False):
            problems.append(f"{relative.as_posix()}: {NON_REGULAR_CI_TEXT_MESSAGE}")
            continue
        problems.extend(check(relative, path.read_text(encoding="utf-8", errors="replace")))
    return problems


def launchpad_ppa_problems(root: Path) -> list[str]:
    """Ban registration text in tracked sources; fail closed on non-regular paths."""
    return _scan_ci_text(root, ppa_text_problems)


def apt_update_contract_problems(root: Path) -> list[str]:
    """Require fail-closed flags and shared refresh helper across tracked CI text files."""
    return _scan_ci_text(root, apt_update_problems)


UBUNTU_DEV_DOCKERFILES = tuple(
    ROOT / f"docker/dev/ubuntu-26.04{variant}.Dockerfile" for variant in ("", "-cuda", "-sycl")
)
APT_SNAPSHOT_FILE = "/etc/apt/apt.conf.d/50ubuntu-snapshot"
APT_SNAPSHOT_WRITE = [
    "printf",
    'APT::Snapshot "%s";\\n',
    "$UBUNTU_APT_SNAPSHOT",
    ">",
    APT_SNAPSHOT_FILE,
]
APT_SNAPSHOT_CHECK = ["grep", "-Eqx", 'APT::Snapshot "[0-9]{8}T[0-9]{6}Z";', APT_SNAPSHOT_FILE]
APT_CERTIFICATE_BOOTSTRAP = [
    ["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"],
    ["apt-get", "install", "-y", "--no-install-recommends", "ca-certificates"],
]


def _snapshot_commands(
    text: str,
) -> tuple[list[tuple[int, list[str]]], list[str], list[str]]:
    """Read bounded Dockerfile shell instructions, the operator ending each, and problems."""
    commands: list[tuple[int, list[str]]] = []
    ends: list[str] = []
    problems: list[str] = []
    for number, line, overflowed in _logical_ci_lines(text):
        if overflowed:
            problems.append(f"{number}: {APT_CONTINUATION_MESSAGE}")
        match = re.match(r"\s*(RUN|COPY|FROM)\s+(.*)", line, re.IGNORECASE | re.DOTALL)
        if match is None:
            continue
        try:
            tokens = _tokenise_shell_item(match[2])
        except ValueError:
            problems.append(f"{number}: snapshot contract requires balanced shell quotes")
            continue
        if match[1].upper() == "RUN":
            for clause, end in _shell_clauses_with_operators(tokens):
                commands.append((number, clause))
                ends.append(end)
        else:
            commands.append((number, [match[1].upper(), *tokens]))
            ends.append("")
    return commands, ends, problems


def _snapshot_staging_dir(commands: list[tuple[int, list[str]]], pin: int) -> str | None:
    """Return the directory the image copies build-config.env into before the pin."""
    for _, cmd in commands[:pin]:
        if cmd[0] == "COPY" and IMAGE_BUILD_CONFIG_NAME in cmd[1:-1] and cmd[-1].endswith("/"):
            return cmd[-1]
    return None


def _snapshot_input_problems(commands: list[tuple[int, list[str]]], pin: int) -> list[str]:
    """Require the pin value to originate from the copied shared build configuration."""
    problems: list[str] = []
    pin_line = commands[pin][0]
    staging = _snapshot_staging_dir(commands, pin)
    if staging is None:
        problems.append("COPY build-config.env into a directory before configuring the snapshot")
    elif not any(
        line == pin_line and cmd == [".", staging + IMAGE_BUILD_CONFIG_NAME]
        for line, cmd in commands[:pin]
    ):
        problems.append("source build-config.env in the snapshot configuration RUN")
    if sum(cmd[0] == "FROM" for _, cmd in commands) != 1:
        problems.append("snapshot contract requires one base stage")
    return problems


def _snapshot_apt_override(cmd: list[str]) -> bool:
    """Recognise command-local options that could replace the persistent snapshot."""
    return any(
        "snapshot" in arg.lower()
        or arg in ("-S", "-c", "--config-file")
        or arg.startswith(("APT_CONFIG=", "--config-file="))
        for arg in cmd
    )


def _snapshot_script_problems(cmd: list[str], position: int) -> list[str]:
    """Keep installer execution after configuration and prevent later config removal."""
    problems: list[str] = []
    runs_installer = cmd[0] not in DOCKERFILE_INSTRUCTIONS and any(
        posixpath.basename(token) in IMAGE_INSTALLER_NAMES for token in cmd
    )
    if position < 0 and runs_installer:
        problems.append("configure the snapshot before calling package installer scripts")
    rewrites = cmd != APT_SNAPSHOT_CHECK and any("/etc/apt/apt.conf" in token for token in cmd)
    if position > 0 and rewrites:
        problems.append("do not change or remove the persistent snapshot after configuration")
    return problems


def _snapshot_operation_problems(commands: list[tuple[int, list[str]]], pin: int) -> list[str]:
    """Bound the live bootstrap and prevent package commands bypassing the pin."""
    problems: list[str] = []
    live = []
    for index, (_, cmd) in enumerate(commands):
        if any(re.search(r"\bapt(?:-get)?\s", token) for token in cmd):
            problems.append("package commands must be direct Dockerfile shell clauses")
        package = any(_is_apt_token(token) for token in cmd)
        if index < pin and package:
            live.append(cmd)
        if package and any(action in cmd for action in ("update", "install")):
            if _snapshot_apt_override(cmd):
                problems.append("package operation must inherit the persistent snapshot")
        problems.extend(_snapshot_script_problems(cmd, index - pin))
    if live != APT_CERTIFICATE_BOOTSTRAP:
        problems.append("only the named ca-certificates bootstrap may use the live archive")
    return problems


def _snapshot_check_masked(
    commands: list[tuple[int, list[str]]], ends: list[str], write: int
) -> bool:
    """Report an operator that lets the RUN skip the timestamp check or pass a failed one."""
    line = commands[write + 1][0]
    ops = [ends[i] for i in range(write + 1, len(commands)) if commands[i][0] == line]
    return ends[write] not in {"&&", ""} or ops != ["&&"] * (len(ops) - 1) + [""]


def ubuntu_apt_snapshot_problems(text: str) -> list[str]:
    """Require a persistent pin before every package operation except the named bootstrap."""
    commands, ends, problems = _snapshot_commands(text)
    writes = [i for i, (_, cmd) in enumerate(commands) if cmd == APT_SNAPSHOT_WRITE]
    if len(writes) != 1:
        return [*problems, "write UBUNTU_APT_SNAPSHOT to persistent APT::Snapshot exactly once"]
    problems.extend(_snapshot_input_problems(commands, writes[0]))
    problems.extend(_snapshot_operation_problems(commands, writes[0]))
    if [cmd for _, cmd in commands[writes[0] + 1 : writes[0] + 2]] != [APT_SNAPSHOT_CHECK]:
        problems.append("check the written APT::Snapshot timestamp before any package operation")
    elif _snapshot_check_masked(commands, ends, writes[0]):
        problems.append("join the APT::Snapshot timestamp check and every later clause with &&")
    if "# UBUNTU_APT_CERTIFICATE_BOOTSTRAP:" not in text:
        problems.append("name the live archive certificate bootstrap explicitly")
    return problems


BUILD_CONFIG_SNAPSHOT = re.compile(r'UBUNTU_APT_SNAPSHOT="[0-9]{8}T[0-9]{6}Z"')


def build_config_snapshot_problems(text: str) -> list[str]:
    """Allow one UBUNTU_APT_SNAPSHOT line, in the only shape the Renovate manager updates."""
    named = [
        line
        for line in text.splitlines()
        if "UBUNTU_APT_SNAPSHOT" in line and not line.lstrip().startswith("#")
    ]
    if len(named) != 1:
        return [f"name UBUNTU_APT_SNAPSHOT on exactly one line, found {len(named)}"]
    if BUILD_CONFIG_SNAPSHOT.fullmatch(named[0]) is None:
        return [f'write the snapshot as UBUNTU_APT_SNAPSHOT="YYYYMMDDTHHMMSSZ": {named[0]!r}']
    return []


class DisposableRepoTestCase(unittest.TestCase):
    """Provide an isolated temporary Git repository for tracked-path contract tests."""

    def setUp(self) -> None:
        """Create disposable Git state isolated from caller repository and configuration."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        # Strip every inherited GIT_* variable and disable caller system/global configuration.
        self.git_run = partial(subprocess.run, env=listing_git_environment(self.root))
        self.git_run(self.git_argv("init", "-q"), check=True, capture_output=True, timeout=10)
        self.enterContext(mock.patch.object(subprocess, "run", side_effect=self.git_run))

    def git_argv(self, *args: str) -> list[str]:
        """Return Git arguments confined to disposable state, hooks and signing disabled."""
        return [
            "git",
            "-c",
            "core.hooksPath=/dev/null",
            "-c",
            "commit.gpgsign=false",
            "-C",
            str(self.root),
            *args,
        ]

    def track(self, files: dict[str, str]) -> None:
        """Write and index only explicitly supplied disposable files."""
        for relative, content in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        self.git_run(
            self.git_argv("add", "--", *files), check=True, capture_output=True, timeout=10
        )


class LaunchpadPPAContract(DisposableRepoTestCase):
    """Keep tracked installation sources independent of live Launchpad registration."""

    def test_current_tree_has_no_registration_text(self) -> None:
        """Every tracked installation source must satisfy ADR-3533."""
        problems = launchpad_ppa_problems(ROOT)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_tracked_sources_name_each_path_and_line(self) -> None:
        """Reject registration text across repository paths, suffixes and container names."""
        paths = (
            ".github/workflows/x.yml",
            ".github/actions/a/action.yaml",
            "tools/elsewhere/action.yml",
            "scripts/ci/setup.sh",
            "scripts/setup.bash",
            "docker/Dockerfile.ci",
            "Containerfile",
            "docs/example.yaml",
            "Dockerfile",
            "docker/Containerfile.ci",
            "docker/dev/x.Dockerfile",
            "docker/dev/x.Containerfile",
        )
        self.track(dict.fromkeys(paths, "# setup\nsudo add-apt-repository -y ppa:x/y\n"))
        expected = [
            f"{path}:2: add-apt-repository registers a Launchpad PPA source; "
            "install from the Ubuntu archive (ADR-3533)"
            for path in sorted(paths)
        ]
        self.assertEqual(launchpad_ppa_problems(self.root), expected)

    def test_registration_forms_and_source_lines(self) -> None:
        """Reject nested commands, aliases, variables, merges, heredocs and comments."""
        for text, matches in (
            ("run: add-apt-repository -y ppa:x/y\n", ((1, "add-apt-repository"),)),
            ("run: bash -c 'add-apt-repository -y ppa:x/y'\n", ((1, "add-apt-repository"),)),
            ('src=ppa:x/y\nadd-apt-repository "$src"\n', ((1, "ppa:"), (2, "add-apt-repository"))),
            (
                "step: &step\n  run: add-apt-repository ppa:x/y\nsteps:\n  - <<: *step\n",
                ((2, "add-apt-repository"),),
            ),
            ("cat <<'EOF'\nadd-apt-repository ppa:x/y\nEOF\n", ((2, "add-apt-repository"),)),
            ("apt-add-repository -y ppa:x/y\n", ((1, "apt-add-repository"),)),
            (
                "deb https://ppa.launchpadcontent.net/x/y/ubuntu noble main\n",
                ((1, "ppa.launchpadcontent.net"),),
            ),
            ("deb https://ppa.launchpad.net/x/y/ubuntu noble main\n", ((1, "ppa.launchpad.net"),)),
            ("PPA:x/y\n", ((1, "PPA:"),)),
            ("ADD-APT-REPOSITORY ppa:x/y\n", ((1, "ADD-APT-REPOSITORY"),)),
            ("# add-apt-repository ppa:x/y\n", ((1, "add-apt-repository"),)),
            ('echo "a\nppa:x/y"\n', ((2, "ppa:"),)),
        ):
            with self.subTest(text=text):
                self.track({"fixture.yml": text})
                expected = [
                    f"fixture.yml:{line}: {match} registers a Launchpad PPA source; "
                    "install from the Ubuntu archive (ADR-3533)"
                    for line, match in matches
                ]
                self.assertEqual(ppa_text_problems(Path("fixture.yml"), text), expected)
                self.assertEqual(launchpad_ppa_problems(self.root), expected)

    def test_unquoted_output_is_banned(self) -> None:
        """Fail closed: unquoted echo data counts, with one finding per matching line."""
        problems = ppa_text_problems(Path("fixture.sh"), "echo add-apt-repository ppa:x/y\n")
        self.assertEqual(
            problems,
            [
                "fixture.sh:1: add-apt-repository registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            ],
        )

    def test_allowed_text_and_untracked_or_other_files(self) -> None:
        """Allow archive installs, empty or malformed text, and files outside tracked scope."""
        self.track(
            {
                "setup.sh": 'Launchpad\napt-get install -y gcc-15\necho "a\nb"\n',
                "empty.yaml": "",
                "example.py": "add-apt-repository ppa:x/y\n",
                "example.md": "add-apt-repository ppa:x/y\n",
                "docker/OtherDockerfile": "add-apt-repository ppa:x/y\n",
            }
        )
        (self.root / "untracked.yml").write_text("ppa:x/y\n", encoding="utf-8")
        self.assertEqual(launchpad_ppa_problems(self.root), [])

    def test_file_count_boundary(self) -> None:
        """Accept exactly the tracked-file bound; reject one additional source path."""
        names = [f"setup-{number}.sh" for number in range(MAX_CI_TEXT_FILES + 1)]
        listing = "\0".join(names[:-1]) + "\0"
        result = TextCommandResult(argv=("git",), returncode=0, stdout=listing, stderr="")
        module = sys.modules[__name__]
        with mock.patch.object(module, "run_command", return_value=result) as run:
            self.assertEqual(len(ci_text_paths(self.root)), MAX_CI_TEXT_FILES)
            run.assert_called_once_with(
                ["git", "-C", str(self.root), "ls-files", "-z"],
                allowed_executables=("git",),
                env=listing_git_environment(self.root),
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=SCRIPT_TIMEOUT_S,
            )
            run.return_value = TextCommandResult(
                argv=("git",), returncode=0, stdout=listing + names[-1] + "\0", stderr=""
            )
            with self.assertRaisesRegex(AssertionError, f"{MAX_CI_TEXT_FILES} files"):
                ci_text_paths(self.root)

    def test_inherited_git_state_cannot_reach_disposable_repository(self) -> None:
        """Poisoned caller repository, index and configuration variables stay outside fixtures."""
        bad_config = self.root.parent / f"{self.root.name}-bad.gitconfig"
        bad_config.write_text("[broken\n", encoding="utf-8")
        self.addCleanup(bad_config.unlink)
        poison = {
            "GIT_DIR": str(self.root / "missing.git"),
            "GIT_WORK_TREE": str(self.root / "missing"),
            "GIT_INDEX_FILE": str(self.root / "missing.index"),
            "GIT_PREFIX": "missing/",
            "GIT_CONFIG_GLOBAL": str(bad_config),
            "GIT_CONFIG_SYSTEM": str(bad_config),
            "GIT_CONFIG_COUNT": "bogus",
        }
        with mock.patch.dict(os.environ, poison):
            self.setUp()
            self.track({"setup.sh": "add-apt-repository ppa:x/y\n"})
            problems = launchpad_ppa_problems(self.root)
        self.assertEqual(
            problems,
            [
                "setup.sh:1: add-apt-repository registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            ],
        )

    def test_caller_global_configuration_cannot_reach_fixture_listing(self) -> None:
        """A malformed caller ~/.gitconfig or XDG Git configuration cannot break fixture scans."""
        self.track({"setup.sh": "add-apt-repository ppa:x/y\n"})
        home = self.root.parent / f"{self.root.name}-home"
        (home / ".config" / "git").mkdir(parents=True)
        self.addCleanup(shutil.rmtree, home)
        (home / ".gitconfig").write_text("[broken\n", encoding="utf-8")
        (home / ".config" / "git" / "config").write_text("[broken\n", encoding="utf-8")
        with mock.patch.dict(
            os.environ, {"HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config")}
        ):
            problems = launchpad_ppa_problems(self.root)
        self.assertEqual(
            problems,
            [
                "setup.sh:1: add-apt-repository registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            ],
        )

    def test_this_checkout_keeps_caller_git_environment(self) -> None:
        """The real checkout keeps hook state; only fixtures drop caller configuration."""
        with mock.patch.dict(os.environ, {"GIT_INDEX_FILE": "hook.index"}):
            self.assertEqual(listing_git_environment(ROOT), dict(os.environ))

    def test_git_listing_failure_propagates(self) -> None:
        """A failed tracked-path lookup cannot silently pass the contract."""
        with self.assertRaises(CommandFailed):
            launchpad_ppa_problems(self.root / "missing")

    def test_non_regular_tracked_path_is_reported(self) -> None:
        """A listed non-regular tracked path fails closed without printing to stderr."""
        self.track({"missing.sh": ""})
        (self.root / "missing.sh").unlink()
        expected = [f"missing.sh: {NON_REGULAR_CI_TEXT_MESSAGE}"]
        with mock.patch("sys.stderr") as stderr:
            self.assertEqual(launchpad_ppa_problems(self.root), expected)
            self.assertEqual(apt_update_contract_problems(self.root), expected)
        stderr.write.assert_not_called()

    def test_tracked_symlink_fails_without_reading_the_target(self) -> None:
        """A tracked link fails closed as non-regular without reading the target text."""
        target = self.root / "target.txt"
        target.write_text("add-apt-repository ppa:x/y\napt-get update\n", encoding="utf-8")
        try:
            (self.root / "linked.sh").symlink_to(target.name)
        except OSError as error:
            self.skipTest(f"symbolic links unavailable: {error}")
        self.git_run(
            self.git_argv("add", "--", "linked.sh"), check=True, capture_output=True, timeout=10
        )
        expected = [f"linked.sh: {NON_REGULAR_CI_TEXT_MESSAGE}"]
        with mock.patch("sys.stderr") as stderr:
            self.assertEqual(launchpad_ppa_problems(self.root), expected)
            self.assertEqual(apt_update_contract_problems(self.root), expected)
        stderr.write.assert_not_called()

    def test_replacement_decoding_preserves_registration_check(self) -> None:
        """Undecodable source bytes cannot hide registration text or crash scanning."""
        self.track({"setup.sh": ""})
        (self.root / "setup.sh").write_bytes(b"# \xff\nppa:x/y\n")
        self.assertEqual(
            launchpad_ppa_problems(self.root),
            [
                "setup.sh:2: ppa: registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            ],
        )

    def test_planted_tidy_registration_fails_contract(self) -> None:
        """Restoring the removed tidy registration fails with its precise source location."""
        relative = ".github/workflows/lint-and-format.yml"
        content = (ROOT / relative).read_text(encoding="utf-8")
        self.track(
            {
                relative: content
                + "\n        sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test\n"
            }
        )
        problems = launchpad_ppa_problems(self.root)
        line = len(content.splitlines()) + 2
        self.assertEqual(
            problems,
            [
                f"{relative}:{line}: add-apt-repository registers a Launchpad PPA source; "
                "install from the Ubuntu archive (ADR-3533)"
            ],
        )
        with self.assertRaisesRegex(AssertionError, rf"lint-and-format\.yml:{line}:"):
            self.assertEqual(problems, [], "\n".join(problems))


class UbuntuAptSnapshotContract(unittest.TestCase):
    """Pin all three Ubuntu dev images, including their nested package installers."""

    def image(self) -> str:
        return UBUNTU_DEV_DOCKERFILES[0].read_text(encoding="utf-8")

    def test_all_three_images_inherit_snapshot(self) -> None:
        for path in UBUNTU_DEV_DOCKERFILES:
            with self.subTest(path=path):
                problems = ubuntu_apt_snapshot_problems(path.read_text(encoding="utf-8"))
                self.assertEqual(problems, [], "\n".join(problems))
                self.assertEqual(
                    apt_update_problems(path.relative_to(ROOT), path.read_text(encoding="utf-8")),
                    [],
                )

    def test_snapshot_is_one_timestamp_in_build_config(self) -> None:
        text = (ROOT / "build-config.env").read_text(encoding="utf-8")
        self.assertEqual(build_config_snapshot_problems(text), [])

    def test_build_config_rejects_a_second_or_reshaped_snapshot(self) -> None:
        """Sourcing takes the last assignment; Renovate updates only the managed shape."""
        managed = 'UBUNTU_APT_SNAPSHOT="20261101T000000Z"'
        others = {
            "export": "export UBUNTU_APT_SNAPSHOT=20261201T000000Z",
            "single quotes": "UBUNTU_APT_SNAPSHOT='20261201T000000Z'",
            "trailing comment": 'UBUNTU_APT_SNAPSHOT="20261201T000000Z"  # newer',
            "indented": '  UBUNTU_APT_SNAPSHOT="20261201T000000Z"',
            "readonly": 'readonly UBUNTU_APT_SNAPSHOT="20261201T000000Z"',
            "default expansion": ': "${UBUNTU_APT_SNAPSHOT:=20261201T000000Z}"',
            "same line": f"{managed}; UBUNTU_APT_SNAPSHOT=20261201T000000Z",
        }
        for name, line in others.items():
            with self.subTest(f"after the managed line: {name}"):
                self.assertNotEqual(build_config_snapshot_problems(f"{managed}\n{line}\n"), [])
            with self.subTest(f"alone: {name}"):
                self.assertNotEqual(build_config_snapshot_problems(f"{line}\n"), [])
        self.assertNotEqual(build_config_snapshot_problems("OTHER=1\n"), [])
        commented = f"# UBUNTU_APT_SNAPSHOT advances weekly\n{managed}\n"
        self.assertEqual(build_config_snapshot_problems(commented), [])

    def test_unpinned_install_is_rejected(self) -> None:
        planted = self.image().replace("RUN apt-get", "RUN apt-get install -y curl && apt-get", 1)
        self.assertIn(
            "only the named ca-certificates bootstrap may use the live archive",
            ubuntu_apt_snapshot_problems(planted),
        )

    def test_bootstrap_allows_only_certificates_once(self) -> None:
        for extra in (" curl", " ca-certificates"):
            with self.subTest(extra=extra):
                planted = self.image().replace(
                    "--no-install-recommends ca-certificates",
                    "--no-install-recommends ca-certificates" + extra,
                    1,
                )
                self.assertTrue(ubuntu_apt_snapshot_problems(planted))
        repeated = self.image().replace(
            "&& . /tmp/build-config.env",
            "&& apt-get install -y --no-install-recommends ca-certificates "
            "&& . /tmp/build-config.env",
        )
        self.assertTrue(ubuntu_apt_snapshot_problems(repeated))
        self.assertEqual(ubuntu_apt_snapshot_problems(self.image()), [])

    def test_pin_inputs_cannot_be_removed(self) -> None:
        for old, new in (
            ("COPY build-config.env", "COPY other.env"),
            ("scripts/setup/ubuntu.sh /tmp/", "scripts/setup/ubuntu.sh /opt/"),
            ("scripts/setup/ubuntu.sh /tmp/", "scripts/setup/ubuntu.sh /tmp"),
            (". /tmp/build-config.env", ":"),
            ("$UBUNTU_APT_SNAPSHOT", "20261101T000000Z"),
            ("# UBUNTU_APT_CERTIFICATE_BOOTSTRAP:", "# bootstrap:"),
        ):
            with self.subTest(removed=old):
                self.assertNotEqual(self.image().replace(old, new), self.image())
                self.assertTrue(ubuntu_apt_snapshot_problems(self.image().replace(old, new)))

    def test_later_operations_cannot_override_or_remove_pin(self) -> None:
        for command in (
            "apt-get --snapshot now install -y curl",
            "apt-get -o APT::Snapshot=now install -y curl",
            "apt-get --config-file=/tmp/live.conf install -y curl",
            "sh -c 'apt-get install -y curl'",
            f"rm {APT_SNAPSHOT_FILE}",
            "rm -rf /etc/apt/apt.conf.d",
        ):
            with self.subTest(command=command):
                self.assertTrue(ubuntu_apt_snapshot_problems(self.image() + f"\nRUN {command}\n"))

    def test_every_package_command_before_pin_is_live(self) -> None:
        """Any apt operation before the pin reads the live archive, not only installs."""
        for command in (
            "apt-get upgrade -y",
            "apt-get dist-upgrade -y",
            "apt-get full-upgrade -y",
            "apt-get remove -y ca-certificates",
            "apt-get build-dep -y curl",
            "apt upgrade -y",
        ):
            with self.subTest(command=command):
                planted = self.image().replace(
                    "&& . /tmp/build-config.env", f"&& {command} && . /tmp/build-config.env", 1
                )
                self.assertNotEqual(planted, self.image())
                self.assertIn(
                    "only the named ca-certificates bootstrap may use the live archive",
                    ubuntu_apt_snapshot_problems(planted),
                )
        after_pin = self.image() + "\nRUN apt-get upgrade -y\n"
        self.assertEqual(ubuntu_apt_snapshot_problems(after_pin), [])

    def test_written_timestamp_is_checked_before_use(self) -> None:
        """The clause after the write checks the file APT reads; it cannot move or go."""
        check = shlex.join(APT_SNAPSHOT_CHECK)
        self.assertIn(f"&& {check} \\\n", self.image())
        moved = self.image().replace(f"&& {check} \\\n", "", 1) + f"\nRUN {check}\n"
        for planted in (self.image().replace(f"&& {check} \\\n", "", 1), moved):
            with self.subTest(planted=planted.count(check)):
                self.assertIn(
                    "check the written APT::Snapshot timestamp before any package operation",
                    ubuntu_apt_snapshot_problems(planted),
                )

    def test_failed_timestamp_check_cannot_be_masked(self) -> None:
        """The check and every later clause join with ``&&``; ``||``, ``;`` or ``|`` mask it."""
        check = shlex.join(APT_SNAPSHOT_CHECK)
        line = f"&& {check} \\\n    && "
        installer = "&& bash /tmp/ubuntu.sh \\\n"
        self.assertIn(line, self.image())
        self.assertIn(installer, self.image())
        planted = {
            "skipped": self.image().replace(line, f"|| {check} \\\n    && ", 1),
            "or true": self.image().replace(line, f"&& {check} || true \\\n    && ", 1),
            "semicolon": self.image().replace(line, f"&& {check} \\\n    ; ", 1),
            "pipe": self.image().replace(line, f"&& {check} | cat \\\n    && ", 1),
            "later or": self.image().replace(installer, "&& bash /tmp/ubuntu.sh || true \\\n", 1),
        }
        for name, text in planted.items():
            with self.subTest(name):
                self.assertNotEqual(text, self.image())
                self.assertIn(
                    "join the APT::Snapshot timestamp check and every later clause with &&",
                    ubuntu_apt_snapshot_problems(text),
                )
        self.assertEqual(ubuntu_apt_snapshot_problems(self.image()), [])

    def test_timestamp_check_rejects_missing_and_malformed_values(self) -> None:
        """Run the image's check on what its printf writes for each build-config value."""
        grep = shutil.which("grep")
        if grep is None:
            self.skipTest("grep not on PATH; the check runs inside the Ubuntu image")
        template = APT_SNAPSHOT_WRITE[1].replace("\\n", "\n")
        with tempfile.TemporaryDirectory() as scratch:
            written = Path(scratch) / "50ubuntu-snapshot"
            for value, expected in (
                ("20261101T000000Z", 0),
                ("", 1),
                ("20261101", 1),
                ("latest", 1),
                ("20261101T000000Z0", 1),
                ("2026-11-01T00:00:00Z", 1),
            ):
                with self.subTest(value=value):
                    written.write_text(template % value, encoding="utf-8")
                    result = run_command(
                        [grep, *APT_SNAPSHOT_CHECK[1:-1], str(written)],
                        allowed_executables=(grep,),
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout_seconds=SCRIPT_TIMEOUT_S,
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)

    def test_installer_cannot_run_before_pin(self) -> None:
        planted = self.image().replace("RUN apt-get", "RUN bash /tmp/ubuntu.sh && apt-get", 1)
        self.assertIn(
            "configure the snapshot before calling package installer scripts",
            ubuntu_apt_snapshot_problems(planted),
        )

    def test_renovate_extracts_only_the_shared_timestamp(self) -> None:
        config = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
        managers = [
            manager
            for manager in config["customManagers"]
            if manager.get("depNameTemplate") == "ubuntu-apt-snapshot"
        ]
        self.assertEqual(len(managers), 1)
        manager = managers[0]
        pattern = re.compile(to_python(manager["matchStrings"][0]))
        text = (ROOT / "build-config.env").read_text(encoding="utf-8")
        matches = list(pattern.finditer(text))
        self.assertEqual(len(matches), 1)
        self.assertIn(f'UBUNTU_APT_SNAPSHOT="{matches[0]["currentValue"]}"', text)
        for invalid in ("20261101", "2026-11-01T000000Z", "latest", ""):
            with self.subTest(invalid=invalid):
                self.assertIsNone(pattern.search(f'UBUNTU_APT_SNAPSHOT="{invalid}"'))
        file_pattern = re.compile(manager["managerFilePatterns"][0][1:-1])
        self.assertIsNotNone(file_pattern.fullmatch("build-config.env"))
        self.assertIsNone(file_pattern.fullmatch("nested/build-config.env"))
        version = re.compile(to_python(manager["versioningTemplate"].removeprefix("regex:")))
        older = version.fullmatch("20261101T000000Z")
        newer = version.fullmatch("20261105T000000Z")
        self.assertIsNotNone(older)
        self.assertIsNotNone(newer)
        if older is not None and newer is not None:
            self.assertLess(int(older["major"]), int(newer["major"]))
        rules = [
            rule
            for rule in config["packageRules"]
            if "custom.ubuntu-apt-snapshot" in rule.get("matchDatasources", [])
        ]
        self.assertEqual(len(rules), 1)
        self.assertEqual(rules[0]["schedule"], ["* 0-5 * * 1"])
        self.assertFalse(rules[0]["automerge"])
        self.assertIn("ubuntu-apt-snapshot", config["customDatasources"])


class AptUpdateFailClosedContract(DisposableRepoTestCase):
    """Require fail-closed index refresh flags on tracked apt-get update invocations."""

    def test_current_tree_apt_updates_fail_closed(self) -> None:
        """Every tracked apt index refresh must satisfy ADR-3537."""
        problems = apt_update_contract_problems(ROOT)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_positive_non_compliant_update_forms(self) -> None:
        """Reject missing flags, bare apt, mixed case, nested bash -c and comments."""
        cases = (
            ("Dockerfile", "FROM ubuntu:26.04\nRUN apt-get update && apt-get install -y x\n", 2),
            ("docker/dev/x.Dockerfile", "FROM ubuntu:26.04\nRUN apt-get update\n", 2),
            ("docker/dev/x.Containerfile", "FROM ubuntu:26.04\nRUN apt-get update\n", 2),
            ("docker/Dockerfile.exec", 'FROM ubuntu:26.04\nRUN ["apt-get", "update"]\n', 2),
            (
                "docker/Containerfile.exec",
                'FROM ubuntu:26.04\nRUN ["sh", "-c", "apt-get update"]\n',
                2,
            ),
            ("scripts/ci/setup.sh", "#!/usr/bin/env bash\n$SUDO apt-get update -qq\n", 2),
            (".github/workflows/ci.yml", "steps:\n  - run: sudo apt-get update\n", 2),
            ("scripts/no-retries.sh", "apt-get update --error-on=any\n", 1),
            ("scripts/no-error-on.sh", "apt-get -o Acquire::Retries=3 update\n", 1),
            ("scripts/bare-apt.sh", "apt update\n", 1),
            ("scripts/bare-apt-flags.sh", "apt --error-on=any -o Acquire::Retries=3 update\n", 1),
            ("scripts/mixed-case.sh", "APT-GET UPDATE\n", 1),
            (
                "scripts/mixed-case-flags.sh",
                "APT-GET --error-on=any -o Acquire::Retries=3 UPDATE\n",
                1,
            ),
            ("scripts/comment.sh", "# run apt-get update before installing\n", 1),
            (
                "scripts/commented-flags.sh",
                "# apt-get --error-on=any -o Acquire::Retries=3 update\n",
                1,
            ),
            (
                "scripts/trailing-comment.sh",
                "apt-get update # --error-on=any -o Acquire::Retries=3\n",
                1,
            ),
            (
                "scripts/bash-c-trailing.sh",
                "bash -c 'apt-get update' --error-on=any -o Acquire::Retries=3\n",
                1,
            ),
            ("scripts/usr-bin.sh", "/usr/bin/apt-get update\n", 1),
            ("scripts/subshell.sh", "(apt-get update)\n", 1),
        )
        for relative, content, line in cases:
            with self.subTest(path=relative):
                expected = [f"{relative}:{line}: {APT_UPDATE_MESSAGE}"]
                self.assertEqual(apt_update_problems(Path(relative), content), expected)
        self.track({relative: content for relative, content, _ in cases})
        expected_all = [
            f"{relative}:{line}: {APT_UPDATE_MESSAGE}"
            for relative, _, line in sorted(cases, key=lambda item: item[0])
        ]
        self.assertEqual(apt_update_contract_problems(self.root), expected_all)

    def test_negative_compliant_forms_and_out_of_scope_files(self) -> None:
        """Allow compliant options, non-update package tokens, empty files and out-of-scope paths."""
        self.track(
            {
                "Dockerfile": (
                    "FROM ubuntu:26.04\n"
                    "RUN apt-get --error-on=any -o Acquire::Retries=3 update \\\n"
                    "    && apt-get install -y --no-install-recommends curl\n"
                    "RUN apt-get update --error-on=any -o Acquire::Retries=3 && apt-get install -y git\n"
                    'RUN ["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]\n'
                    "RUN --mount=type=cache,target=/var/cache/apt "
                    '["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]\n'
                ),
                "scripts/ci/setup.sh": (
                    "$SUDO apt-get -o Acquire::Retries=3 --error-on=any update -qq\n"
                    "$SUDO apt-get update -qq --error-on=any -o Acquire::Retries=3\n"
                    "DEBIAN_FRONTEND=noninteractive /usr/bin/apt-get --error-on=any -o Acquire::Retries=3 update -qq\n"
                    "apt-get install -y --no-install-recommends ca-certificates\n"
                    "apt-get install -y update-notifier-common\n"
                    "apt-get install -y ./update.sh update+x updates\n"
                ),
                ".github/workflows/ci.yml": (
                    "steps:\n"
                    "  - run: bash scripts/ci/apt-install-cached.sh refresh\n"
                    "  - run: docker run --rm ubuntu:26.04 apt-get --error-on=any -o Acquire::Retries=3 update\n"
                ),
                "empty.yaml": "",
                "docs/guide.md": "apt-get update\n",
                "scripts/helper.py": "cmd = 'apt-get update'\n",
                "docker/OtherDockerfile": "RUN apt-get update\n",
            }
        )
        (self.root / "untracked.sh").write_text("apt-get update\n", encoding="utf-8")
        self.assertEqual(apt_update_contract_problems(self.root), [])

    def test_containerfile_exec_run_contract(self) -> None:
        """Evaluate compliant, non-compliant, wrapped and single-argv exec-form `RUN` arrays."""
        refresh = "apt-get --error-on=any -o Acquire::Retries=3 update"
        exec_ok = 'RUN ["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]\n'
        for container_path in (
            "Dockerfile",
            "Containerfile",
            "docker/dev/x.Dockerfile",
            "docker/dev/x.Containerfile",
        ):
            with self.subTest(container_path=container_path):
                self.assertEqual(apt_update_problems(Path(container_path), exec_ok), [])
        for compliant in (
            'run ["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]\n',
            f'RUN ["sh", "-c", "{refresh}"]\n',
            'RUN ["sudo", "apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]\n',
            f'RUN ["env", "-S", "{refresh}"]\n',
        ):
            with self.subTest(compliant=compliant):
                self.assertEqual(apt_update_problems(Path("Dockerfile"), compliant), [])
        for non_compliant in (
            'RUN ["apt-get", "update"]\n',
            'RUN ["sh", "-c", "apt-get update"]\n',
            'RUN ["sudo", "apt-get", "update"]\n',
            'RUN ["env", "-S", "apt-get update"]\n',
            'RUN ["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update", '
            '"&&", "-o", "Acquire::Retries=0"]\n',
        ):
            with self.subTest(non_compliant=non_compliant):
                self.assertEqual(
                    apt_update_problems(Path("Dockerfile"), non_compliant),
                    [f"Dockerfile:1: {APT_UPDATE_MESSAGE}"],
                )

    def test_containerfile_exec_run_boundaries_and_yaml(self) -> None:
        """Accept `--flag` prefixes, fall back on invalid JSON, and leave YAML unchanged."""
        exec_args = '["apt-get", "--error-on=any", "-o", "Acquire::Retries=3", "update"]'
        for compliant in (
            f"RUN --mount=type=cache,target=/var/cache/apt {exec_args}\n",
            f"RUN --mount=type=cache,target=/var/cache/apt --network=default {exec_args}\n",
            'RUN --mount=type=cache,target=/var/cache/apt ["python3", "scripts/update.py"]\n',
        ):
            with self.subTest(compliant=compliant):
                self.assertEqual(apt_update_problems(Path("Dockerfile"), compliant), [])
        self.assertEqual(
            apt_update_problems(
                Path("Dockerfile"),
                'RUN --mount=type=cache,target=/var/cache/apt ["apt-get", "update"]\n',
            ),
            [f"Dockerfile:1: {APT_UPDATE_MESSAGE}"],
        )
        for invalid_json, expected_message in (
            ('RUN ["apt-get", update]\n', APT_UPDATE_MESSAGE),
            ('RUN ["apt-get", "update]\n', APT_UNBALANCED_QUOTE_MESSAGE),
            ('RUN [1, "apt-get", "update"]\n', APT_UPDATE_MESSAGE),
        ):
            with self.subTest(invalid_json=invalid_json):
                shell_verdict = _logical_line_apt_problem(invalid_json.rstrip("\n"), False)
                self.assertEqual(shell_verdict, expected_message)
                self.assertEqual(
                    apt_update_problems(Path("Dockerfile"), invalid_json),
                    [f"Dockerfile:1: {shell_verdict}"],
                )
        yaml_exec = f"steps:\n  - run: |\n      RUN {exec_args}\n"
        self.assertEqual(
            apt_update_problems(Path(".github/workflows/ci.yml"), yaml_exec),
            [f".github/workflows/ci.yml:3: {APT_UPDATE_MESSAGE}"],
        )

    def test_boundary_two_updates_on_one_line(self) -> None:
        """Reject a line holding two update calls unless both carry fail-closed flags."""
        for text in (
            "apt-get update && apt-get --error-on=any -o Acquire::Retries=3 update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 update && apt-get update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 update; apt update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 update || apt-get update\n",
            "(apt-get --error-on=any -o Acquire::Retries=3 update) & (apt-get update)\n",
        ):
            with self.subTest(text=text):
                self.track({"boundary.sh": text})
                self.assertEqual(
                    apt_update_contract_problems(self.root),
                    [f"boundary.sh:1: {APT_UPDATE_MESSAGE}"],
                )
        both_compliant = (
            "apt-get --error-on=any -o Acquire::Retries=3 update "
            "&& apt-get update --error-on=any -o Acquire::Retries=3\n"
        )
        self.track({"boundary.sh": both_compliant})
        self.assertEqual(apt_update_contract_problems(self.root), [])

    def test_update_split_across_continuation(self) -> None:
        """Join backslash continuations with first-line reporting and bounded physical length."""
        self.track({"split.sh": "apt-get -qq \\   \n  update\n"})
        self.assertEqual(
            apt_update_contract_problems(self.root),
            [f"split.sh:1: {APT_UPDATE_MESSAGE}"],
        )
        compliant_split = (
            "#!/usr/bin/env bash\n"
            "apt-get \\\n"
            "  --error-on=any \\\n"
            "  -o Acquire::Retries=3 \\\n"
            "  update\n"
        )
        self.track({"split.sh": compliant_split})
        self.assertEqual(apt_update_contract_problems(self.root), [])
        at_bound = "echo \\\n" * (MAX_CONTINUATION_LINES - 1) + "ok\n"
        self.track({"split.sh": at_bound})
        self.assertEqual(apt_update_contract_problems(self.root), [])
        over_bound = "echo \\\n" * MAX_CONTINUATION_LINES + "ok\n"
        self.track({"split.sh": over_bound})
        self.assertEqual(
            apt_update_contract_problems(self.root),
            [f"split.sh:1: {APT_CONTINUATION_MESSAGE}"],
        )

    def test_effective_acquire_retries_and_option_spellings(self) -> None:
        """Require positive final Acquire::Retries and forbid APT::Update::Error-Mode overrides."""
        for compliant in (
            "apt-get --error-on=any -o Acquire::Retries=0 -o Acquire::Retries=3 update\n",
            "apt-get --error-on=any -oAcquire::Retries=3 update\n",
            "apt-get --error-on=any --option Acquire::Retries=3 update\n",
            "apt-get --error-on=any --option=Acquire::Retries=3 update\n",
        ):
            with self.subTest(compliant=compliant):
                self.assertEqual(apt_update_problems(Path("s.sh"), compliant), [])
        for non_compliant in (
            "apt-get --error-on=any -o Acquire::Retries=3 -o Acquire::Retries=0 update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 --option=Acquire::Retries=0 update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 -o acquire::retries=0 update\n",
            "apt-get --error-on=any -o Acquire::Retries= update\n",
            "apt-get --error-on=any -o Acquire::Retries update\n",
            "apt-get update -- --error-on=any -o Acquire::Retries=3\n",
            "apt-get --error-on=any -o Acquire::Retries=3 -o APT::Update::Error-Mode=persistent update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 -oAPT::Update::Error-Mode=any update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 --option APT::Update::Error-Mode=persistent update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 --option=apt::update::error-mode=persistent update\n",
            "apt-get -o APT::Update::Error-Mode=persistent --error-on=any -o Acquire::Retries=3 update\n",
            "apt-get --error-on=any -o Acquire::Retries=3 --error-on=persistent update\n",
        ):
            with self.subTest(non_compliant=non_compliant):
                self.assertEqual(
                    apt_update_problems(Path("s.sh"), non_compliant),
                    [f"s.sh:1: {APT_UPDATE_MESSAGE}"],
                )

    def test_nested_shell_work_list_and_unbalanced_quotes(self) -> None:
        """Inspect nested sh/bash -c command strings up to 8 work items and reject bad quotes."""
        nested = "apt-get --error-on=any -o Acquire::Retries=3 update"
        for _ in range(MAX_APT_WORK_ITEMS - 1):
            nested = f"sh -c {shlex.quote(nested)}"
        self.assertEqual(apt_update_problems(Path("s.sh"), nested + "\n"), [])
        too_deep = f"sh -c {shlex.quote(nested)}\n"
        self.assertEqual(
            apt_update_problems(Path("s.sh"), too_deep),
            [f"s.sh:1: {APT_NESTING_MESSAGE}"],
        )
        for unbalanced in (
            "bash -c 'apt-get --error-on=any -o Acquire::Retries=3 update\n",
            'bash -c "apt-get \'update"\n',
        ):
            with self.subTest(unbalanced=unbalanced):
                self.assertEqual(
                    apt_update_problems(Path("s.sh"), unbalanced),
                    [f"s.sh:1: {APT_UNBALANCED_QUOTE_MESSAGE}"],
                )
        self.assertEqual(apt_update_problems(Path("s.sh"), "echo 'no apt here\n"), [])

    def test_hosted_runner_sudo_refresh_contract(self) -> None:
        """Require hosted workflow and action refreshes to use apt-install-cached.sh."""
        refresh = "apt-get --error-on=any -o Acquire::Retries=3 update"
        for hosted_path in (
            ".github/workflows/build.yml",
            ".github/workflows/extra.yaml",
            ".github/actions/gen-node-bpf/action.yml",
            ".github/actions/nested/tool/action.yaml",
        ):
            with self.subTest(hosted_path=hosted_path):
                text = f"steps:\n  - run: sudo {refresh}\n"
                self.assertEqual(
                    apt_update_problems(Path(hosted_path), text),
                    [f"{hosted_path}:2: {HOSTED_APT_UPDATE_MESSAGE}"],
                )
        for nested_sudo in (
            f"run: bash -c 'sudo -E /usr/bin/{refresh}'\n",
            f"run: sudo bash -c '{refresh}'\n",
            f"run: sudo sh -c '{refresh}'\n",
            f"run: sudo -E bash -c '{refresh}'\n",
            f"run: sudo -u root sh -c '{refresh}'\n",
            f"run: sudo env A=1 bash -c '{refresh}'\n",
            f"run: sudo -g adm -C 3 -D / -h x -p p -r r -t t -U u -T 9 zsh -c '{refresh}'\n",
            f"run: sudo env -i A=1 dash -c 'ksh -c \"{refresh}\"'\n",
            f"run: sudo bash -c \"sh -c '{refresh}'\"\n",
            f"{refresh} && sudo {refresh}\n",
        ):
            with self.subTest(nested_sudo=nested_sudo):
                self.assertEqual(
                    apt_update_problems(Path(".github/workflows/build.yml"), nested_sudo),
                    [f".github/workflows/build.yml:1: {HOSTED_APT_UPDATE_MESSAGE}"],
                )
        for allowed_path, allowed_text in (
            (
                ".github/workflows/published-rc-licence-companions.yml",
                f"docker run --rm debian bash -c '{refresh} -qq'\n",
            ),
            (".github/workflows/build.yml", f"sudo echo ok && {refresh}\n"),
            (".github/workflows/build.yml", f"sudo echo ok && bash -c '{refresh}'\n"),
            (".github/workflows/build.yml", f"sudo docker run img bash -c '{refresh}'\n"),
            ("scripts/ci/apt-install-cached.sh", f"sudo {refresh}\n"),
            ("scripts/ci/apt-install-cached.sh", f"sudo bash -c '{refresh}'\n"),
            ("tools/elsewhere/action.yml", f"sudo {refresh}\n"),
            ("tools/elsewhere/action.yml", f"sudo bash -c '{refresh}'\n"),
        ):
            with self.subTest(allowed_path=allowed_path, allowed_text=allowed_text):
                self.assertEqual(apt_update_problems(Path(allowed_path), allowed_text), [])

    def test_hosted_yaml_folded_run_contract(self) -> None:
        """Require hosted multi-line `run` refreshes to use literal `|` blocks."""
        workflow = Path(".github/workflows/ci.yml")
        refresh = "apt-get --error-on=any -o Acquire::Retries=3 update"
        for refused in (
            "steps:\n  - run: >-\n      sudo apt-get\n      update\n",
            "steps:\n  - run: >\n      sudo apt-get\n      update\n",
            "steps:\n  - run: sudo apt-get\n      update\n",
            'steps:\n  - run: "sudo apt-get\n      update"\n',
            "steps:\n  - run: 'sudo apt-get\n      update'\n",
        ):
            with self.subTest(refused=refused):
                self.assertEqual(
                    apt_update_problems(workflow, refused),
                    [f".github/workflows/ci.yml:2: {FOLDED_RUN_APT_MESSAGE}"],
                )
        action = Path(".github/actions/setup/action.yml")
        action_folded = "runs:\n  steps:\n    - run: >-\n        sudo apt-get\n        update\n"
        self.assertEqual(
            apt_update_problems(action, action_folded),
            [f".github/actions/setup/action.yml:3: {FOLDED_RUN_APT_MESSAGE}"],
        )
        for accepted in (
            "steps:\n  - run: |\n      bash scripts/ci/apt-install-cached.sh refresh\n",
            "steps:\n  - run: >-\n      bash scripts/test/fetch-test-yuvs.sh\n      a b\n",
            f"steps:\n  - run: >-\n      docker run --rm debian {refresh}\n",
        ):
            with self.subTest(accepted=accepted):
                self.assertEqual(apt_update_problems(workflow, accepted), [])
        sudo_single_folded = f"steps:\n  - run: >-\n      sudo {refresh}\n"
        self.assertEqual(
            apt_update_problems(workflow, sudo_single_folded),
            [f".github/workflows/ci.yml:3: {HOSTED_APT_UPDATE_MESSAGE}"],
        )
        non_hosted = "steps:\n  - run: >-\n      sudo apt-get\n      update\n"
        self.assertEqual(apt_update_problems(Path("tools/elsewhere/action.yml"), non_hosted), [])

    def test_env_split_string_expansion_contract(self) -> None:
        """Expand `env` split-string flags before apt detection and hosted sudo inheritance."""
        workflow = Path(".github/workflows/ci.yml")
        script = Path("s.sh")
        refresh = "apt-get --error-on=any -o Acquire::Retries=3 update"
        self.assertEqual(
            apt_update_problems(workflow, "run: sudo env -S 'bash -c \"apt-get update\"'\n"),
            [f".github/workflows/ci.yml:1: {APT_UPDATE_MESSAGE}"],
        )
        for hosted_sudo in (
            f"run: sudo env -S 'bash -c \"{refresh}\"'\n",
            f"run: sudo env -S '{refresh}'\n",
            f"run: sudo env --split-string='bash -c \"{refresh}\"'\n",
            f"run: sudo env -iS 'bash -c \"{refresh}\"'\n",
        ):
            with self.subTest(hosted_sudo=hosted_sudo):
                self.assertEqual(
                    apt_update_problems(workflow, hosted_sudo),
                    [f".github/workflows/ci.yml:1: {HOSTED_APT_UPDATE_MESSAGE}"],
                )
        for non_compliant in (
            "env -S 'apt-get update'\n",
            "env -S'apt-get update'\n",
            "env -Sapt-get\\ update\n",
            "env --split-string='apt-get update'\n",
            "env --split-string 'apt-get update'\n",
            "env -iS 'apt-get update'\n",
            "env -iS'apt-get update'\n",
        ):
            with self.subTest(non_compliant=non_compliant):
                self.assertEqual(
                    apt_update_problems(script, non_compliant), [f"s.sh:1: {APT_UPDATE_MESSAGE}"]
                )
        for compliant in (
            f"sudo env -S '{refresh}'\n",
            f"sudo env -S'{refresh}'\n",
            f"sudo env --split-string='{refresh}'\n",
            f"sudo env --split-string '{refresh}'\n",
            f"sudo env -iS '{refresh}'\n",
            f"env -S '-C' /tmp -S '{refresh}'\n",
        ):
            with self.subTest(compliant=compliant):
                self.assertEqual(apt_update_problems(script, compliant), [])

    def test_env_split_string_quotes_and_bounds(self) -> None:
        """Report unbalanced quotes inside `env -S` strings and cap chained `-S` passes."""
        script = Path("s.sh")
        refresh = "apt-get --error-on=any -o Acquire::Retries=3 update"
        for unbalanced in ("env -S 'apt-get update\n", "env -S 'apt-get \"update'\n"):
            with self.subTest(unbalanced=unbalanced):
                self.assertEqual(
                    apt_update_problems(script, unbalanced),
                    [f"s.sh:1: {APT_UNBALANCED_QUOTE_MESSAGE}"],
                )
        chained = refresh
        for _ in range(MAX_APT_WORK_ITEMS):
            chained = f"env -S {shlex.quote(chained)}"
        self.assertEqual(apt_update_problems(script, chained + "\n"), [])
        self.assertEqual(
            apt_update_problems(script, f"env -S {shlex.quote(chained)}\n"),
            [f"s.sh:1: {APT_NESTING_MESSAGE}"],
        )

    def test_hosted_yaml_parse_failure_contract(self) -> None:
        """Fail closed when hosted workflow or composite action YAML cannot be parsed."""
        workflow = Path(".github/workflows/ci.yml")
        action = Path(".github/actions/setup/action.yml")
        self.assertEqual(
            apt_update_problems(workflow, "steps:\n  - run: [\n"),
            [f".github/workflows/ci.yml:3: {HOSTED_YAML_PARSE_MESSAGE}"],
        )
        self.assertEqual(
            apt_update_problems(action, "runs: \x00\n"),
            [f".github/actions/setup/action.yml:1: {HOSTED_YAML_PARSE_MESSAGE}"],
        )
        self.assertEqual(
            apt_update_problems(Path("tools/elsewhere/action.yml"), "steps:\n  - run: [\n"),
            [],
        )

    def test_hosted_yaml_duplicate_key_contract(self) -> None:
        """Reject duplicate scalar keys inside one hosted YAML mapping while allowing distinct steps."""
        workflow = Path(".github/workflows/ci.yml")
        action = Path(".github/actions/setup/action.yml")
        duplicate_run = (
            "steps:\n"
            "  - name: refresh\n"
            "    run: bash scripts/ci/apt-install-cached.sh refresh\n"
            "    run: echo override\n"
        )
        self.assertEqual(
            apt_update_problems(workflow, duplicate_run),
            [f".github/workflows/ci.yml:4: {HOSTED_YAML_DUPLICATE_KEY_MESSAGE}"],
        )
        duplicate_steps = (
            "runs:\n"
            "  using: composite\n"
            "  steps:\n"
            "    - run: echo first\n"
            "  steps:\n"
            "    - run: echo second\n"
        )
        self.assertEqual(
            apt_update_problems(action, duplicate_steps),
            [f".github/actions/setup/action.yml:5: {HOSTED_YAML_DUPLICATE_KEY_MESSAGE}"],
        )
        separate_steps = (
            "steps:\n"
            "  - name: first\n"
            "    run: bash scripts/ci/apt-install-cached.sh refresh\n"
            "  - name: second\n"
            "    run: echo ok\n"
        )
        self.assertEqual(apt_update_problems(workflow, separate_steps), [])
        self.assertEqual(apt_update_problems(Path("tools/elsewhere/action.yml"), duplicate_run), [])

    def test_planted_cuda_installer_without_error_on_fails_contract(self) -> None:
        """Removing --error-on=any from install-cuda-toolkit.sh fails at that file and line."""
        relative = "scripts/ci/install-cuda-toolkit.sh"
        content = (ROOT / relative).read_text(encoding="utf-8")
        self.assertIn(" --error-on=any", content)
        mutated = content.replace(" --error-on=any", "", 1)
        line = next(
            number
            for number, (before, after) in enumerate(
                zip(content.splitlines(), mutated.splitlines(), strict=True), 1
            )
            if before != after
        )
        self.track({relative: mutated})
        self.assertEqual(
            apt_update_contract_problems(self.root),
            [f"{relative}:{line}: {APT_UPDATE_MESSAGE}"],
        )


class FailClosedCIContract(unittest.TestCase):
    def test_contract_is_wired_into_required_ci_and_local_hooks(self) -> None:
        """Keep the CI contract in Tooling Tests and local hooks for every tracked installation source."""
        rules = (WORKFLOWS / "rule-enforcement.yml").read_text(encoding="utf-8")
        hooks = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        # A test runs once in CI (ADR-1568): Tooling Tests runs this file.
        sys.path.insert(0, str(ROOT))
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        self.assertIn("scripts/ci/test_fail_closed_ci.py", suite_members(ROOT, "tooling"))
        self.assertNotIn("test_fail_closed_ci.py", rules)
        self.assertIn("id: fail-closed-ci-contract", hooks)
        self.assertRegex(hooks, r"entry: python3(?: -B)? scripts/ci/test_fail_closed_ci\.py")
        hook = hooks.split("      - id: fail-closed-ci-contract\n", maxsplit=1)[1].split(
            "      - id:", maxsplit=1
        )[0]
        pattern = yaml.safe_load(hook)["files"]
        for path in (
            ".github/workflows/lint-and-format.yml",
            ".github/workflows/new-workflow.yaml",
            ".github/actions/nested/example/action.yml",
            ".github/actions/new-action/action.yaml",
            "tools/elsewhere/action.yml",
            "scripts/ci/setup.sh",
            "scripts/setup.bash",
            "docs/example.yaml",
            "Dockerfile",
            "docker/Dockerfile.ci",
            "Containerfile",
            "docker/Containerfile.ci",
            "docker/dev/x.Dockerfile",
            "docker/dev/x.Containerfile",
            "scripts/ci/test_fail_closed_ci.py",
            "python/tox.ini",
            ".pre-commit-config.yaml",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(re.search(pattern, path))
        for path in ("docs/example.md", "scripts/example.py", "docker/OtherDockerfile"):
            with self.subTest(path=path):
                self.assertIsNone(re.search(pattern, path))

    def test_python_tox_warnings_and_coverage_fail_closed(self) -> None:
        tox = (ROOT / "python" / "tox.ini").read_text(encoding="utf-8")
        self.assertNotRegex(tox, r"(?m)^ignore_outcome\s*=\s*true\s*$")
        self.assertNotIn("-p no:warnings", tox)
        self.assertIn("filterwarnings = error", tox)
        coverage = tox.split("[testenv:coverage]", maxsplit=1)[1].split("\n[testenv:", maxsplit=1)[
            0
        ]
        self.assertIn("depends = py314", coverage)
        self.assertNotRegex(coverage, r"(?m)^\s*coverage (?:report|xml|html) .*\s-i(?:\s|$)")

    def test_cpu_coverage_records_and_reasserts_pytest_failure(self) -> None:
        workflow = (WORKFLOWS / "tests-and-quality-gates.yml").read_text(encoding="utf-8")
        self.assertNotRegex(workflow, r"(?m)^    if: false\s*$")
        assertion = workflow_step(workflow, "Assert every Python coverage shard passed")
        self.assertIn('!= "success"', assertion)
        for index in range(1, 5):
            with self.subTest(shard=index):
                pytest_step = workflow_step(workflow, f"Run Python test suite, shard {index}/4")
                self.assertIn(f"id: python_coverage_shard_{index}", pytest_step)
                self.assertIn(f"--shard {index}/4", pytest_step)
                self.assertIn("continue-on-error: true", pytest_step)
                self.assertNotIn("|| true", executable_body(pytest_step))
                self.assertIn(f"steps.python_coverage_shard_{index}.outcome", assertion)
                # #2823 wrote PYTHO1PATH..PYTHO4PATH, so no shard got the path.
                self.assertRegex(
                    pytest_step, r"(?m)^          PYTHONPATH: \$\{\{ github\.workspace \}\}/python$"
                )
                self.assertNotRegex(pytest_step, r"PYTHO\dPATH")

        gpu_coverage = workflow_job(workflow, "coverage-gpu")
        self.assertIn("name: Coverage GPU", gpu_coverage)
        self.assertIn("# required-aggregator", gpu_coverage)
        self.assertNotIn("continue-on-error", gpu_coverage)
        aggregator = (WORKFLOWS / "required-aggregator.yml").read_text(encoding="utf-8")
        self.assertIn("'Coverage GPU',", aggregator)

    def test_coverage_shard_assertion_names_why_a_shard_was_skipped(self) -> None:
        """Assert that the coverage workflow identifies why a skipped shard was skipped."""
        workflow = (WORKFLOWS / "tests-and-quality-gates.yml").read_text(encoding="utf-8")
        assertion = workflow_step(workflow, "Assert every Python coverage shard passed")
        coverage = workflow_job(workflow, "coverage")
        for step_id in re.findall(r"\$\{\{ steps\.(\w+)\.outcome \}\}", assertion):
            with self.subTest(step=step_id):
                self.assertIn(f"        id: {step_id}\n", coverage)
        self.assertIn("steps.coverage_meson_suite.outcome", assertion)
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash not on PATH; the workflow step runs on ubuntu")
        script = step_script(assertion)
        earlier = "Build-with-gcov=success Meson-unit-suite=failure Prepare-Python-suite=skipped"

        def run(shards: str, before: str) -> subprocess.CompletedProcess[str]:
            """Execute the coverage assertion script under bash with the given outcomes."""
            env = {"SHARD_OUTCOMES": shards, "EARLIER_OUTCOMES": before, "PATH": "/usr/bin:/bin"}
            return subprocess.run(  # noqa: S603 - argv is bash plus the tracked workflow step
                [bash, "-c", script],
                env=env,
                capture_output=True,
                text=True,
                check=False,
                timeout=SCRIPT_TIMEOUT_S,
            )

        skipped = run("skipped skipped skipped skipped", earlier)
        self.assertEqual(skipped.returncode, 1)
        self.assertIn(
            "shard 1/4 outcome was skipped: step Meson-unit-suite outcome was failure",
            skipped.stderr,
        )
        self.assertEqual(skipped.stderr.count("::error::"), 4)
        failed = run("success failure success success", earlier.replace("failure", "success"))
        self.assertEqual(failed.returncode, 1)
        self.assertIn("shard 2/4 outcome was failure\n", failed.stderr)
        self.assertNotIn("skipped", failed.stderr)
        passed = run("success success success success", "Meson-unit-suite=success")
        self.assertEqual((passed.returncode, passed.stderr), (0, ""))
        unexplained = run("success skipped success success", "Meson-unit-suite=success")
        self.assertIn("shard 2/4 outcome was skipped: no earlier step failed", unexplained.stderr)

    def test_nightly_benchmark_failure_remains_a_failure(self) -> None:
        workflow = (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8")
        benchmark = workflow_step(workflow, "Run benchmark")
        self.assertIn("VMAF_BIN: ${{ github.workspace }}/build/tools/vmaf", benchmark)
        self.assertIn("bash testdata/bench_all.sh", benchmark)
        self.assertNotIn("|| true", executable_body(benchmark))
        upload = workflow.split("name: nightly-benchmark-results", maxsplit=1)[0]
        # ADR-3368 guards the v7 upload off Gitea; the condition inside stays always().
        self.assertRegex(
            upload,
            r"(?m)^        if: \$\{\{ \(always\(\)\) && env\.GITEA_ACTIONS != 'true' \}\}\s*$",
        )

    def test_nightly_clang_tidy_configuration_and_toolchain(self) -> None:
        """Keep the archive GCC and existing LLVM installer aligned with the CPU baseline."""
        workflow = (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8")
        tidy_job = workflow_job(workflow, "clang-tidy-full")
        self.assertNotIn("ppa:ubuntu-toolchain-r/test", tidy_job)
        self.assertIn("sudo bash scripts/ci/install-llvm.sh 22", tidy_job)
        # The packages come from the job's apt list (apt archive cache).
        self.assertIn(
            "grep -v '^#' .github/apt/clang-tidy-full.txt | xargs sudo apt-get install -y",
            tidy_job,
        )
        apt_list = (ROOT / ".github/apt/clang-tidy-full.txt").read_text(encoding="utf-8")
        self.assertIn("clang-tidy-22", apt_list.splitlines())
        # The job runs the cpu lane's Makefile targets, as the required job and
        # the dev container do (T-TIDY-RATCHET-UNMEASURED-AS-CLEAN-2026-10-06):
        # gcc-15, no LTO and the hashed build requirements come from there.
        self.assertIn("make tidy-ratchet-build LANE=cpu", tidy_job)
        self.assertIn("make tidy-ratchet LANE=cpu", tidy_job)
        self.assertIn("CLANG_TIDY_BIN=/usr/bin/clang-tidy-22", tidy_job)
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("TIDY_RATCHET_COMPILERS_cpu := CC=gcc-15 CXX=g++-15", makefile)
        setup_cpu = makefile.split("TIDY_RATCHET_SETUP_cpu :=", 1)[1].split("\nTIDY_", 1)[0]
        self.assertIn("-Db_lto=false", setup_cpu)
        self.assertIn("install --require-hashes -r requirements/locks/build.txt", makefile)

    def test_changed_tidy_excludes_windows_only_translation_unit(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        tidy_job = workflow_job(workflow, "clang-tidy")
        self.assertIn("^core/tools/test/test_vmaf_windows_utf8_argv\\.cpp$", tidy_job)

    def test_tidy_ratchet_build_stays_outside_repository(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        ratchet_job = workflow_job(workflow, "clang-tidy-ratchet")
        build_step = workflow_step(ratchet_job, "Build (compile_commands.json + generated headers)")
        run_step = workflow_step(
            ratchet_job, "Ratchet — whole-tree clang-tidy vs committed baseline"
        )
        self.assertIn('TIDY_RATCHET_BUILD_DIR="$RUNNER_TEMP/vmafx-tidy-cpu"', build_step)
        self.assertIn("make tidy-ratchet-build LANE=cpu", build_step)
        self.assertIn('TIDY_RATCHET_BUILD_DIR="$RUNNER_TEMP/vmafx-tidy-cpu"', run_step)
        self.assertNotIn("meson setup build core", executable_body(build_step))

    def test_fuzz_workflows_have_adequate_timeout_budget(self) -> None:
        fuzz_wf = (WORKFLOWS / "fuzz.yml").read_text(encoding="utf-8")
        fuzz_job = workflow_job(fuzz_wf, "fuzz")
        self.assertRegex(fuzz_job, r"(?m)^    timeout-minutes: 45\s*$")

        sanitizers_wf = (WORKFLOWS / "sanitizers.yml").read_text(encoding="utf-8")
        sanitizer_fuzz_job = workflow_job(sanitizers_wf, "fuzz-nightly")
        self.assertRegex(sanitizer_fuzz_job, r"(?m)^    timeout-minutes: 55\s*$")

    def test_semgrep_advisory_preserves_failure_outcome(self) -> None:
        workflow = (WORKFLOWS / "security-scans.yml").read_text(encoding="utf-8")
        registry = workflow_step(workflow, "Run semgrep (registry rule packs — advisory)")
        self.assertIn("continue-on-error: true", registry)
        self.assertNotIn("|| true", executable_body(registry))

    def test_python_lint_uses_the_fail_closed_merge_base_gate(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        job = workflow_job(workflow, "python-lint")
        self.assertEqual(python_lint_job_problems(job), [])

    def test_python_lint_contract_detects_fail_open_mutations(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        job = workflow_job(workflow, "python-lint")
        mutations = {
            "shallow checkout": job.replace("fetch-depth: 0", "fetch-depth: 1", 1),
            "unhashed checker": job.replace(" --require-hashes", "", 1),
            "missing push base": job.replace("github.event.before", "github.sha", 1),
            "advisory status": job.replace(
                "python3 scripts/git-hooks/pre-push-mypy.py",
                'python3 scripts/git-hooks/pre-push-mypy.py || echo "advisory"',
                1,
            ),
            "raw directory scan": job.replace(
                "python3 scripts/git-hooks/pre-push-mypy.py", "mypy ai/ scripts/", 1
            ),
        }
        for name, mutated in mutations.items():
            with self.subTest(name=name):
                self.assertTrue(python_lint_job_problems(mutated))

    def test_sanitizer_enumeration_does_not_mask_producer_errors(self) -> None:
        expected_counts = {"tests-and-quality-gates.yml": 1, "sanitizers.yml": 2}
        for workflow_name, expected_count in expected_counts.items():
            workflow = (WORKFLOWS / workflow_name).read_text(encoding="utf-8")
            starts = list(re.finditer(r"TESTS=\$\(meson introspect build --tests", workflow))
            self.assertEqual(expected_count, len(starts), workflow_name)
            for start in starts:
                end = workflow.find('\n          echo "test count:', start.start())
                self.assertNotEqual(-1, end, workflow_name)
                enumeration = workflow[start.start() : end]
                self.assertNotIn("|| true", executable_body(enumeration), workflow_name)
                self.assertNotIn("set +o pipefail", executable_body(enumeration), workflow_name)

    def test_executable_body_drops_comments_but_keeps_commands(self) -> None:
        """A comment naming a fail-open is not one; a real suffix still is."""
        commented = (
            "        run: |\n          # removed the trailing || true here\n          pytest\n"
        )
        self.assertNotIn("|| true", executable_body(commented))
        real = "        run: |\n          # documented\n          pytest || true\n"
        self.assertIn("|| true", executable_body(real))
        inline = "        run: pytest || true  # keep going\n"
        self.assertIn("|| true", executable_body(inline))

    def test_workflows_require_checkout_before_consuming_repo_resources(self) -> None:
        """Every workflow consuming repo requirements or helpers must run actions/checkout first."""
        checker_path = ROOT / "scripts" / "ci" / "check_python_dependency_locks.py"
        spec = importlib.util.spec_from_file_location("check_python_dependency_locks", checker_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)  # type: ignore[union-attr]
        checker = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
        spec.loader.exec_module(checker)  # type: ignore[union-attr]
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text(encoding="utf-8")
            findings = checker.scan_workflow_checkout_ordering(workflow, text, root=ROOT)
            self.assertEqual(
                findings, [], f"{workflow.name} consumes repo resources before actions/checkout"
            )
            with mock.patch.object(checker, "yaml", None):
                fallback_findings = checker.scan_workflow_checkout_ordering(
                    workflow, text, root=ROOT
                )
                self.assertEqual(
                    fallback_findings,
                    [],
                    f"{workflow.name} consumes repo resources before actions/checkout (fallback)",
                )


class InstallStepContract(unittest.TestCase):
    """Keep LLVM installs off a single network source and MSYS2 inherit steps out of paccache."""

    RETRY_FLAGS = ("--tries=5", "--retry-connrefused", "--waitretry=5", "--timeout=30")

    def installer_commands(self, workflow: str) -> list[list[str]]:
        commands = []
        for job in yaml.safe_load(workflow).get("jobs", {}).values():
            for step in job.get("steps", []):
                for raw_line in step.get("run", "").splitlines():
                    if "scripts/ci/install-llvm.sh" not in raw_line:
                        continue
                    # The build matrix selects clang inside a shell case arm.
                    command_line = re.sub(r"^\s*\*clang-\d+\)\s*", "", raw_line)
                    tokens = shlex.split(command_line, comments=True)
                    if tokens[:1] == ["sudo"]:
                        tokens = tokens[1:]
                    if tokens[:2] == ["bash", "scripts/ci/install-llvm.sh"]:
                        commands.append(tokens)
        return commands

    def test_llvm_workflows_use_shared_installer(self) -> None:
        expected = {
            "sanitizers.yml": 3,
            "lint-and-format.yml": 3,
            "fuzz.yml": 1,
            "nightly.yml": 1,
            "libvmaf-build-matrix.yml": 1,
            "tests-and-quality-gates.yml": 1,
        }
        for path in sorted(WORKFLOWS.glob("*.yml")):
            with self.subTest(workflow=path.name):
                workflow = path.read_text(encoding="utf-8")
                self.assertNotIn("apt.llvm.org/llvm.sh", executable_body(workflow))
                commands = self.installer_commands(workflow)
                self.assertEqual(len(commands), expected.get(path.name, 0))
                self.assertTrue(all(command[2] == "22" for command in commands))

    def test_installer_check_rejects_echo_and_non_run_text(self) -> None:
        for command in (
            "echo 'sudo bash scripts/ci/install-llvm.sh 22'",
            "# sudo bash scripts/ci/install-llvm.sh 22",
        ):
            workflow = yaml.safe_dump({"jobs": {"job": {"steps": [{"run": command}]}}})
            self.assertEqual(self.installer_commands(workflow), [])
        workflow = "jobs: {job: {steps: [{name: 'sudo bash scripts/ci/install-llvm.sh 22'}]}}"
        self.assertEqual(self.installer_commands(workflow), [])
        for command in (
            "sudo bash scripts/ci/install-llvm.sh 22",
            "bash scripts/ci/install-llvm.sh 22",
        ):
            workflow = yaml.safe_dump({"jobs": {"job": {"steps": [{"run": command}]}}})
            self.assertEqual(len(self.installer_commands(workflow)), 1)

    def write_installer_stubs(self, stubs: Path, bash: str, checksum: str) -> Path:
        calls = stubs / "calls"
        bodies = {
            "apt-get": "if [[ $1 == install && ! -e $STUBS/archive-attempt ]]; then\n"
            '  : > "$STUBS/archive-attempt"\n  exit "$FAKE_ARCHIVE_INSTALL"\nfi\n',
            "apt-cache": '[[ $2 != "$FAKE_MISSING" ]] || exit 100\nexit "$FAKE_APT_CACHE"\n',
            "wget": '[[ -z $FAKE_SIGNAL ]] || kill -s "$FAKE_SIGNAL" "$PPID"\n'
            'printf "%s" "$FAKE_PAYLOAD" > "$2"\nexit "$FAKE_WGET"\n',
            "bash": "exit 0\n",
            "rm": "exit 0\n",
            "sha256sum": f'exec {shlex.quote(checksum)} "$@"\n',
        }
        for tool, body in bodies.items():
            stub = stubs / tool
            stub.write_text(
                f'#!{bash}\necho "{tool} $*" >> "$CALLS"\n{body}',
                encoding="utf-8",
            )
            stub.chmod(0o755)
        (stubs / "mktemp").write_text(f'#!{bash}\necho "$STUBS/installer"\n', encoding="utf-8")
        (stubs / "mktemp").chmod(0o755)
        return calls

    def run_installer(
        self,
        script: str,
        major: str = "22",
        apt_cache: int = 0,
        wget: int = 0,
        missing: str = "",
        archive_install: int = 0,
        source: str | None = None,
        corrupt: bool = False,
        signal: str = "",
    ) -> tuple[int, str, list[str]]:
        """Execute only controlled stub tools and the real checksum utility."""
        bash = shutil.which("bash")
        checksum = shutil.which("sha256sum")
        if bash is None or checksum is None:
            self.skipTest("bash and sha256sum required for the Ubuntu installer")
        payload = "stub installer\n"
        # Substitute only the reviewed digest, never the verification command.
        script = re.sub(
            r"(?m)^LLVM_INSTALLER_SHA256=[0-9a-f]{64}$",
            "LLVM_INSTALLER_SHA256=" + hashlib.sha256(payload.encode()).hexdigest(),
            script,
        )
        with tempfile.TemporaryDirectory() as directory:
            stubs = Path(directory)
            calls = self.write_installer_stubs(stubs, bash, checksum)
            argv = [bash, "-c", script, "install-llvm.sh", major]
            if source is not None:
                argv.append(source)
            # Tracked installer, isolated PATH, controlled stubs; no network or package changes.
            result = subprocess.run(  # noqa: S603  # nosec B603
                argv,
                env={
                    "PATH": directory,
                    "CALLS": str(calls),
                    "STUBS": directory,
                    "FAKE_APT_CACHE": str(apt_cache),
                    "FAKE_WGET": str(wget),
                    "FAKE_MISSING": missing,
                    "FAKE_ARCHIVE_INSTALL": str(archive_install),
                    "FAKE_PAYLOAD": "corrupt\n" if corrupt else payload,
                    "FAKE_SIGNAL": signal,
                },
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            recorded = calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []
            recorded = [line.replace(directory, "<stubs>") for line in recorded]
        return result.returncode, result.stdout + result.stderr, recorded

    def installer(self) -> str:
        return (ROOT / "scripts/ci/install-llvm.sh").read_text(encoding="utf-8")

    def test_distribution_archive_needs_no_llvm_org(self) -> None:
        status, stdout, calls = self.run_installer(self.installer())
        self.assertEqual(status, 0)
        self.assertIn("LLVM 22 from the distribution archive", stdout)
        self.assertIn(
            "apt-get install -y -q clang-22 lldb-22 lld-22 clangd-22 clang-tidy-22", calls
        )
        self.assertFalse([call for call in calls if call.startswith("wget")])

    def test_fallback_fetches_with_bounded_retries_and_names_source(self) -> None:
        self.assert_fallback_contract(self.installer())

    def assert_fallback_contract(self, script: str) -> None:
        status, stdout, calls = self.run_installer(script, apt_cache=100)
        self.assertEqual(status, 0)
        self.assertIn("LLVM 22 from apt.llvm.org", stdout)
        fetch = [call for call in calls if call.startswith("wget")]
        self.assertEqual(len(fetch), 1)
        self.assertEqual(fetch[0].split()[-1], "https://apt.llvm.org/llvm.sh")
        for flag in self.RETRY_FLAGS:
            self.assertIn(flag, fetch[0].split())
        self.assertIn("bash <stubs>/installer 22", calls)
        self.assertLess(
            calls.index("sha256sum --check --status"), calls.index("bash <stubs>/installer 22")
        )

    def test_failed_fetch_fails_the_step(self) -> None:
        for wget in (4, 8):
            with self.subTest(wget_status=wget):
                status, _, calls = self.run_installer(self.installer(), apt_cache=100, wget=wget)
                self.assertEqual(status, wget)
                self.assertNotIn("bash <stubs>/installer 22", calls)

    def test_rejects_missing_or_non_numeric_major(self) -> None:
        for major in ("", "2x", "22;true"):
            with self.subTest(major=major):
                status, _, calls = self.run_installer(self.installer(), major=major)
                self.assertEqual(status, 2)
                self.assertEqual(calls, [])

    def test_retry_check_rejects_an_installer_without_retries(self) -> None:
        for flag in self.RETRY_FLAGS:
            with self.subTest(missing=flag), self.assertRaises(AssertionError):
                self.assert_fallback_contract(self.installer().replace(flag, ""))

    def assert_inherited_cache_disabled(self, workflow: str) -> int:
        inherited = 0
        for job in yaml.safe_load(workflow).get("jobs", {}).values():
            for step in job.get("steps", []):
                if not step.get("uses", "").startswith("msys2/setup-msys2@"):
                    continue
                inputs = step.get("with", {})
                if inputs.get("path-type") == "inherit":
                    inherited += 1
                    self.assertIs(inputs.get("cache"), False)
        return inherited

    def test_inherited_msys2_steps_disable_package_cache(self) -> None:
        inherited = 0
        for path in sorted(WORKFLOWS.glob("*.yml")):
            with self.subTest(workflow=path.name):
                inherited += self.assert_inherited_cache_disabled(path.read_text(encoding="utf-8"))
        self.assertGreater(inherited, 0)

    def test_msys2_cache_check_rejects_default_enabled_and_misplaced_cache(self) -> None:
        prefix = (
            "jobs:\n  j:\n    steps:\n      - uses: msys2/setup-msys2@pin\n"
            "        with:\n          path-type: inherit\n"
        )
        for cache in ("", "true", '"false"', "0"):
            workflow = prefix
            if cache:
                workflow += f"          cache: {cache}\n"
            workflow += "        env:\n          cache: false\n"
            with self.subTest(cache=cache), self.assertRaises(AssertionError):
                self.assert_inherited_cache_disabled(workflow)
        self.assertEqual(
            self.assert_inherited_cache_disabled(prefix + "          cache: false\n"), 1
        )

    def test_msys2_cache_check_allows_minimal_path(self) -> None:
        workflow = (
            "jobs: {j: {steps: [{uses: 'msys2/setup-msys2@pin', with: {path-type: minimal}}]}}"
        )
        self.assertEqual(self.assert_inherited_cache_disabled(workflow), 0)

    def test_archive_checks_every_requested_package(self) -> None:
        for package in ("clang", "lldb", "lld", "clangd", "clang-tidy"):
            with self.subTest(package=package):
                status, _, calls = self.run_installer(self.installer(), missing=f"{package}-22")
                self.assertEqual(status, 0)
                self.assertIn(f"apt-cache show {package}-22", calls)
                self.assertIn("bash <stubs>/installer 22", calls)

    def test_archive_install_failure_falls_back(self) -> None:
        status, _, calls = self.run_installer(self.installer(), archive_install=100)
        self.assertEqual(status, 0)
        self.assertIn("bash <stubs>/installer 22", calls)

    def test_installer_pins_the_upstream_digest(self) -> None:
        self.assertRegex(self.installer(), r"(?m)^LLVM_INSTALLER_SHA256=[0-9a-f]{64}$")

    def test_checksum_mismatch_names_installer_and_never_executes_it(self) -> None:
        status, output, calls = self.run_installer(self.installer(), apt_cache=100, corrupt=True)
        self.assertNotEqual(status, 0)
        self.assertIn("install-llvm: llvm.sh SHA-256 mismatch", output)
        self.assertNotIn("bash <stubs>/installer 22", calls)
        self.assertIn("rm -f <stubs>/installer", calls)

    def test_cleanup_after_fetch_failure_and_signals(self) -> None:
        for signal, expected in (("INT", 130), ("TERM", 143)):
            with self.subTest(signal=signal):
                status, _, calls = self.run_installer(
                    self.installer(), apt_cache=100, signal=signal
                )
                self.assertEqual(status, expected)
                self.assertIn("rm -f <stubs>/installer", calls)
                self.assertNotIn("bash <stubs>/installer 22", calls)
        status, _, calls = self.run_installer(self.installer(), apt_cache=100, wget=8)
        self.assertEqual(status, 8)
        self.assertIn("rm -f <stubs>/installer", calls)

    def test_explicit_llvm_org_source_bypasses_archive(self) -> None:
        status, _, calls = self.run_installer(self.installer(), source="--source=apt.llvm.org")
        self.assertEqual(status, 0)
        self.assertFalse([call for call in calls if call.startswith("apt-cache")])
        self.assertIn("bash <stubs>/installer 22", calls)
        status, _, calls = self.run_installer(self.installer(), source="--source=unknown")
        self.assertEqual((status, calls), (2, []))

    def test_ratchet_jobs_explicitly_select_baseline_source(self) -> None:
        for filename, job_id in (
            ("lint-and-format.yml", "clang-tidy-ratchet"),
            ("nightly.yml", "clang-tidy-full"),
        ):
            job = yaml.safe_load((WORKFLOWS / filename).read_text())["jobs"][job_id]
            workflow = yaml.safe_dump({"jobs": {job_id: job}})
            (command,) = self.installer_commands(workflow)
            self.assertEqual(command[2:], ["22", "--source=apt.llvm.org"])

    def assert_installer_copyright(self, script: str) -> None:
        self.assertRegex(script, r"(?m)^# Copyright 2026 Lusoris$")

    def test_installer_has_required_copyright(self) -> None:
        self.assert_installer_copyright(self.installer())

    def test_installer_copyright_rejects_unwanted_suffix(self) -> None:
        with self.assertRaises(AssertionError):
            self.assert_installer_copyright(
                self.installer().replace(
                    "# Copyright 2026 Lusoris",
                    "# Copyright 2026 Lusoris and Claude (Anthropic)",
                )
            )


if __name__ == "__main__":
    unittest.main()

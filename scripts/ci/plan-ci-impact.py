#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a bounded, fail-closed CI plan from the exact Git revisions of an event.

The plan decides which *surfaces* a change touches so that every required
check can start unconditionally (its status context always exists) and then
do real work only when its surface is impacted. Anything the planner cannot
prove safe collapses to ``mode=full`` — every selector true — which is exactly
today's behaviour. See ADR-1140.

Outputs (to ``--github-output``, one ``name=value`` per line):
  mode               full | impact
  reason             why (mapped-additive-diff, unknown-path:<p>, ...)
  base_sha, head_sha the revisions actually diffed
  changed_paths_json JSON array of changed paths (empty in full mode)
  <selector>         true | false, one line per selector in the config

Run ``--print`` locally to see the plan for a base..head pair.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

DEFAULT_CONFIG = Path(".github/ci-impact.json")
ZERO_SHA = "0" * 40
GIT = shutil.which("git") or "/usr/bin/git"  # absolute path: ruff S607

# Git environment inherited from a pre-commit / hook parent that must not leak
# into the planner's own git invocations (it would point them at the wrong
# index, work tree or object store).
_GIT_ENV_BLOCKLIST = frozenset(
    {
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_CEILING_DIRECTORIES",
        "GIT_COMMON_DIR",
        "GIT_CONFIG",
        "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_SYSTEM",
        "GIT_DIR",
        "GIT_GRAFT_FILE",
        "GIT_IMPLICIT_WORK_TREE",
        "GIT_INDEX_FILE",
        "GIT_NO_REPLACE_OBJECTS",
        "GIT_OBJECT_DIRECTORY",
        "GIT_PREFIX",
        "GIT_REPLACE_REF_BASE",
        "GIT_SHALLOW_FILE",
        "GIT_WORK_TREE",
    }
)


class PlanError(ValueError):
    """The planner input or configuration is structurally invalid."""


@dataclass(frozen=True)
class Change:
    status: str
    paths: tuple[str, ...]

    @property
    def code(self) -> str:
        return self.status[0]


@dataclass(frozen=True)
class Plan:
    mode: str
    reason: str
    base_sha: str
    head_sha: str
    changed_paths: tuple[str, ...]
    selectors: dict[str, bool]

    def github_outputs(self) -> dict[str, str]:
        outputs = {
            "mode": self.mode,
            "reason": self.reason,
            "base_sha": self.base_sha,
            "head_sha": self.head_sha,
            "changed_paths_json": json.dumps(
                self.changed_paths, ensure_ascii=True, separators=(",", ":")
            ),
        }
        outputs.update(
            {name: "true" if on else "false" for name, on in sorted(self.selectors.items())}
        )
        return outputs


# --------------------------------------------------------------------------- config


def _string_list(value: Any, label: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise PlanError(f"{label} must be a list of strings")
    if any(not item or item != item.strip() for item in value):
        raise PlanError(f"{label} contains an empty or whitespace-padded entry")
    return tuple(value)


def load_config(path: Path) -> dict[str, Any]:
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise PlanError(f"cannot read impact config {path}: {exc}") from exc
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise PlanError("impact config must be an object with schema_version 1")
    limits = config.get("limits")
    if not isinstance(limits, dict):
        raise PlanError("limits must be an object")
    for key in ("max_diff_bytes", "max_paths"):
        if not isinstance(limits.get(key), int) or limits[key] <= 0:
            raise PlanError(f"limits.{key} must be a positive integer")
    _string_list(config.get("known_prefixes"), "known_prefixes")
    _string_list(config.get("known_files"), "known_files")
    _string_list(config.get("full_patterns"), "full_patterns")
    if any(not prefix.endswith("/") for prefix in config["known_prefixes"]):
        raise PlanError("every known_prefixes entry must end with '/'")
    selectors = config.get("selectors")
    if not isinstance(selectors, dict) or not selectors:
        raise PlanError("selectors must be a non-empty object")
    for name, selector in selectors.items():
        if not isinstance(selector, dict):
            raise PlanError(f"selectors.{name} must be an object")
        _string_list(selector.get("patterns", []), f"selectors.{name}.patterns")
        inherits = _string_list(selector.get("inherits", []), f"selectors.{name}.inherits")
        unknown = set(inherits) - set(selectors)
        if unknown:
            raise PlanError(f"selectors.{name}.inherits names unknown selectors: {sorted(unknown)}")
    return config


# ------------------------------------------------------------------------- git diff


def _decode_path(raw: bytes) -> str:
    path = raw.decode("utf-8", errors="surrogateescape")
    parts = PurePosixPath(path).parts
    if not parts or path.startswith("/") or ".." in parts or "." in parts:
        raise PlanError(f"refusing unsafe path from git: {path!r}")
    return path


def parse_name_status(raw: bytes, max_paths: int) -> tuple[Change, ...]:
    """Parse ``git diff --name-status -z``, including rename/copy pairs."""
    if not raw:
        return ()
    if not raw.endswith(b"\0"):
        raise PlanError("NUL-delimited Git diff is missing its final delimiter")
    fields = raw[:-1].split(b"\0")
    changes: list[Change] = []
    index = 0
    while index < len(fields):
        status = fields[index].decode("ascii", errors="strict")
        if not status:
            raise PlanError("empty status field in Git diff stream")
        width = 2 if status[0] in {"R", "C"} else 1
        paths = fields[index + 1 : index + 1 + width]
        if len(paths) != width:
            raise PlanError(f"truncated Git diff record for status {status!r}")
        changes.append(Change(status=status, paths=tuple(_decode_path(p) for p in paths)))
        index += 1 + width
        if len(changes) > max_paths:
            raise PlanError(f"more than {max_paths} changed records")
    return tuple(changes)


def clean_git_environment() -> dict[str, str]:
    return {key: value for key, value in os.environ.items() if key not in _GIT_ENV_BLOCKLIST}


def _git(repo_root: Path, *args: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(  # noqa: S603 -- fixed argv, absolute git, no shell
        [GIT, "-C", str(repo_root), *args],
        capture_output=True,
        check=False,
        env=clean_git_environment(),
    )


def _commit_exists(repo_root: Path, sha: str) -> bool:
    return _git(repo_root, "cat-file", "-e", f"{sha}^{{commit}}").returncode == 0


def collect_changes(
    repo_root: Path, event: str, base_sha: str, head_sha: str, limits: dict[str, int]
) -> tuple[tuple[Change, ...] | None, str | None, str]:
    """Return (changes, fallback_reason, effective_base). Never raises for git state."""
    fallback: str | None = None
    changes: tuple[Change, ...] | None = None
    if not head_sha or head_sha == ZERO_SHA or not _commit_exists(repo_root, head_sha):
        fallback = "head-unavailable"
    elif not base_sha or base_sha == ZERO_SHA or not _commit_exists(repo_root, base_sha):
        fallback = "base-unavailable"
    elif event == "pull_request":
        # Merge-base aware: judge the PR by what it adds relative to where it
        # forked, not by what master did in the meantime.
        merge_base = _git(repo_root, "merge-base", base_sha, head_sha)
        if merge_base.returncode != 0:
            fallback = "no-merge-base"
        else:
            base_sha = merge_base.stdout.decode("ascii").strip()
    elif _git(repo_root, "merge-base", "--is-ancestor", base_sha, head_sha).returncode != 0:
        # Push with a non-linear `before` (force-push, branch creation): the
        # delta cannot be bounded, so run everything.
        fallback = "non-linear-push"
    if fallback is None:
        diff = _git(
            repo_root,
            "diff",
            "--name-status",
            "--find-renames",
            "--no-color",
            "-z",
            base_sha,
            head_sha,
        )
        if diff.returncode != 0:
            fallback = "git-diff-failed"
        elif len(diff.stdout) > limits["max_diff_bytes"]:
            fallback = "diff-too-large"
        else:
            try:
                changes = parse_name_status(diff.stdout, limits["max_paths"])
            except PlanError as exc:
                fallback = f"diff-unparseable:{exc}"
    return changes, fallback, base_sha


# ----------------------------------------------------------------------- planning


def _matches(path: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _is_known(path: str, config: dict[str, Any]) -> bool:
    if path in config["known_files"]:
        return True
    return any(path.startswith(prefix) for prefix in config["known_prefixes"])


def _selector_value(
    name: str,
    paths: tuple[str, ...],
    selectors: dict[str, Any],
    memo: dict[str, bool],
    visiting: set[str],
) -> bool:
    if name in memo:
        return memo[name]
    if name in visiting:
        raise PlanError(f"selector inheritance cycle through {name}")
    visiting.add(name)
    selector = selectors[name]
    selected = any(_matches(path, tuple(selector.get("patterns", []))) for path in paths)
    if not selected:
        selected = any(
            _selector_value(parent, paths, selectors, memo, visiting)
            for parent in selector.get("inherits", [])
        )
    visiting.remove(name)
    memo[name] = selected
    return selected


def full_plan(reason: str, base_sha: str, head_sha: str, config: dict[str, Any]) -> Plan:
    return Plan(
        mode="full",
        reason=reason,
        base_sha=base_sha,
        head_sha=head_sha,
        changed_paths=(),
        selectors=dict.fromkeys(config["selectors"], True),
    )


def build_plan(
    config: dict[str, Any],
    changes: tuple[Change, ...] | None,
    fallback_reason: str | None,
    base_sha: str,
    head_sha: str,
) -> Plan:
    if fallback_reason is not None or changes is None:
        return full_plan(
            fallback_reason or "change-enumeration-unavailable", base_sha, head_sha, config
        )
    if not changes:
        return full_plan("empty-diff", base_sha, head_sha, config)
    # Only additions and in-place modifications are provably scoped. A delete,
    # rename, copy, type change or mode change can silently widen the blast
    # radius (a header that vanished, a script renamed out of a glob), so those
    # run everything.
    unsafe = next((change for change in changes if change.code not in {"A", "M"}), None)
    if unsafe is not None:
        return full_plan(f"non-additive-change:{unsafe.status}", base_sha, head_sha, config)
    paths = tuple(sorted({path for change in changes for path in change.paths}))
    unknown = next((path for path in paths if not _is_known(path, config)), None)
    if unknown is not None:
        return full_plan(f"unknown-path:{unknown}", base_sha, head_sha, config)
    full_patterns = tuple(config["full_patterns"])
    global_path = next((path for path in paths if _matches(path, full_patterns)), None)
    if global_path is not None:
        return full_plan(f"global-ci-input:{global_path}", base_sha, head_sha, config)
    memo: dict[str, bool] = {}
    selectors = {
        name: _selector_value(name, paths, config["selectors"], memo, set())
        for name in config["selectors"]
    }
    return Plan(
        mode="impact",
        reason="mapped-additive-diff",
        base_sha=base_sha,
        head_sha=head_sha,
        changed_paths=paths,
        selectors=selectors,
    )


def write_github_output(path: Path, plan: Plan) -> None:
    with path.open("a", encoding="utf-8") as output:
        for name, value in plan.github_outputs().items():
            if "\n" in value:
                raise PlanError(f"output {name} would span lines")
            output.write(f"{name}={value}\n")


# ---------------------------------------------------------------------------- cli


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--event", required=True, help="GitHub event name (pull_request, push, ...)"
    )
    parser.add_argument("--base", required=True, help="base SHA (PR base, or push `before`)")
    parser.add_argument("--head", required=True, help="head SHA")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--repo-root", type=Path, default=Path())
    parser.add_argument("--github-output", type=Path, help="append name=value lines here")
    parser.add_argument("--print", action="store_true", help="print the plan as JSON to stdout")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        config = load_config(
            args.repo_root / args.config if not args.config.is_absolute() else args.config
        )
        effective_base = args.base
        if args.event in {"pull_request", "push"}:
            changes, fallback, effective_base = collect_changes(
                args.repo_root, args.event, args.base, args.head, config["limits"]
            )
        else:
            changes, fallback = None, f"event-not-routed:{args.event}"
        plan = build_plan(config, changes, fallback, effective_base, args.head)
        if args.github_output is not None:
            write_github_output(args.github_output, plan)
        if args.print or args.github_output is None:
            print(
                json.dumps(
                    {
                        "mode": plan.mode,
                        "reason": plan.reason,
                        "selectors": plan.selectors,
                        "changed_paths": list(plan.changed_paths),
                    },
                    indent=2,
                    sort_keys=True,
                )
            )
    except PlanError as exc:
        print(f"plan-ci-impact: {exc}", file=sys.stderr)
        return 2
    print(
        f"plan-ci-impact: mode={plan.mode} reason={plan.reason} selected={sorted(n for n, v in plan.selectors.items() if v)}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

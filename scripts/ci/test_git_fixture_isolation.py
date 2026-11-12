#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run Git fixture helpers under caller redirection without touching a real repo.

Also guards the fixture identity: no test script may write ``user.email`` or
``user.name`` into a git config, and every script that builds a scratch
repository must leave a hook-exported ``GIT_DIR`` repository untouched.

Three nets, because each one misses what another catches:

* the identity rule: no tracked script persists ``user.*``;
* the isolation rule: a tracked script that creates a repository (``git init``,
  ``git clone``) or persists any git config key (``core.bare``,
  ``uploadpack.*``, ...) must drop the caller's ``GIT_*`` variables in the
  same file, or be a declared exception. This is the net for a pytest-only
  test and for a script that is not a test, which the third net cannot run;
* the sentinel run: every scratch-repository test script that runs on its
  own is executed the way a hook runs it, and must leave the hook's
  repository byte-identical.
"""

from __future__ import annotations

import ast
import concurrent.futures
import datetime
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from functools import partial
from pathlib import Path
from typing import Any, TypeGuard

try:  # the hooks run this file with plain `python3`; only the tooling suite brings pytest
    import pytest
except ImportError:
    pytest = None

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
VARIABLES = (
    "GIT_DIR",
    "GIT_COMMON_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_CONFIG_PARAMETERS",
)


# `git config user.email|user.name` written to a config file. Per-command `-c user.*=`
# and GIT_AUTHOR_* / GIT_COMMITTER_* variables are the allowed ways to give a fixture an
# identity: they never persist, so they cannot leak into a caller's shared .git/config.
IDENTITY_WRITE = re.compile(
    r"""config\s+(?:--(?:local|global|worktree|system)\s+)?user\.(?:email|name)\b"""
    r"""|["']config["']\s*,\s*(?:["']--(?:local|global|worktree|system)["']\s*,\s*)?["']user\.(?:email|name)["']"""
)
# Scripts that create a scratch repository: a `git ... init` command or an "init" argument.
SCRATCH_REPOSITORY = re.compile(r"""git[^\n]*\binit\b|["']init["']""")
TEST_SCRIPT = re.compile(r"(^|/)test[-_][^/]*\.(sh|py)$")
# Workflow steps and Containerfiles are out of scope: a CI runner or an image build has no hook
# environment, and the clone they configure is ephemeral.
SUFFIXES = (".sh", ".py", ".mjs", ".go", ".rs", ".mk", "Makefile")

# An exception names one file, one rule, a reason and an expiry (ISO date) and fails like a
# missing exception once it expires. Every entry builds its git environment through a helper
# that drops GIT_*, and the runtime sentinel check below covers it as well.
IDENTITY_EXCEPTION_REASON = (
    "git helper scrubs GIT_* and ignores global config; sentinel check covers it"
)
IDENTITY_EXCEPTION_EXPIRY = "2026-12-31"
IDENTITY_EXCEPTION_FILES = (
    "scripts/ci/test_git_fixture_isolation.py",
    "scripts/ci/tests/test_check_source_adr_citations.py",
    "scripts/ci/tests/test_research_digest_ids.py",
    "scripts/ci/tests/test_scorecard_gate.py",
    "scripts/dev/tests/test_install_merge_train_guard.py",
    "scripts/dev/tests/test_merge_train_guard.py",
    "scripts/git-hooks/test-pre-push-mypy.py",
    "scripts/git-hooks/test-pre-push-pr-body-lint.py",
    "scripts/githooks/tests/test_install.py",
)


# The isolation rule. A script that runs one of these acts on the CALLER's repository when a
# hook's GIT_DIR / GIT_INDEX_FILE are still set, whatever its working directory is.
SHELL_SUFFIXES = (".sh", ".mk", "Makefile")
_GIT_OPTIONS = r"(?:\s+-[cC]\s*\S+|\s+--[\w-]+(?:=\S+)?)*"
_READ_ONLY = r"(?:get|get-all|get-regexp|get-urlmatch|list|show-origin|show-scope|name-only)"
SHELL_GIT_WRITE = re.compile(
    r"(?m)^(?!\s*#)[^\n#]*?\bgit\b" + _GIT_OPTIONS + r"\s+(?:(?:init|clone)\b"
    r"|config\s+(?!--" + _READ_ONLY + r"\b)"
    r"(?:--(?:local|global|worktree|system|add|replace-all|file\s+\S+)\s+)*[\w.-]+\.[\w.-]+[ \t]+\S)"
)
# Before the subcommand, a Git call may hold code and option literals ("-q", or "-C"/"-c" and
# their value), never another string: in git("commit", "-m", "init") the subcommand is commit.
_CODE_GIT_PREFIX = r"""(?:[^"'\n]|["']-[cC]["']\s*,\s*["'][^"'\n]*["']|["']-[^"'\n]*["'])"""
# Match git at a word boundary or after an underscore (e.g. run_git), never inside digit.
CODE_GIT_WRITE = re.compile(
    r"""(?im)^(?!\s*(?:#|//))[^\n]*(?:\b|_)git\w*["']?"""
    + _CODE_GIT_PREFIX
    + r"""{0,80}?["'](?:init|clone)["']\s*[,)\]]"""
    r"""|["']config["']\s*,\s*(?:["']--(?:local|global|worktree|system|add|replace-all)["']\s*,\s*)*"""
    r"""["'][\w.-]+\.[\w.-]+["']\s*,\s*\S"""
)
# What counts as dropping the caller's variables in shell, Node and Go; Python uses AST checks.
ISOLATION_MARKER = re.compile(
    r"""(?:clean|drop)-git-env\.sh|compgen -A variable GIT_"""
    r"""|startsWith\(\s*["']GIT_|HasPrefix\([^)\n]*"GIT_"""
)
PYTHON_HELPERS = ("clean_git_environment", "git_environment")
COMPREHENSION_NODES = (ast.ListComp, ast.SetComp, ast.DictComp, ast.GeneratorExp)
FILTER_NODES = (*COMPREHENSION_NODES, ast.Call)
BARRIER_NULL_KEYS = ("GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM")
BARRIER_KEYS = (*BARRIER_NULL_KEYS, "GIT_CONFIG_NOSYSTEM")
PAIR_LENGTH = 2
# One file, one reason, one expiry: a file whose git runs isolated in a way the marker cannot
# see, with the place that does it. The sentinel run covers every test script among them.
ISOLATION_EXCEPTION_EXPIRY = "2026-12-31"
ISOLATION_EXCEPTIONS = {
    ".config/lefthook/scripts/common.py": "clean_env() removes the repository variables by name",
    "scripts/ci/tests/test_research_digest_ids.py": (
        "git runs through check-research-digest-ids._git, which drops GIT_*"
    ),
    "scripts/ci/tests/test_pr_time_verify_workflows.py": (
        "run_validate() builds env_base from a fixed allow-list, never caller GIT_* "
        "(scripts/ci/tests/test_pr_time_verify_workflows.py:95)"
    ),
    "scripts/dev/tests/test_merge_train_guard.py": (
        "git runs through merge_train_guard.environment(), which drops GIT_*"
    ),
    "scripts/dev/tests/test_upstream_pr_sweep_hardening.py": (
        "git runs through upstream_pr_sweep_fixture.git_run, which uses git_environment()"
    ),
    "scripts/docs/tests/test_render_at_landing.py": (
        "every git call gets a fixed environment (ENV) that holds no caller variable"
    ),
    "tools/apicompat/gate/main.go": "main() unsets gitRepositoryVariables before any git call",
    # NOT isolated: the fixtures of its --self-test mode inherit the environment. The file is the
    # governance engine's template and no hook or workflow of this repository runs that mode.
    # Reported upstream as cordanaLLM/praetor#1190; drop the entry with the engine bump that fixes it.
    "tools/markdownlint/verify.mjs": (
        "engine template; --self-test fixtures inherit GIT_*; that mode is not run here"
    ),
}


def is_str_constant(node: ast.AST | None, expected: str | None = None) -> bool:
    """True when node is a str Constant, optionally equal to expected."""
    if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
        return False
    return expected is None or node.value == expected


def is_os_attr(node: ast.AST, attr: str) -> bool:
    """True when node is the attribute expression os.<attr>."""
    return (
        isinstance(node, ast.Attribute)
        and node.attr == attr
        and isinstance(node.value, ast.Name)
        and node.value.id == "os"
    )


def is_environ_copy(node: ast.expr) -> bool:
    """True when node is os.environ.copy()."""
    return (
        isinstance(node, ast.Call)
        and not node.args
        and not node.keywords
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "copy"
        and is_os_attr(node.func.value, "environ")
    )


def is_environ_dict_arg(node: ast.expr) -> bool:
    """True when node is os.environ, os.environ.copy() or .items() on either."""
    if is_os_attr(node, "environ") or is_environ_copy(node):
        return True
    return (
        isinstance(node, ast.Call)
        and not node.args
        and not node.keywords
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "items"
        and (is_os_attr(node.func.value, "environ") or is_environ_copy(node.func.value))
    )


def is_environ_mapping(node: ast.expr) -> bool:
    """True when node evaluates to os.environ, os.environ.copy() or dict(os.environ)."""
    if is_os_attr(node, "environ") or is_environ_copy(node):
        return True
    if not (
        isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "dict"
    ):
        return False
    if len(node.args) == 1 and is_environ_dict_arg(node.args[0]):
        return True
    return any(
        kw.arg is None and (is_os_attr(kw.value, "environ") or is_environ_copy(kw.value))
        for kw in node.keywords
    )


def environ_iterable_kind(node: ast.expr) -> str | None:
    """Classify an iterable derived from os.environ, or None when unrelated."""
    if is_environ_mapping(node):
        return "mapping"
    if (
        isinstance(node, ast.Call)
        and not node.args
        and not node.keywords
        and isinstance(node.func, ast.Attribute)
        and node.func.attr in ("items", "keys", "values")
        and is_environ_mapping(node.func.value)
    ):
        return node.func.attr
    return None


def startswith_prefix_arg(node: ast.expr, receiver: str | None = None) -> ast.expr | None:
    """Return the sole prefix argument of <receiver>.startswith(arg), else None."""
    if not (isinstance(node, ast.Call) and len(node.args) == 1 and not node.keywords):
        return None
    if not (isinstance(node.func, ast.Attribute) and node.func.attr == "startswith"):
        return None
    if receiver is not None and not (
        isinstance(node.func.value, ast.Name) and node.func.value.id == receiver
    ):
        return None
    return node.args[0]


def is_canonical_prefix(arg: ast.expr) -> bool:
    """True for 'GIT_' or a tuple of str constants starting with 'GIT_'."""
    if is_str_constant(arg, "GIT_"):
        return True
    return (
        isinstance(arg, ast.Tuple)
        and bool(arg.elts)
        and all(is_str_constant(elt) for elt in arg.elts)
        and is_str_constant(arg.elts[0], "GIT_")
    )


def is_canonical_exclusion(cond: ast.expr, key: str) -> bool:
    """True when cond is exclusively `not <key>.startswith('GIT_' | ('GIT_', ...))`."""
    if not (isinstance(cond, ast.UnaryOp) and isinstance(cond.op, ast.Not)):
        return False
    prefix = startswith_prefix_arg(cond.operand, key)
    return prefix is not None and is_canonical_prefix(prefix)


def is_config_key_slice(elt: ast.expr, key: str) -> bool:
    """True when elt is `<key>[len('GIT_CONFIG_KEY_'):]`."""
    if not (
        isinstance(elt, ast.Subscript)
        and isinstance(elt.value, ast.Name)
        and elt.value.id == key
        and isinstance(elt.slice, ast.Slice)
        and elt.slice.upper is None
        and elt.slice.step is None
    ):
        return False
    lower = elt.slice.lower
    return (
        isinstance(lower, ast.Call)
        and isinstance(lower.func, ast.Name)
        and lower.func.id == "len"
        and len(lower.args) == 1
        and not lower.keywords
        and is_str_constant(lower.args[0], "GIT_CONFIG_KEY_")
    )


def is_config_key_lookup(elt: ast.expr, cond: ast.expr, key: str, value: str) -> bool:
    """Allow extracting one planted GIT_CONFIG_KEY_* suffix by exact value match."""
    if not is_config_key_slice(elt, key):
        return False
    if not (
        isinstance(cond, ast.BoolOp)
        and isinstance(cond.op, ast.And)
        and len(cond.values) == PAIR_LENGTH
    ):
        return False
    prefix = startswith_prefix_arg(cond.values[0], key)
    compare = cond.values[1]
    return (
        prefix is not None
        and is_str_constant(prefix, "GIT_CONFIG_KEY_")
        and isinstance(compare, ast.Compare)
        and isinstance(compare.left, ast.Name)
        and compare.left.id == value
        and len(compare.ops) == 1
        and isinstance(compare.ops[0], ast.Eq)
        and len(compare.comparators) == 1
        and is_str_constant(compare.comparators[0])
    )


def comprehension_targets(target: ast.expr, kind: str) -> tuple[str | None, str | None]:
    """Extract (key_name, value_name) bound by an os.environ comprehension target."""
    if kind in ("mapping", "keys") and isinstance(target, ast.Name):
        return target.id, None
    if (
        kind == "items"
        and isinstance(target, (ast.Tuple, ast.List))
        and len(target.elts) == PAIR_LENGTH
        and isinstance(target.elts[0], ast.Name)
        and isinstance(target.elts[1], ast.Name)
    ):
        return target.elts[0].id, target.elts[1].id
    return None, None


def comprehension_violation(
    name: str,
    text: str,
    node: ast.ListComp | ast.SetComp | ast.DictComp | ast.GeneratorExp,
) -> str:
    """Validate any comprehension iterating over an os.environ-derived iterable."""
    kinds = [environ_iterable_kind(gen.iter) for gen in node.generators]
    if not any(kind is not None for kind in kinds):
        return ""
    gen = node.generators[0]
    if len(node.generators) != 1 or gen.is_async or len(gen.ifs) != 1 or kinds[0] is None:
        segment = ast.get_source_segment(text, node) or ""
        return f"{name}:{node.lineno}: noncanonical environment filter: {segment}"
    cond = gen.ifs[0]
    key_name, value_name = comprehension_targets(gen.target, kinds[0])
    allowed = key_name is not None and (
        is_canonical_exclusion(cond, key_name)
        or (
            not isinstance(node, ast.DictComp)
            and value_name is not None
            and is_config_key_lookup(node.elt, cond, key_name, value_name)
        )
    )
    if allowed:
        return ""
    segment = ast.get_source_segment(text, cond) or ""
    return f"{name}:{cond.lineno}: noncanonical environment condition: {segment}"


def is_filter_call(node: ast.Call) -> bool:
    """True when node calls builtin filter(...)."""
    if isinstance(node.func, ast.Name):
        return node.func.id == "filter"
    return (
        isinstance(node.func, ast.Attribute)
        and node.func.attr == "filter"
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == "builtins"
    )


def plain_lambda_parameter(node: ast.expr) -> str | None:
    """Return the sole parameter name of a plain single-argument lambda, else None."""
    if not isinstance(node, ast.Lambda):
        return None
    params = node.args
    if (
        len(params.args) != 1
        or params.posonlyargs
        or params.kwonlyargs
        or params.vararg is not None
        or params.kwarg is not None
        or params.defaults
        or params.kw_defaults
    ):
        return None
    return params.args[0].arg


def filter_call_violation(name: str, text: str, node: ast.Call) -> str:
    """Validate filter(...) calls whose iterable derives from os.environ."""
    if not is_filter_call(node):
        return ""
    kinds = [environ_iterable_kind(arg) for arg in node.args]
    kw_environ = any(environ_iterable_kind(kw.value) is not None for kw in node.keywords)
    if not any(kind is not None for kind in kinds) and not kw_environ:
        return ""
    key_name = plain_lambda_parameter(node.args[0]) if node.args else None
    if (
        len(node.args) == PAIR_LENGTH
        and not node.keywords
        and kinds[1] in ("mapping", "keys")
        and key_name is not None
        and isinstance(node.args[0], ast.Lambda)
        and is_canonical_exclusion(node.args[0].body, key_name)
    ):
        return ""
    focus: ast.AST = node
    if node.args:
        focus = node.args[0].body if isinstance(node.args[0], ast.Lambda) else node.args[0]
    line = getattr(focus, "lineno", node.lineno)
    segment = ast.get_source_segment(text, focus) or ""
    return f"{name}:{line}: noncanonical environment condition: {segment}"


def python_filter_violation(name: str, text: str, tree: ast.AST) -> str:
    """Allow only complete canonical GIT_* exclusion filters over os.environ."""
    candidates = sorted(
        (node for node in ast.walk(tree) if isinstance(node, FILTER_NODES)),
        key=lambda node: (node.lineno, node.col_offset),
    )
    for node in candidates:
        violation = (
            filter_call_violation(name, text, node)
            if isinstance(node, ast.Call)
            else comprehension_violation(name, text, node)
        )
        if violation:
            return violation
    return ""


def is_marker_prefix(arg: ast.expr) -> bool:
    """True when arg is a GIT_* str constant or a tuple containing one."""
    if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
        return arg.value.startswith("GIT_")
    return isinstance(arg, ast.Tuple) and any(
        isinstance(elt, ast.Constant)
        and isinstance(elt.value, str)
        and elt.value.startswith("GIT_")
        for elt in arg.elts
    )


def has_python_marker(tree: ast.AST) -> bool:
    """True when tree calls an environment helper or startswith('GIT_*')."""
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if isinstance(node.func, ast.Name) and node.func.id in PYTHON_HELPERS:
            return True
        if isinstance(node.func, ast.Attribute) and node.func.attr in PYTHON_HELPERS:
            return True
        prefix = startswith_prefix_arg(node)
        if prefix is not None and is_marker_prefix(prefix):
            return True
    return False


def is_barrier_value(key: str, value: ast.expr) -> bool:
    """True when value assigns the accepted barrier target for key."""
    if key in BARRIER_NULL_KEYS:
        return is_os_attr(value, "devnull") or is_str_constant(value, "/dev/null")
    if key == "GIT_CONFIG_NOSYSTEM":
        return is_str_constant(value, "1")
    return False


def is_mapping_call(node: ast.AST) -> TypeGuard[ast.Call]:
    """True when node is a dict(...) or <mapping>.update(...) call."""
    if not isinstance(node, ast.Call):
        return False
    return (isinstance(node.func, ast.Name) and node.func.id == "dict") or (
        isinstance(node.func, ast.Attribute) and node.func.attr == "update"
    )


def subscript_str_key(target: ast.AST) -> str | None:
    """Return the string constant slice of target['KEY'], else None."""
    if not isinstance(target, ast.Subscript):
        return None
    if isinstance(target.slice, ast.Constant) and isinstance(target.slice.value, str):
        return target.slice.value
    return None


def unpack_mapping_item(
    value: ast.expr,
) -> ast.Dict | ast.Call | tuple[str | None, ast.expr]:
    """Expand nested dict displays or mapping calls; keep environ or opaque unpacks."""
    if isinstance(value, ast.Dict):
        return value
    if is_mapping_call(value):
        return (None, value) if not value.keywords and is_environ_mapping(value) else value
    return (None, value)


def positional_mapping_item(
    arg: ast.expr,
) -> ast.Dict | ast.Call | tuple[str | None, ast.expr] | None:
    """Classify a positional mapping-call argument for ordered entry extraction."""
    if (
        isinstance(arg, ast.Dict)
        or is_mapping_call(arg)
        or is_environ_mapping(arg)
        or is_environ_dict_arg(arg)
    ):
        return unpack_mapping_item(arg)
    return None


def push_dict_display_children(
    node: ast.Dict,
    stack: list[ast.Dict | ast.Call | tuple[str | None, ast.expr]],
) -> None:
    """Push dict display pairs and nested **unpacks onto stack in reverse evaluation order."""
    for key, val in zip(reversed(node.keys), reversed(node.values), strict=True):
        if key is None:
            stack.append(unpack_mapping_item(val))
        elif isinstance(key, ast.Constant) and isinstance(key.value, str):
            stack.append((key.value, val))


def push_mapping_call_children(
    call: ast.Call,
    stack: list[ast.Dict | ast.Call | tuple[str | None, ast.expr]],
) -> None:
    """Push call's positional and keyword entries onto stack in reverse evaluation order."""
    for kw in reversed(call.keywords):
        stack.append(unpack_mapping_item(kw.value) if kw.arg is None else (kw.arg, kw.value))
    for arg in reversed(call.args):
        item = positional_mapping_item(arg)
        if item is not None:
            stack.append(item)


def node_mapping_entries(node: ast.AST) -> list[tuple[str | None, ast.expr]]:
    """Ordered (key, value) entries for a dict display or dict/update call; None marks **unpack."""
    if not (isinstance(node, ast.Dict) or is_mapping_call(node)):
        return []
    entries: list[tuple[str | None, ast.expr]] = []
    stack: list[ast.Dict | ast.Call | tuple[str | None, ast.expr]] = [node]
    for _ in range(len(list(ast.walk(node)))):
        if not stack:
            break
        current = stack.pop()
        if isinstance(current, tuple):
            entries.append(current)
        elif isinstance(current, ast.Dict):
            push_dict_display_children(current, stack)
        else:
            push_mapping_call_children(current, stack)
    return entries


def node_barrier_pairs(node: ast.AST) -> list[tuple[str, ast.expr]]:
    """Extract executable (key, value) candidate assignments from one AST node."""
    if isinstance(node, ast.Dict) or is_mapping_call(node):
        return [(key, val) for key, val in node_mapping_entries(node) if key is not None]
    if isinstance(node, ast.Assign):
        return [
            (key, node.value)
            for target in node.targets
            if (key := subscript_str_key(target)) is not None
        ]
    if isinstance(node, ast.AnnAssign) and node.value is not None:
        key = subscript_str_key(node.target)
        return [(key, node.value)] if key is not None else []
    return []


def mapping_expression_nodes(tree: ast.AST) -> list[ast.Dict | ast.Call]:
    """Return dict displays and dict/update calls in tree ordered by source position."""
    return sorted(
        (node for node in ast.walk(tree) if isinstance(node, ast.Dict) or is_mapping_call(node)),
        key=lambda node: (node.lineno, node.col_offset),
    )


def post_barrier_unpack_line(node: ast.AST) -> int | None:
    """Line of the first **os.environ unpack placed after a barrier key in node, else None."""
    seen_barrier = False
    for key, value in node_mapping_entries(node):
        if key is not None and is_barrier_value(key, value):
            seen_barrier = True
        elif (
            key is None
            and seen_barrier
            and (is_environ_mapping(value) or is_environ_dict_arg(value))
        ):
            return value.lineno
    return None


def overridden_barrier_entry(node: ast.AST) -> tuple[str, int] | None:
    """First barrier key set and later overridden by a non-barrier in node, with line."""
    seen_barrier: set[str] = set()
    overridden: dict[str, int] = {}
    for key, value in node_mapping_entries(node):
        if key is None or key not in BARRIER_KEYS:
            continue
        if is_barrier_value(key, value):
            seen_barrier.add(key)
            overridden.pop(key, None)
        elif key in seen_barrier:
            overridden[key] = value.lineno
    return next(iter(overridden.items()), None)


def barrier_order_violation(name: str, tree: ast.AST) -> str:
    """Refuse **os.environ unpacking placed after GIT_CONFIG_* barrier keys."""
    for node in mapping_expression_nodes(tree):
        lineno = post_barrier_unpack_line(node)
        if lineno is not None:
            return (
                f"{name}:{lineno}: **os.environ after GIT_CONFIG_* barrier keys "
                "lets the caller override them; place the expansion first"
            )
    return ""


def barrier_override_violation(name: str, tree: ast.AST) -> str:
    """Refuse barrier keys set and then overridden by a non-barrier in one expression."""
    for node in mapping_expression_nodes(tree):
        overridden = overridden_barrier_entry(node)
        if overridden is not None:
            key, lineno = overridden
            return (
                f"{name}:{lineno}: {key} is set to a barrier and then overridden "
                "in the same expression; keep the barrier value last"
            )
    return ""


def barrier_expression_violation(name: str, tree: ast.AST) -> str:
    """First barrier order or same-expression override violation in tree, else empty."""
    return barrier_order_violation(name, tree) or barrier_override_violation(name, tree)


def fixture_config_violation(name: str, tree: ast.AST) -> str:
    """Require explicit global and system config isolation in Python fixture files."""
    path = Path(name)
    fixture = TEST_SCRIPT.search(name) or "tests" in path.parts or "fixture" in path.name
    if not fixture:
        return ""
    barrier_violation = barrier_expression_violation(name, tree)
    if barrier_violation:
        return barrier_violation
    assigned = {
        key
        for node in ast.walk(tree)
        for key, value in node_barrier_pairs(node)
        if is_barrier_value(key, value)
    }
    if "GIT_CONFIG_GLOBAL" not in assigned:
        return f"{name}: missing GIT_CONFIG_GLOBAL assignment to os.devnull"
    if not assigned.intersection(("GIT_CONFIG_SYSTEM", "GIT_CONFIG_NOSYSTEM")):
        return f"{name}: missing GIT_CONFIG_SYSTEM=os.devnull or GIT_CONFIG_NOSYSTEM=1 assignment"
    return ""


def python_isolation_violation(name: str, text: str, *, require_marker: bool = True) -> str:
    """Validate Python syntax, os.environ filters, isolation markers and fixture barriers."""
    try:
        tree = ast.parse(text, filename=name)
    except SyntaxError as error:
        return f"{name}:{error.lineno or 1}: cannot parse Python source: {error.msg}"
    found = CODE_GIT_WRITE.search(text)
    if found is None:
        return ""
    violation = python_filter_violation(name, text, tree)
    if violation:
        return violation
    if require_marker and not has_python_marker(tree):
        return found.group(0).strip()[:100]
    return fixture_config_violation(name, tree)


def unisolated_git(name: str, text: str) -> str:
    """Return the first unsafe Git write, filter condition or missing fixture config key."""
    if name.endswith(".py"):
        return python_isolation_violation(name, text)
    found = (SHELL_GIT_WRITE if name.endswith(SHELL_SUFFIXES) else CODE_GIT_WRITE).search(text)
    if found is None or ISOLATION_MARKER.search(text):
        return ""
    return found.group(0).strip()[:100]


def unisolated_offenders(root: Path, names: list[str], today: str) -> list[str]:
    """Diagnostics for every listed script under root that writes Git state unisolated."""
    offenders = []
    for name in names:
        if not name.endswith(SUFFIXES) or name == "scripts/ci/test_git_fixture_isolation.py":
            continue
        path = root / name
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        excepted = name in ISOLATION_EXCEPTIONS and today <= ISOLATION_EXCEPTION_EXPIRY
        if excepted and not name.endswith(".py"):
            continue
        sample = (
            python_isolation_violation(name, text, require_marker=not excepted)
            if name.endswith(".py")
            else unisolated_git(name, text)
        )
        if sample:
            offenders.append(sample if sample.startswith(name + ":") else f"{name}: {sample}")
    return offenders


# The sentinel run is split into shards, one test method each: the tooling suite stops a
# single test after 300 s (suite_registry.PER_TEST_TIMEOUT_SECONDS) and a hosted runner needed
# more than that for all 63 scripts in one test. The scripts named here are the measured slow
# ones (seconds: hosted runner standalone / this host under the sentinel); every other script
# falls into one of REST_SHARDS shards by its position in the sorted list, so a new script is
# always in a shard.
SLOW_SHARDS = {
    # 396 / 125 before its scratch tree was built once; 90 here after.
    "default_model": ("scripts/ci/tests/test-default-model-single-source.sh",),
    # 68 / 64 and 28 / 28.
    "hooks": (
        "scripts/git-hooks/test-pre-push-pr-body-lint.py",
        "scripts/githooks/tests/test_install.py",
    ),
}
REST_SHARDS = 2
# The one shard that cannot stay under the suite's limit on a hosted runner: its script alone
# took 396 s there. sentinel_damage() bounds the script itself at 600 s.
DEFAULT_MODEL_SHARD_TIMEOUT_SECONDS = 900


def per_test_timeout(seconds: int) -> Callable[[Any], Any]:
    """Give one test its own limit in the tooling suite; a no-op without pytest."""
    if pytest is None:
        return lambda function: function
    return pytest.mark.timeout(seconds)  # type: ignore[no-any-return]


def sentinel_shards(scripts: list[str]) -> dict[str, list[str]]:
    """Split the scratch-repository scripts into the shards the sentinel tests run."""
    slow = {name for names in SLOW_SHARDS.values() for name in names}
    shards = {
        label: [name for name in names if name in scripts] for label, names in SLOW_SHARDS.items()
    }
    rest = sorted(name for name in scripts if name not in slow)
    for index in range(REST_SHARDS):
        shards[f"rest_{index}"] = rest[index::REST_SHARDS]
    return shards


def partition_faults(scripts: list[str], shards: dict[str, list[str]]) -> list[str]:
    """Every script in exactly one shard, and no shard entry that is not a script."""
    placed = [name for names in shards.values() for name in names]
    faults = [f"{name}: in no shard" for name in sorted(set(scripts) - set(placed))]
    faults += [f"{name}: not a scratch script" for name in sorted(set(placed) - set(scripts))]
    faults += [
        f"{name}: in {placed.count(name)} shards"
        for name in sorted(set(placed))
        if placed.count(name) > 1
    ]
    return faults


def tracked_files() -> list[str]:
    """List tracked files without inheriting a caller's repository."""
    done = run_git(ROOT, "ls-files", "-z", environment=clean_git_environment())
    listing = done.stdout
    assert isinstance(listing, str)
    return [name for name in listing.split("\0") if name]


def identity_writes(text: str) -> bool:
    """True when the text writes a fixture identity into a git config."""
    return IDENTITY_WRITE.search(text) is not None


def scratch_scripts() -> list[str]:
    """Tracked test scripts that build a scratch repository and can run on their own."""
    found = []
    for name in tracked_files():
        if not TEST_SCRIPT.search(name) or name == "scripts/ci/test_git_fixture_isolation.py":
            continue
        text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
        runnable = name.endswith(".sh") or "__main__" in text
        if runnable and SCRATCH_REPOSITORY.search(text):
            found.append(name)
    return found


# No git process may start background work in the sentinel: see make_sentinel().
SENTINEL_CONFIG = (("maintenance.auto", "false"), ("gc.auto", "0"))
BACKGROUND_WORK = re.compile(r"run_command: git (?:maintenance run|gc --auto)")


def commit_starts_background_work(repository: Path) -> bool:
    """True when an empty commit in `repository` makes git start maintenance or gc."""
    environment = {**clean_git_environment(), "GIT_TRACE": "1"}
    done = run_git(
        repository,
        "-c", "user.name=Sentinel", "-c", "user.email=sentinel@example.invalid",
        "commit", "-q", "--allow-empty", "-m", "probe",
        environment=environment,
    )  # fmt: skip
    return BACKGROUND_WORK.search(str(done.stderr)) is not None


def sentinel_environment(sentinel: Path) -> dict[str, str]:
    """The environment a git hook gives its children: GIT_DIR and GIT_INDEX_FILE."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_DIR=str(sentinel / ".git"), GIT_INDEX_FILE=str(sentinel / ".git/index"))
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    # Auto-maintenance would race the snapshot with a transient objects/maintenance.lock.
    environment.update(
        GIT_CONFIG_COUNT="2",
        GIT_CONFIG_KEY_0="maintenance.auto",
        GIT_CONFIG_VALUE_0="false",
        GIT_CONFIG_KEY_1="gc.auto",
        GIT_CONFIG_VALUE_1="0",
    )
    return environment


def make_sentinel(root: Path) -> Path:
    """A committed repository whose files must be byte-identical after a hook-run script."""
    sentinel = root / "sentinel"
    sentinel.mkdir()
    clean = clean_git_environment()
    run_git(sentinel, "init", "-q", "-b", "master", environment=clean)
    # A commit starts `git maintenance run --auto --detach`. The detached process holds
    # objects/maintenance.lock for a moment after the commit returned; on a hosted runner the
    # first snapshot caught it and the script under test was blamed for the difference. Switch
    # auto-maintenance off in the sentinel itself, before its baseline commit.
    for key, value in SENTINEL_CONFIG:
        run_git(sentinel, "config", key, value, environment=clean)
    (sentinel / "tracked").write_text("caller work\n")
    run_git(sentinel, "add", "tracked", environment=clean)
    run_git(
        sentinel,
        "-c", "user.name=Sentinel", "-c", "user.email=sentinel@example.invalid",
        "commit", "-qm", "sentinel baseline",
        environment=clean,
    )  # fmt: skip
    return sentinel


def sentinel_damage(script: str, directory: Path) -> list[str]:
    """Run `script` from the checkout root under a sentinel GIT_DIR; list changed sentinel files."""
    sentinel = make_sentinel(directory)
    before = snapshot(sentinel)
    path = Path(script) if Path(script).is_absolute() else ROOT / script
    executable = sys.executable if path.suffix == ".py" else BASH
    run_command(
        [executable, str(path)],
        allowed_executables=(executable,),
        cwd=ROOT,
        env=sentinel_environment(sentinel),
        text=True,
        capture_output=True,
        timeout_seconds=600,
        check=False,
    )
    after = snapshot(sentinel)
    return sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))


def snapshot(directory: Path) -> dict[str, bytes]:
    """Capture config, refs, object store, index and work without refreshing Git."""
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in directory.rglob("*")
        if path.is_file()
    }


def clean_git_environment() -> dict[str, str]:
    """Return a deterministic environment with all caller Git state removed."""
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    clean.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, LC_ALL="C")
    return clean


def run_git(path: Path, *args: str, environment: dict[str, str]) -> CommandResult:
    """Run Git only inside a disposable fixture repository."""
    return run_command(
        [GIT, "-C", str(path), *args],
        allowed_executables=(GIT,),
        env=environment,
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=60,
    )


def linked_hook_command(root: Path, old_command: bool) -> list[str]:
    """Select the destructive control or the isolated fixed helper."""
    if old_command:
        return [GIT, "init", "-q", str(root / "old-fixture")]
    return [
        sys.executable,
        str(ROOT / "scripts/ci/tests/test_level_zero_single_source.py"),
        "LevelZeroSingleSource.test_workflow_checker_entrypoint_retains_container_validation",
    ]


class GitFixtureIsolation(unittest.TestCase):
    def exercise(self, helper: str, variables: tuple[str, ...]) -> None:
        with tempfile.TemporaryDirectory(prefix="git-fixture-isolation-") as directory:
            root = Path(directory)
            caller = root / "caller"
            caller.mkdir()
            temporary = root / "scratch"
            temporary.mkdir()
            # Even the adversarial test's setup must never inherit a real
            # caller's repository, index, object store or config overrides.
            clean = clean_git_environment()

            def git(*args: str) -> None:
                run_git(caller, *args, environment=clean)

            git("init", "-q", "-b", "master")
            git("config", "user.name", "Isolation Test")
            git("config", "user.email", "fixture@example.invalid")
            (caller / "tracked").write_text("committed caller work\n")
            git("add", "tracked")
            git("commit", "-qm", "test: caller baseline")
            (caller / "staged").write_text("unique staged caller work\n")
            git("add", "staged")
            (caller / "tracked").write_text("unique unstaged caller work\n")
            poison = {
                "GIT_DIR": str(caller / ".git"),
                "GIT_COMMON_DIR": str(caller / ".git"),
                "GIT_WORK_TREE": str(caller),
                "GIT_INDEX_FILE": str(caller / ".git/index"),
                "GIT_CONFIG_PARAMETERS": "'core.bare=true' 'fixture.marker=inherited'",
            }
            environment = {
                **clean,
                **{key: poison[key] for key in variables},
                "TMPDIR": str(temporary),
            }
            before = snapshot(caller)
            executable = sys.executable if helper.endswith(".py") else BASH
            result = run_command(
                [executable, str(ROOT / helper)],
                allowed_executables=(executable,),
                cwd=root,
                env=environment,
                text=True,
                capture_output=True,
                timeout_seconds=90,
                check=False,
            )
            after = snapshot(caller)
            changed = sorted(
                key for key in before.keys() | after.keys() if before.get(key) != after.get(key)
            )
            self.assertEqual(
                changed, [], f"{helper} changed caller files under {variables}: {changed}"
            )
            self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])

    def check_helper(self, helper: str) -> None:
        for variables in (*((variable,) for variable in VARIABLES), VARIABLES):
            with self.subTest(helper=helper, variables=variables):
                self.exercise(helper, variables)

    def test_replay_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_stack.py")

    def test_smoke_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_smoke_safety.py")

    def test_cleanup_fixture(self) -> None:
        self.check_helper("scripts/dev/test-cleanup-agent-state.sh")

    def test_state_md_resolver_fixture(self) -> None:
        self.check_helper("scripts/dev/test-resolve-state-md-conflict.py")

    def test_dependency_classifier_fixture(self) -> None:
        self.check_helper("scripts/ci/test-classify-dependency-pr.sh")

    def test_level_zero_fixture(self) -> None:
        self.check_helper("scripts/ci/tests/test_level_zero_single_source.py")

    def test_real_linked_worktree_hook_preserves_shared_repository(self) -> None:
        # A real Git hook supplies GIT_DIR even when its caller has no Git
        # variables. Keep both the old-command control and fixed helper inside
        # fresh disposable linked repositories, never the developer's checkout.
        for old_command in (True, False):
            with (
                self.subTest(old_command=old_command),
                tempfile.TemporaryDirectory(prefix="git-linked-hook-") as directory,
            ):
                root = Path(directory)
                caller = root / "caller"
                linked = root / "linked"
                caller.mkdir()
                git = partial(run_git, environment=clean_git_environment())

                git(caller, "init", "-q", "-b", "master")
                git(caller, "config", "user.name", "Linked Hook Test")
                git(caller, "config", "user.email", "fixture@example.invalid")
                (caller / "tracked").write_text("committed caller work\n")
                git(caller, "add", "tracked")
                git(caller, "commit", "-qm", "test: linked hook baseline")
                git(caller, "worktree", "add", "-q", "-b", "linked", str(linked))
                for path in (caller, linked):
                    (path / "staged").write_text("unique staged work\n")
                    git(path, "add", "staged")
                    (path / "tracked").write_text("unique unstaged work\n")
                self.assertEqual(
                    git(caller, "config", "--local", "--get", "core.bare").stdout, "false\n"
                )
                marker = root / "hook-environment.json"
                command = linked_hook_command(root, old_command)
                hook = caller / ".git/hooks/pre-commit"
                hook.write_text(
                    f"#!{sys.executable}\n"
                    "import json, os, subprocess\nfrom pathlib import Path\n"
                    f"Path({str(marker)!r}).write_text(json.dumps({{'GIT_DIR': os.environ.get('GIT_DIR')}}))\n"
                    f"subprocess.run({command!r}, check=True)\n"
                )
                hook.chmod(0o700)
                before_caller, before_linked = snapshot(caller), snapshot(linked)
                git(linked, "hook", "run", "pre-commit")
                inherited = json.loads(marker.read_text())["GIT_DIR"]
                self.assertEqual((linked / inherited).resolve(), caller / ".git/worktrees/linked")
                after_caller, after_linked = snapshot(caller), snapshot(linked)
                if old_command:
                    self.assertNotEqual(after_caller[".git/config"], before_caller[".git/config"])
                    self.assertIn(b"bare = true", after_caller[".git/config"])
                else:
                    self.assertEqual(after_caller, before_caller)
                    self.assertEqual(after_linked, before_linked)

    def test_no_script_writes_a_fixture_identity_into_a_git_config(self) -> None:
        today = datetime.date.today().isoformat()
        offenders = []
        for name in tracked_files():
            if not name.endswith(SUFFIXES) and Path(name).name != "Makefile":
                continue
            path = ROOT / name
            if not path.is_file():
                continue
            if not identity_writes(path.read_text(encoding="utf-8", errors="replace")):
                continue
            if name not in IDENTITY_EXCEPTION_FILES or today > IDENTITY_EXCEPTION_EXPIRY:
                offenders.append(name)
        self.assertEqual(
            offenders,
            [],
            "fixture identity written to a git config; use GIT_AUTHOR_*/GIT_COMMITTER_* "
            "(scripts/lib/clean-git-env.sh) or `git -c user.*=`: " + ", ".join(offenders),
        )

    def test_identity_exceptions_are_live_and_needed(self) -> None:
        self.assertGreaterEqual(
            IDENTITY_EXCEPTION_EXPIRY, datetime.date.today().isoformat(), "exceptions expired"
        )
        for name in IDENTITY_EXCEPTION_FILES:
            with self.subTest(name=name):
                text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
                self.assertTrue(identity_writes(text), "no longer needed: remove the exception")

    def test_identity_rule_refuses_the_planted_old_scripts(self) -> None:
        # Negative controls: the exact lines of the scripts that leaked `t@t` on 2026-10-08.
        for planted in (
            'cd "$tmp"\ngit init -q\ngit config user.email t@t\ngit config user.name t\n',
            'git -C "$dst" config user.email t@t\n',
            '        self.git("config", "user.email", "test@example.invalid")\n',
        ):
            with self.subTest(planted=planted):
                self.assertTrue(identity_writes(planted))
        for allowed in (
            "git -c user.name=t -c user.email=t@example.invalid commit -qm x\n",
            "export GIT_AUTHOR_EMAIL=t@example.invalid\n",
            "git config --get user.email\n",
        ):
            with self.subTest(allowed=allowed):
                self.assertFalse(identity_writes(allowed))

    def test_planted_leaking_script_damages_the_sentinel(self) -> None:
        # Runtime negative controls, each run the way a hook runs it: the old
        # test-preflight-msvcism.sh pattern (identity), the two other keys that reached the
        # shared config (core.bare, uploadpack.*), and a clone. Each must change the sentinel;
        # the gate below must be able to see that.
        head = '#!/usr/bin/env bash\nset -euo pipefail\ntmp=$(mktemp -d)\ncd "$tmp"\ngit init -q\n'
        planted = {
            "identity": (
                head + "git config user.email t@t\ngit config user.name t\n",
                ".git/config",
            ),
            "core.bare": (head + "git config core.bare true\n", ".git/config"),
            "uploadpack": (
                head + "git config uploadpack.allowAnySHA1InWant true\n",
                ".git/config",
            ),
            "clone": (
                head + "git -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty"
                ' -m x\ngit clone -q "$tmp" "$tmp/copy"\n',
                "",
            ),
        }
        for label, (body, expected) in planted.items():
            with (
                self.subTest(leak=label),
                tempfile.TemporaryDirectory(prefix="git-identity-planted-") as directory,
            ):
                root = Path(directory)
                leak = root / "test-planted-leak.sh"
                leak.write_text(body)
                changed = sentinel_damage(str(leak), root)
                self.assertNotEqual(changed, [], f"{label}: the sentinel saw nothing")
                if expected:
                    self.assertIn(expected, changed)

    def test_the_sentinel_starts_no_background_maintenance(self) -> None:
        with tempfile.TemporaryDirectory(prefix="git-identity-maintenance-") as directory:
            root = Path(directory)
            # Negative control: a repository without the two settings starts maintenance on
            # every commit, which is the process that raced the snapshot.
            plain = root / "plain"
            plain.mkdir()
            run_git(plain, "init", "-q", "-b", "master", environment=clean_git_environment())
            self.assertTrue(commit_starts_background_work(plain))
            sentinel = make_sentinel(root)
            self.assertFalse(commit_starts_background_work(sentinel))
            # Two snapshots with nothing in between are equal: no lock file comes or goes.
            self.assertEqual(snapshot(sentinel), snapshot(sentinel))

    def test_the_fixture_helper_starts_no_background_maintenance(self) -> None:
        # A detached repack deleted the loose objects of a scratch repository while
        # test-default-model-single-source.sh copied it (`cp: cannot stat .git/objects/..`).
        # A commit after scripts/lib/clean-git-env.sh must start neither maintenance nor gc;
        # the same commit after drop-git-env.sh alone is the control that does.
        script = (
            'set -euo pipefail\n. "$1"\ntmp=$(mktemp -d)\ncd "$tmp"\ngit init -q\n'
            "GIT_TRACE=1 git -c user.name=F -c user.email=f@example.invalid "
            'commit -q --allow-empty -m x 2>&1\nrm -rf "$tmp"\n'
        )

        def trace(helper: str) -> str:
            done = run_command(
                [BASH, "-c", script, "trace", str(ROOT / "scripts/lib" / helper)],
                allowed_executables=(BASH,),
                env=clean_git_environment(),
                text=True,
                capture_output=True,
                check=True,
                timeout_seconds=60,
            )
            return str(done.stdout)

        self.assertRegex(trace("drop-git-env.sh"), BACKGROUND_WORK)
        self.assertNotRegex(trace("clean-git-env.sh"), BACKGROUND_WORK)

    def test_the_isolated_form_of_each_planted_script_leaves_the_sentinel_alone(self) -> None:
        # The same commands after scripts/lib/clean-git-env.sh: the controls above fail for
        # the missing isolation, not for the commands.
        helper = ROOT / "scripts/lib/clean-git-env.sh"
        body = (
            f'#!/usr/bin/env bash\nset -euo pipefail\n. "{helper}"\ntmp=$(mktemp -d)\ncd "$tmp"\n'
            "git init -q\ngit config core.bare false\n"
            "git config uploadpack.allowAnySHA1InWant true\n"
            'git commit -q --allow-empty -m x\ngit clone -q "$tmp" "$tmp/copy"\n'
        )
        with tempfile.TemporaryDirectory(prefix="git-identity-isolated-") as directory:
            root = Path(directory)
            script = root / "test-planted-isolated.sh"
            script.write_text(body)
            self.assertEqual(sentinel_damage(str(script), root), [])

    def test_no_script_creates_or_configures_a_repository_unisolated(self) -> None:
        self.check_python_isolation_controls()
        today = datetime.date.today().isoformat()
        offenders = unisolated_offenders(ROOT, tracked_files(), today)
        self.assertEqual(
            offenders,
            [],
            "git creates or configures a repository with the caller's GIT_* still set; source "
            "scripts/lib/clean-git-env.sh (or drop-git-env.sh), or build the environment "
            'without keys that start with "GIT_":\n' + "\n".join(offenders),
        )

    def check_python_isolation_controls(self) -> None:
        """Replay rejected filters, required config keys and accepted fixture forms."""
        self.test_isolation_rule_refuses_underscore_git_helpers()
        self.test_isolation_rule_ignores_read_only_and_unrelated_helpers()
        name = "tests/test_fixture.py"
        create = 'git("init", "-q")\n'
        canonical = '{k: v for k, v in os.environ.items() if not k.startswith("GIT_")}'
        config = 'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n'
        for condition in (
            'not k.startswith("GIT_") or k.startswith("GIT_CONFIG_")',
            'not k.startswith("GIT_") and not k.startswith("GIT_CONFIG")',
            'k.startswith("GIT_CONFIG_")',
            'not k.startswith("GIT_CONFIG_")',
        ):
            for expression in (
                canonical.replace('not k.startswith("GIT_")', condition),
                f"filter(lambda k: {condition}, os.environ)",
                f"[k for k in os.environ if {condition}]",
                f"(k for k in os.environ if {condition})",
            ):
                text = "env = " + expression + "\n"
                with self.subTest(condition=condition, expression=expression):
                    diagnostic = unisolated_git(name, text + config + create)
                    self.assertIn(f"{name}:1", diagnostic)
                    self.assertIn(condition, diagnostic)
                    self.assertIn(
                        f"{name}:2", unisolated_git(name, canonical + "\n" + text + create)
                    )
        for config_text, missing in (
            ('env.update(GIT_CONFIG_NOSYSTEM="1")\n', "GIT_CONFIG_GLOBAL"),
            ("env.update(GIT_CONFIG_GLOBAL=os.devnull)\n", "GIT_CONFIG_SYSTEM"),
        ):
            with self.subTest(missing=missing):
                diagnostic = unisolated_git(
                    name, "env = " + canonical + "\n" + config_text + create
                )
                self.assertIn(name, diagnostic)
                self.assertIn(missing, diagnostic)
        self.test_python_ast_barrier_refuses_non_executable_forms()
        self.test_python_ast_barrier_refuses_post_barrier_environ_unpack()
        self.test_python_ast_barrier_refuses_same_expression_override()
        self.test_python_ast_marker_refuses_comments_and_strings()
        self.test_python_ast_comprehension_and_filter_allowlist()
        self.check_accepted_python_isolation(canonical, create)
        self.check_planted_python_isolation()

    def test_python_ast_barrier_refuses_non_executable_forms(self) -> None:
        """Refuse barriers appearing only in strings, comments, docstrings or setdefault."""
        name = "tests/test_fixture.py"
        canonical = 'env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
        create = 'git("init", "-q")\n'
        for header in (
            '"""env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")"""\n',
            '# env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n',
            "note = 'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM=\"1\")'\n",
            'env.setdefault("GIT_CONFIG_GLOBAL", os.devnull)\n'
            'env.setdefault("GIT_CONFIG_NOSYSTEM", "1")\n',
        ):
            with self.subTest(header=header):
                diagnostic = unisolated_git(name, header + canonical + create)
                self.assertIn("missing GIT_CONFIG_GLOBAL", diagnostic)

    def test_python_ast_barrier_refuses_post_barrier_environ_unpack(self) -> None:
        """Accept pre-barrier **os.environ and refuse post-barrier or mid-barrier unpacks."""
        name = "tests/test_fixture.py"
        canonical = 'env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
        create = 'git("init", "-q")\n'
        barriers = '"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"'
        kw_barriers = 'GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1"'
        for accepted in (
            f"{{**os.environ, {barriers}}}",
            f"dict(**os.environ, {kw_barriers})",
            f"dict(os.environ, {kw_barriers})",
            f"dict({{**os.environ}}, {kw_barriers})",
            f"{{**os.environ, **{{{barriers}}}}}",
            f"dict(**os.environ, **{{{barriers}}})",
        ):
            with self.subTest(accepted=accepted):
                self.assertEqual(
                    unisolated_git(name, canonical + f"out = {accepted}\n" + create), ""
                )
        expected = (
            f"{name}:2: **os.environ after GIT_CONFIG_* barrier keys "
            "lets the caller override them; place the expansion first"
        )
        for rejected in (
            f"{{{barriers}, **os.environ}}",
            f"{{{barriers}, **dict(os.environ)}}",
            f"{{{barriers}, **os.environ.copy()}}",
            '{"GIT_CONFIG_GLOBAL": os.devnull, **os.environ, "GIT_CONFIG_NOSYSTEM": "1"}',
            '{"GIT_CONFIG_NOSYSTEM": "1", **os.environ, "GIT_CONFIG_GLOBAL": os.devnull}',
            f"dict({kw_barriers}, **os.environ)",
            f"env.update({kw_barriers}, **os.environ.copy())",
            f"dict({{{barriers}}}, **os.environ)",
            f"env.update({{{barriers}}}, **os.environ.copy())",
            f"dict(dict({kw_barriers}), **os.environ)",
            f"{{**{{{barriers}}}, **os.environ}}",
            f"dict(**{{{barriers}}}, **os.environ)",
            f"{{**dict({kw_barriers}), **os.environ}}",
            f"{{**{{**{{{barriers}}}}}, **os.environ}}",
        ):
            with self.subTest(rejected=rejected):
                self.assertEqual(
                    unisolated_git(name, canonical + f"out = {rejected}\n" + create), expected
                )
        two_displays = (
            canonical
            + 'first = {"GIT_CONFIG_GLOBAL": os.devnull, **os.environ}\n'
            + 'second = {"GIT_CONFIG_NOSYSTEM": "1", **os.environ}\n'
            + create
        )
        self.assertEqual(unisolated_git(name, two_displays), expected)
        create_env = 'git("init", "-q", env=out)\n'
        with self.subTest(call=create_env):
            bad_call = f"out = dict({{{barriers}}}, **os.environ)\n"
            good_call = f"out = dict(os.environ, {kw_barriers})\n"
            self.assertEqual(unisolated_git(name, canonical + bad_call + create_env), expected)
            self.assertEqual(unisolated_git(name, canonical + good_call + create_env), "")

    def test_python_ast_barrier_refuses_same_expression_override(self) -> None:
        """Refuse barrier keys overridden later in the same expression unless by a barrier."""
        prefix = 'env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
        suffix = '\ngit("init", "-q")\n'
        barriers = '{"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}'
        fallback = 'os.environ.get("GIT_CONFIG_GLOBAL", os.devnull)'
        for key, expr in (
            ("GIT_CONFIG_GLOBAL", f"dict({barriers}, GIT_CONFIG_GLOBAL={fallback})"),
            (
                "GIT_CONFIG_GLOBAL",
                '{"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1", '
                '"GIT_CONFIG_GLOBAL": os.environ["X"]}',
            ),
            ("GIT_CONFIG_GLOBAL", f'{{**{barriers}, "GIT_CONFIG_GLOBAL": os.environ["X"]}}'),
            ("GIT_CONFIG_NOSYSTEM", f'dict({barriers}, GIT_CONFIG_NOSYSTEM="0")'),
            (
                "GIT_CONFIG_SYSTEM",
                'env.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull}, '
                'GIT_CONFIG_SYSTEM=os.environ["X"])',
            ),
        ):
            with self.subTest(rejected=expr):
                expected = (
                    f"tests/test_override.py:2: {key} is set to a barrier and then overridden "
                    "in the same expression; keep the barrier value last"
                )
                self.assertEqual(
                    unisolated_git("tests/test_override.py", f"{prefix}out = {expr}{suffix}"),
                    expected,
                )
        for valid in (
            f'dict({barriers}, GIT_CONFIG_GLOBAL="/dev/null")',
            '{"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1", '
            '"GIT_CONFIG_GLOBAL": "/dev/null"}',
            '{"GIT_CONFIG_GLOBAL": os.environ["X"], "GIT_CONFIG_GLOBAL": os.devnull, '
            '"GIT_CONFIG_NOSYSTEM": "1"}',
            '{"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_GLOBAL": os.environ["X"], '
            '"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}',
        ):
            with self.subTest(accepted=valid):
                self.assertEqual(
                    unisolated_git("tests/test_override.py", f"{prefix}out = {valid}{suffix}"),
                    "",
                )

    def test_python_ast_marker_refuses_comments_and_strings(self) -> None:
        """Require AST call markers, check helper-marked fixtures, and report syntax lines."""
        name = "tests/test_fixture.py"
        config = 'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n'
        create = 'git("init", "-q")\n'
        for fake_marker in (
            '"""k.startswith("GIT_") and clean_git_environment()"""\n',
            '# not k.startswith("GIT_") clean_git_environment()\n',
            "text = 'clean_git_environment() k.startswith(\"GIT_\")'\n",
        ):
            with self.subTest(fake_marker=fake_marker):
                self.assertEqual(unisolated_git(name, fake_marker + config + create), 'git("init",')
        for helper_call in ("env = clean_git_environment()\n", "env = mod.git_environment()\n"):
            with self.subTest(helper_call=helper_call):
                self.assertIn(
                    "missing GIT_CONFIG_GLOBAL", unisolated_git(name, helper_call + create)
                )
        broken = unisolated_git("tests/test_broken.py", create + "def broken(\n")
        self.assertIn("tests/test_broken.py:2:", broken)
        self.assertIn("cannot parse Python source", broken)
        self.assertIn("tests/test_broken.py:1:", unisolated_git("tests/test_broken.py", "def f(\n"))
        self.assertEqual(unisolated_git(name, ""), "")

    def test_python_ast_comprehension_and_filter_allowlist(self) -> None:
        """Fail closed on noncanonical conditions, copied os.environ sources and extra clauses."""
        name = "tests/test_fixture.py"
        config = 'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n'
        create = 'git("init", "-q")\n'
        for expression in (
            "{k: v for k, v in os.environ.items() if k not in drop}",
            "{k: v for k, v in os.environ.items() if not k.startswith(PREFIX)}",
            '{k: v for k, v in os.environ.items() if not k.startswith("GIT" + "_")}',
            '{k: v for k, v in os.environ.items() if not k.startswith("GIT" "_")'
            ' or k.startswith("GIT" "_CONFIG_")}',
            '{k: v for k, v in os.environ.copy().items() if not k.startswith("GIT_")'
            ' or k.startswith("GIT_CONFIG_")}',
            '{k: v for k, v in dict(os.environ).items() if not k.startswith("GIT_CONFIG_")}',
            "[k for k in os.environ.copy() if keep(k)]",
            'filter(lambda k: not k.startswith("GIT_") or flag, dict(os.environ).keys())',
            '{k: v for k, v in os.environ.items() for _ in (0,) if not k.startswith("GIT_")'
            ' or k.startswith("GIT_CONFIG_")}',
            '{k: v for k, v in os.environ.items() if not k.startswith("GIT_") if keep(k)}',
            "{k: v for k, v in os.environ.items()}",
            "filter(keep_env, os.environ)",
            "filter(lambda k: helper(k), os.environ)",
        ):
            with self.subTest(expression=expression):
                diagnostic = unisolated_git(name, f"env = {expression}\n" + config + create)
                self.assertIn(f"{name}:1:", diagnostic)

    def check_accepted_python_isolation(self, canonical: str, create: str) -> None:
        """Accept config assignment styles, tuple prefixes and caller-poison selection."""
        for config in (
            'env = {**env, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": "/dev/null"}\n',
            'env = dict(env, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")\n',
            'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n',
            'env["GIT_CONFIG_GLOBAL"] = os.devnull\nenv["GIT_CONFIG_NOSYSTEM"] = "1"\n',
        ):
            for expression in (
                canonical,
                canonical.replace('"GIT_"', '("GIT_", "X_")'),
                canonical.replace("os.environ.items()", "os.environ.copy().items()"),
                canonical.replace("os.environ.items()", "dict(os.environ).items()"),
                'filter(lambda k: not k.startswith("GIT_"), os.environ)',
                'filter(lambda k: not k.startswith("GIT_"), os.environ.copy())',
                "clean_git_environment()",
            ):
                with self.subTest(config=config, expression=expression):
                    text = "env = " + expression + "\n" + config + create
                    self.assertEqual(unisolated_git("tests/test_fixture.py", text), "")
        poison = (
            'caller = dict(env, GIT_CONFIG_PARAMETERS="poison")\n'
            'index = next(key[len("GIT_CONFIG_KEY_"):] for key, value in os.environ.items()\n'
            '    if key.startswith("GIT_CONFIG_KEY_") and value == "gpg.ssh.allowedSignersFile")\n'
        )
        config = 'env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n'
        self.assertEqual(
            unisolated_git(
                "tests/test_poison.py", "env = " + canonical + "\n" + config + poison + create
            ),
            "",
        )
        self.assertEqual(unisolated_git("tools/product.py", canonical + "\n" + create), "")

    def check_planted_python_isolation(self) -> None:
        """A temporary fixture copy fails the static check after re-admission; restore passes."""
        name = "scripts/ci/tests/test_ci_impact.py"
        original = (ROOT / name).read_text(encoding="utf-8")
        old = 'if not k.startswith("GIT_")'
        planted = original.replace(old, old + ' or k.startswith("GIT_CONFIG_")', 1)
        self.assertNotEqual(original, planted)
        today = datetime.date.today().isoformat()
        with tempfile.TemporaryDirectory(prefix="git-filter-planted-") as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True)
            path.write_text(planted, encoding="utf-8")
            rejected = unisolated_offenders(root, [name], today)
            self.assertEqual(len(rejected), 1, rejected)
            self.assertTrue(rejected[0].startswith(name + ":"), rejected)
            self.assertIn('k.startswith("GIT_CONFIG_")', rejected[0])
            path.write_text(original, encoding="utf-8")
            self.assertEqual(unisolated_offenders(root, [name], today), [])
        self.test_isolation_exceptions_enforce_python_fixture_barriers()

    def test_isolation_exceptions_enforce_python_fixture_barriers(self) -> None:
        """ISOLATION_EXCEPTIONS excuses the GIT_* marker only, not Python fixture barriers."""
        name = "scripts/dev/tests/test_merge_train_guard.py"
        self.assertIn(name, ISOLATION_EXCEPTIONS)
        original = (ROOT / name).read_text(encoding="utf-8")
        stripped = original.replace("GIT_CONFIG_GLOBAL=os.devnull, ", "", 1)
        self.assertNotEqual(original, stripped)
        today = datetime.date.today().isoformat()
        with tempfile.TemporaryDirectory(prefix="git-exception-barrier-") as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True)
            path.write_text(stripped, encoding="utf-8")
            rejected = unisolated_offenders(root, [name], today)
            self.assertEqual(len(rejected), 1, rejected)
            self.assertTrue(rejected[0].startswith(name + ":"), rejected)
            self.assertIn("missing GIT_CONFIG_GLOBAL", rejected[0])
            path.write_text(original, encoding="utf-8")
            self.assertEqual(unisolated_offenders(root, [name], today), [])

    def test_known_gap_python_imperative_environment_mutation(self) -> None:
        """Known gap: imperative mutation after copying os.environ is not data-flow tracked."""
        name = "tests/test_fixture.py"
        setup = (
            'clean = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
            'clean.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")\n'
        )
        for mutation in (
            'env = os.environ.copy()\nenv["GIT_DIR"] = "/tmp/caller"\ngit("init", "-q")\n',
            'env = os.environ.copy()\nfor k in list(env):\n    if k == "GIT_DIR":\n'
            '        env.pop(k)\ngit("init", "-q")\n',
        ):
            with self.subTest(mutation=mutation):
                self.assertEqual(unisolated_git(name, setup + mutation), "")

    def test_known_gap_node_and_go_environment_readmission(self) -> None:
        """Known gap: Node and Go marker checks do not inspect filter re-admission or mutation."""
        node_fixture = (
            'const keep = Object.keys(process.env).filter((k) => !k.startsWith("GIT_")'
            ' || k.startsWith("GIT_CONFIG_"));\n'
            'command("git", ["init", "--quiet"], { cwd: fixture });\n'
        )
        go_fixture = (
            '\tif !strings.HasPrefix(entry, "GIT_") || strings.HasPrefix(entry, "GIT_CONFIG_") {\n'
            "\t\tenv = append(env, entry)\n\t}\n"
            '\trunTool(ctx, repo, "git", "init", "--quiet")\n'
        )
        self.assertEqual(unisolated_git("tests/fixture.mjs", node_fixture), "")
        self.assertEqual(unisolated_git("tools/fixture_test.go", go_fixture), "")

    def test_isolation_exceptions_are_live_and_needed(self) -> None:
        self.assertGreaterEqual(
            ISOLATION_EXCEPTION_EXPIRY, datetime.date.today().isoformat(), "exceptions expired"
        )
        for name, reason in ISOLATION_EXCEPTIONS.items():
            with self.subTest(name=name):
                self.assertTrue(reason)
                text = (ROOT / name).read_text(encoding="utf-8", errors="replace")
                self.assertTrue(unisolated_git(name, text), "no longer needed: remove it")

    def test_isolation_rule_refuses_underscore_git_helpers(self) -> None:
        """Underscore-prefixed Git helpers must trigger the isolation check."""
        for text in (
            'self._git("init")\n',
            'self.run_git("init", "-q")\n',
            'run_git(["clone", url])\n',
            'subprocess.run(["git", "-C", str(path), "init"])\n',
            'git("-c", "init.defaultBranch=main", "init")\n',
        ):
            with self.subTest(text=text):
                self.assertIsNotNone(CODE_GIT_WRITE.search(text))
                self.assertTrue(unisolated_git("tests/test_fixture.py", text))

    def test_isolation_rule_ignores_read_only_and_unrelated_helpers(self) -> None:
        """Only init or clone as the subcommand counts; digit is unrelated to git."""
        for text in (
            'self._git("status")\n',
            'self.run_git("status", "--short")\n',
            'self._git("status", "init")\n',
            '_git(temp_dir, "commit", "-q", "-m", "init")\n',
            'digit("init")\n',
            'self.digit("clone", url)\n',
        ):
            with self.subTest(text=text):
                self.assertIsNone(CODE_GIT_WRITE.search(text))
                self.assertEqual(unisolated_git("tests/test_fixture.py", text), "")

    def test_isolation_rule_refuses_planted_unisolated_fixtures(self) -> None:
        """Verify unisolated_git flags unisolated code and passes isolated or harmless snippets."""
        planted = {
            "t.sh": (
                'cd "$tmp"\ngit init -q\n',
                'git -C "$dst" config core.bare true\n',
                "git config --local uploadpack.allowAnySHA1InWant true\n",
                'timeout 600 git -c advice.detachedHead=false clone --quiet "$url" "$dir"\n',
            ),
            "t.py": (
                'subprocess.run(["git", "init", "-q", str(repo)], check=True)\n',
                '        self.git("init", "-q", "-b", "master")\n',
                '    git("config", "uploadpack.allowAnySHA1InWant", "true")\n',
                '    run_git(root, "config", "core.bare", "true")\n',
                '    run((git, "clone", "--bare", str(src), str(dst)))\n',
                '# k.startswith("GIT_")\nself.git("init", "-q")\n',
            ),
            "t.mjs": ('  command("git", ["init", "--quiet"], { cwd: fixture });\n',),
            "t.go": ('\trunTool(ctx, repo, "git", "init", "--quiet")\n',),
        }
        for name, texts in planted.items():
            for text in texts:
                with self.subTest(name=name, text=text):
                    self.assertTrue(unisolated_git(name, text))
        isolated = {
            "t.sh": '. "$ROOT/scripts/lib/clean-git-env.sh"\ngit init -q\ngit config core.bare true\n',
            "u.sh": '. "$ROOT/scripts/lib/drop-git-env.sh"\ngit clone -q "$url" "$dir"\n',
            "t.py": (
                'env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}\n'
                'subprocess.run(["git", "init"], env=env)\n'
            ),
            "u.py": (
                'env = {k: v for k, v in os.environ.items() if not k.startswith(("GIT_", "GH_"))}\n'
                'self.git("config", "core.bare", "true")\n'
            ),
        }
        for name, text in isolated.items():
            with self.subTest(name=name):
                self.assertEqual(unisolated_git(name, text), "")
        harmless = {
            "t.sh": (
                "# git init would be wrong here\n",
                "git config --get user.email\n",
                "git config --list\n",
                'git -C "$repo" log --oneline\n',
            ),
            "t.py": (
                "",
                'value = git("config", "--get", "user.email")\n',
                'state = registration.get("init", "")\n',
                '# self.git("init") is what the fixture used to do\n',
            ),
        }
        for name, texts in harmless.items():
            for text in texts:
                with self.subTest(name=name, text=text):
                    self.assertEqual(unisolated_git(name, text), "")

    def sentinel_run(self, shard: str) -> None:
        """Run one shard's scripts the way a hook runs them; none may change the sentinel."""
        scripts = sentinel_shards(scratch_scripts())[shard]
        self.assertTrue(scripts, f"shard {shard} holds no script")

        def damage(script: str) -> list[str]:
            with tempfile.TemporaryDirectory(prefix="git-identity-sentinel-") as directory:
                return sentinel_damage(script, Path(directory))

        # Four workers: the scripts are independent and each owns its sentinel.
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = dict(zip(scripts, pool.map(damage, scripts), strict=True))
        for script, changed in results.items():
            with self.subTest(script=script):
                self.assertEqual(changed, [], f"{script} changed the hook's GIT_DIR repository")

    @per_test_timeout(DEFAULT_MODEL_SHARD_TIMEOUT_SECONDS)
    def test_sentinel_shard_default_model(self) -> None:
        self.sentinel_run("default_model")

    def test_sentinel_shard_hooks(self) -> None:
        self.sentinel_run("hooks")

    def test_sentinel_shard_rest_0(self) -> None:
        self.sentinel_run("rest_0")

    def test_sentinel_shard_rest_1(self) -> None:
        self.sentinel_run("rest_1")

    def test_the_shards_hold_every_scratch_script_once(self) -> None:
        scripts = scratch_scripts()
        self.assertGreater(len(scripts), 30, "discovery found too few scratch-repository scripts")
        shards = sentinel_shards(scripts)
        self.assertEqual(partition_faults(scripts, shards), [])
        # A slow script that was renamed or removed must not slide into a rest shard unseen.
        for label, names in SLOW_SHARDS.items():
            self.assertEqual(shards[label], list(names), f"shard {label} names a missing script")
        # Every shard has a test method, and every shard test names a shard.
        methods = {
            name.removeprefix("test_sentinel_shard_")
            for name in dir(self)
            if name.startswith("test_sentinel_shard_")
        }
        self.assertEqual(methods, set(shards))

    def test_a_script_outside_every_shard_is_reported(self) -> None:
        scripts = ["a/test-one.sh", "b/test_two.py", "c/test-three.sh", *SLOW_SHARDS["hooks"]]
        shards = sentinel_shards(scripts)
        self.assertEqual(partition_faults(scripts, shards), [])
        # A script added to the tree lands in a rest shard without any list being edited.
        grown = [*scripts, "d/test-new.sh"]
        self.assertEqual(partition_faults(grown, sentinel_shards(grown)), [])
        # Planted: the same script dropped from its shard, listed twice, or not a script.
        self.assertEqual(partition_faults(grown, shards), ["d/test-new.sh: in no shard"])
        doubled = {**shards, "rest_1": [*shards["rest_1"], "a/test-one.sh"]}
        self.assertEqual(partition_faults(scripts, doubled), ["a/test-one.sh: in 2 shards"])
        stale = {**shards, "hooks": [*shards["hooks"], "e/test-gone.sh"]}
        self.assertEqual(partition_faults(scripts, stale), ["e/test-gone.sh: not a scratch script"])

    def test_local_and_required_ci_hook_registration(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = config.split("      - id: test-git-fixture-isolation\n", 1)[1].split(
            "      - id:", 1
        )[0]
        self.assertIn("entry: python3 scripts/ci/test_git_fixture_isolation.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("pass_filenames: false", hook)
        pattern = hook.split("files: '", 1)[1].split("'", 1)[0]
        for path in (
            "scripts/ci/test_git_fixture_isolation.py",
            "scripts/ci/test_ffmpeg_patch_stack.py",
            "scripts/ci/test_ffmpeg_patch_smoke_safety.py",
            "scripts/dev/test-cleanup-agent-state.sh",
            "scripts/dev/test-resolve-state-md-conflict.py",
            "scripts/dev/resolve-state-md-conflict.py",
            "scripts/ci/test-classify-dependency-pr.sh",
            "scripts/ci/tests/test_level_zero_single_source.py",
            ".pre-commit-config.yaml",
        ):
            self.assertIsNotNone(re.search(pattern, path), path)
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        self.assertIn("name: Pre-Commit", workflow)
        self.assertIn("scripts/ci/precommit_scope.py", workflow)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Generate and verify hash-locked Python dependency inputs.

The committed lock files are the install authority.  The manifest binds each
lock to its source bytes and to one reviewed uv release; ``check`` is entirely
offline, while ``write`` is the explicit networked refresh operation.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import importlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, cast

try:
    yaml: Any = importlib.import_module("yaml")
except ImportError:
    yaml = None

MANIFEST_PATH = Path("requirements/locks/manifest.json")
HASH_RE = re.compile(r"--hash=sha256:[0-9a-f]{64}(?:\s|$)")
EXACT_REQUIREMENT_RE = re.compile(r"^[A-Za-z0-9_.-]+(?:\[[A-Za-z0-9_,.-]+\])?==[^;\s]+")
PIP_NAME_RE = re.compile(
    r"(?:^|[/\\]|\$[\(\{][A-Za-z0-9_]*)pip(?:3|[0-9.]*)?(?:\.exe|[\)\}])?$",
    re.IGNORECASE,
)
PYTHON_NAME_RE = re.compile(r"(?:^|[/\\])(?:python(?:3|[0-9.]*)?|py)(?:\.exe)?$", re.IGNORECASE)
LOCK_HEADER = "# VMAFx hash lock; regenerate with: make python-locks-write"
OPTIONS_WITH_VALUES = {
    "-b",
    "--build",
    "-c",
    "--cache-dir",
    "--cert",
    "--client-cert",
    "--config-settings",
    "--constraint",
    "-e",
    "--editable",
    "--extra-index-url",
    "-f",
    "--find-links",
    "-i",
    "--index-url",
    "--prefix",
    "--proxy",
    "-r",
    "--requirement",
    "--retries",
    "--root",
    "--src",
    "-t",
    "--target",
    "--timeout",
    "--trusted-host",
}
SHORT_OPTIONS_WITH_VALUES = {"b", "c", "e", "f", "i", "r", "t", "d", "C"}
SUPPORTED_ABSOLUTE_PREFIXES = ("/build/", "/vmaf/", "/tmp/")  # noqa: S108
REPO_LOCAL_WORKFLOW_PREFIXES = (
    "ai/",
    "compat/",
    "requirements/",
    "scripts/",
    "tools/",
    "mcp-server/",
    "core/",
    "python/",
    "dev/",
    "docker/",
    "model/",
    "testdata/",
    "LICENSES/",
)
WORKFLOW_REQ_FLAGS = {"-r", "--requirement", "-c", "--constraint"}
WORKFLOW_REQ_PREFIXES = ("-r=", "--requirement=", "-c=", "--constraint=")
WORKFLOW_EDIT_FLAGS = {"-e", "--editable"}
WORKFLOW_EDIT_PREFIXES = ("-e=", "--editable=")
SHELL_RUNNERS = {
    "sh",
    "bash",
    "zsh",
    "/bin/sh",
    "/bin/bash",
    "/bin/zsh",
    "/usr/bin/sh",
    "/usr/bin/bash",
    "/usr/bin/zsh",
}
WORKFLOW_JOB_INDENT = 2
WORKFLOW_STEPS_INDENT = 4
SIMPLE_YAML_MAPPING_RE = re.compile(
    r'^(?:([a-zA-Z0-9_-]+)|"([a-zA-Z0-9_-]+)"|\'([a-zA-Z0-9_-]+)\')\s*:\s*(.*)$'
)


class ContractError(RuntimeError):
    """A lock or install surface violates the repository contract."""


def _reject_duplicate_json_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"{MANIFEST_PATH}: duplicate JSON key {key!r}")
        result[key] = value
    return result


INSECURE_COMPILE_FLAGS = {
    "-i",
    "--index-url",
    "--extra-index-url",
    "-f",
    "--find-links",
    "--trusted-host",
    "--no-index",
    "--default-index",
    "--pre",
}
INSECURE_COMPILE_PREFIXES = (
    "-i=",
    "--index-url=",
    "--extra-index-url=",
    "-f=",
    "--find-links=",
    "--trusted-host=",
    "--default-index=",
)


def _is_repository_relative_path(path: str) -> bool:
    """Return whether ``path`` stays repo-relative on POSIX and Windows."""

    if not path or path != path.strip():
        return False
    if "://" in path or path.startswith(("git+", "hg+", "svn+", "bzr+")):
        return False
    normalized = path.replace("\\", "/")
    posix_path = PurePosixPath(normalized)
    windows_path = PureWindowsPath(path)
    return not (
        posix_path.is_absolute()
        or windows_path.is_absolute()
        or bool(windows_path.drive)
        or ".." in posix_path.parts
    )


def _validate_manifest_output(output: Any) -> list[str]:
    if not isinstance(output, str) or not output:
        return ["manifest lock entry output must be a non-empty string"]
    if "://" in output or not _is_repository_relative_path(output):
        return [
            f"manifest lock output {output!r} cannot be remote, absolute, or traverse outside the repository"
        ]
    return []


def _validate_manifest_inputs(output: str, inputs: Any) -> list[str]:
    if not isinstance(inputs, list) or not inputs or not all(isinstance(p, str) for p in inputs):
        return [f"{output}: manifest inputs must be a non-empty array of paths"]
    problems = []
    if len(inputs) != len(set(inputs)):
        problems.append(f"{output}: manifest inputs contains duplicate paths: {inputs}")
    for inp in inputs:
        if (
            "://" in inp
            or inp.startswith(("git+", "hg+", "svn+", "bzr+"))
            or not _is_repository_relative_path(inp)
        ):
            problems.append(
                f"{output}: manifest input {inp!r} cannot be remote, absolute, or traverse outside the repository"
            )
    return problems


def _validate_manifest_compile_args(output: str, compile_args: Any) -> list[str]:
    if not isinstance(compile_args, list) or not all(isinstance(a, str) for a in compile_args):
        return [f"{output}: manifest compile_args must be an array of strings"]
    problems = []
    for arg in compile_args:
        lowered = arg.lower()
        if arg in {"-o", "--output-file"} or arg.startswith(("-o=", "--output-file=")):
            problems.append(
                f"{output}: manifest compile_args cannot specify -o or --output-file override"
            )
        if lowered in INSECURE_COMPILE_FLAGS or lowered.startswith(INSECURE_COMPILE_PREFIXES):
            problems.append(
                f"{output}: manifest compile_args contains insecure index/find-links/host argument {arg!r}"
            )
        if "://" in arg or arg.startswith(("git+", "hg+", "svn+", "bzr+")):
            problems.append(
                f"{output}: manifest compile_args cannot reference remote URLs: {arg!r}"
            )
        if ".." in Path(arg).parts or (Path(arg).is_absolute() and not arg.startswith("-")):
            problems.append(
                f"{output}: manifest compile_args cannot traverse outside the repository or use absolute paths: {arg!r}"
            )
    return problems


def _normalize_install_target(target: str) -> str:
    """Normalize separators before comparing manifest-owned install paths."""

    return target.replace("\\", "/")


def _check_alias_security(output: str, alias: str, norm: str, context: Any) -> list[str]:
    problems: list[str] = []
    if re.match(r"^[A-Za-z]:", alias):
        problems.append(
            f"{output}: manifest install alias {alias!r} must not be a Windows drive path"
        )
    if norm.startswith("/"):
        if not any(norm.startswith(prefix) for prefix in SUPPORTED_ABSOLUTE_PREFIXES):
            problems.append(
                f"{output}: manifest install alias {alias!r} is an unsupported absolute path"
            )
        if ".." in norm.split("/"):
            problems.append(
                f"{output}: manifest install alias {alias!r} cannot contain path traversal"
            )
    else:
        clean = norm.replace("${PSScriptRoot}", "scripts/setup").replace("$REPO_ROOT", "")
        if clean.startswith("/"):
            clean = clean[1:]
        base_dir = (
            context
            if (isinstance(context, str) and context not in {"container-build", "container"})
            else ""
        )
        candidate = f"{base_dir}/{clean}" if base_dir else clean
        resolved = os.path.normpath(candidate)
        if resolved.startswith("..") or resolved == "..":
            problems.append(
                f"{output}: manifest install alias {alias!r} traverses outside the repository"
            )
    return problems


def _extract_bound_consumers(alias_spec: dict[str, Any]) -> list[str]:
    c_val = alias_spec.get("consumer")
    c_list = alias_spec.get("consumers")
    bound: list[str] = []
    if isinstance(c_val, str):
        bound.append(c_val)
    elif isinstance(c_val, list):
        bound.extend(c for c in c_val if isinstance(c, str))
    if isinstance(c_list, list):
        bound.extend(c for c in c_list if isinstance(c, str))
    elif isinstance(c_list, str):
        bound.append(c_list)
    return bound


def _validate_alias_consumer_paths(output: str, alias: str, item: dict[str, Any]) -> list[str]:
    return [
        f"{output}: manifest install alias {alias!r} consumer path "
        f"{consumer_path!r} must be repository-relative without traversal"
        for consumer_path in _extract_bound_consumers(item)
        if not _is_repository_relative_path(consumer_path)
    ]


def _validate_alias_item(
    output: str,
    item: Any,
    normalized_output: str,
    seen_aliases: set[str],
) -> list[str]:
    if not isinstance(item, dict):
        return [
            f"{output}: manifest install_aliases must contain objects with alias, consumer, and context"
        ]
    alias = item.get("alias")
    if not isinstance(alias, str) or not alias:
        return [f"{output}: manifest install alias must have a non-empty 'alias' string"]
    problems = []
    consumer = item.get("consumer")
    consumers = item.get("consumers")
    if consumer is None and consumers is None:
        problems.append(f"{output}: manifest install alias {alias!r} must specify a consumer")
    elif consumer is not None and not (
        (isinstance(consumer, str) and consumer)
        or (
            isinstance(consumer, list)
            and consumer
            and all(isinstance(c, str) and c for c in consumer)
        )
    ):
        problems.append(
            f"{output}: manifest install alias {alias!r} consumer must be a non-empty string or array"
        )
    elif consumers is not None and not (
        (
            isinstance(consumers, list)
            and consumers
            and all(isinstance(c, str) and c for c in consumers)
        )
        or (isinstance(consumers, str) and consumers)
    ):
        problems.append(
            f"{output}: manifest install alias {alias!r} consumers must be a non-empty array"
        )

    problems.extend(_validate_alias_consumer_paths(output, alias, item))

    context = item.get("context") or item.get("provenance")
    if not isinstance(context, str) or not context:
        problems.append(
            f"{output}: manifest install alias {alias!r} must specify a context or provenance"
        )

    if alias != alias.strip():
        problems.append(
            f"{output}: manifest install alias {alias!r} must not contain surrounding whitespace"
        )
    if not alias.endswith(".txt"):
        problems.append(f"{output}: manifest install alias {alias!r} must end with .txt")
    if "/" not in alias and "\\" not in alias:
        problems.append(f"{output}: manifest install alias {alias!r} must not be a bare filename")
    if "://" in alias or alias.startswith(("git+", "hg+", "svn+", "bzr+")):
        problems.append(f"{output}: manifest install alias {alias!r} must be a local path")

    norm = _normalize_install_target(alias)
    if norm in seen_aliases:
        problems.append(f"{output}: manifest install_aliases contains duplicate alias: {alias!r}")
    seen_aliases.add(norm)

    if norm == normalized_output:
        problems.append(f"{output}: manifest install alias {alias!r} duplicates output")

    problems.extend(_check_alias_security(output, alias, norm, context))
    return problems


def _validate_manifest_install_aliases(output: str, aliases: Any) -> list[str]:
    if aliases is None:
        return []
    if not isinstance(aliases, list):
        return [f"{output}: manifest install_aliases must be an array of objects"]
    problems = []
    normalized_output = _normalize_install_target(output)
    seen_aliases: set[str] = set()

    for item in aliases:
        problems.extend(_validate_alias_item(output, item, normalized_output, seen_aliases))
    return problems


def validate_manifest_entry(entry: dict[str, Any]) -> list[str]:
    output = entry.get("output")
    output_problems = _validate_manifest_output(output)
    if output_problems or not isinstance(output, str):
        return output_problems
    return [
        *_validate_manifest_inputs(output, entry.get("inputs")),
        *_validate_manifest_compile_args(output, entry.get("compile_args")),
        *_validate_manifest_install_aliases(output, entry.get("install_aliases")),
    ]


def load_manifest(root: Path) -> dict[str, Any]:
    path = root / MANIFEST_PATH
    try:
        data = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"{MANIFEST_PATH}: {error}") from error
    if not isinstance(data, dict) or not isinstance(data.get("locks"), list):
        raise ContractError(f"{MANIFEST_PATH}: expected an object with a locks array")
    version = data.get("uv_version")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", version):
        raise ContractError(f"{MANIFEST_PATH}: uv_version must be an exact release")
    install_targets: set[str] = set()
    for raw_entry in data["locks"]:
        if not isinstance(raw_entry, dict):
            raise ContractError(f"{MANIFEST_PATH}: lock entry is not an object")
        entry_problems = validate_manifest_entry(raw_entry)
        if entry_problems:
            raise ContractError(f"{MANIFEST_PATH}: {'; '.join(entry_problems)}")
        aliases: list[str] = []
        for alias_item in raw_entry.get("install_aliases", []):
            if isinstance(alias_item, dict):
                a_str = alias_item.get("alias")
                if isinstance(a_str, str):
                    aliases.append(a_str)
            elif isinstance(alias_item, str):
                aliases.append(alias_item)
        for target in [raw_entry["output"], *aliases]:
            normalized = _normalize_install_target(target)
            if normalized in install_targets:
                raise ContractError(f"{MANIFEST_PATH}: install target {target!r} is repeated")
            install_targets.add(normalized)
    return data


_TABLE_HEADER = re.compile(r"^\[\[?\s*([^\[\]]+?)\s*\]\]?\s*(?:#.*)?$")
_VERSION_KEY = re.compile(r"^version\s*=")


def fingerprint_content(relative: str, content: bytes) -> bytes:
    """Return a lock input's bytes as the fingerprint sees them (ADR-1344).

    release-please rewrites ``[project].version`` in every released
    ``pyproject.toml``. The project's own version never affects dependency
    resolution, so that single line is left out; every other byte counts.
    """
    if not relative.endswith("pyproject.toml"):
        return content
    kept: list[str] = []
    table = ""
    for line in content.decode("utf-8").splitlines(keepends=True):
        stripped = line.strip()
        header = _TABLE_HEADER.match(stripped)
        if header:
            table = header.group(1)
        elif table == "project" and _VERSION_KEY.match(stripped):
            continue
        kept.append(line)
    return "".join(kept).encode("utf-8")


def entry_digest(root: Path, entry: dict[str, Any]) -> str:
    problems = validate_manifest_entry(entry)
    if problems:
        raise ContractError(f"invalid manifest entry: {'; '.join(problems)}")
    digest = hashlib.sha256()
    compile_args = entry["compile_args"]
    inputs = entry["inputs"]
    digest.update(json.dumps(compile_args, separators=(",", ":")).encode())
    digest.update(b"\0")
    for relative in inputs:
        path = root / relative
        try:
            content = fingerprint_content(relative, path.read_bytes())
        except OSError as error:
            raise ContractError(f"{relative}: {error}") from error
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(content)
        digest.update(b"\0")
    return digest.hexdigest()


def logical_lines(text: str) -> Iterable[tuple[int, str]]:
    """Yield backslash/backtick-continued commands with their first line."""

    pending: list[str] = []
    start = 0
    for number, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if not pending:
            start = number
        continuation = stripped.endswith(("\\", "`"))
        pending.append(stripped[:-1].rstrip() if continuation else stripped)
        if continuation:
            continue
        yield start, " ".join(piece for piece in pending if piece)
        pending = []
    if pending:
        yield start, " ".join(piece for piece in pending if piece)


def _shell_tokens(command: str) -> list[str]:
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        return list(lexer)
    except ValueError:
        return command.split()


def _normalize_pip_tokens(tokens: list[str]) -> list[str]:
    """Normalize short option clusters and joined arguments in pip commands."""
    normalized: list[str] = []
    for token in tokens:
        if token.startswith("-") and not token.startswith("--") and len(token) > 1:
            s = token[1:]
            idx = 0
            while idx < len(s):
                ch = s[idx]
                if ch in SHORT_OPTIONS_WITH_VALUES:
                    val = s[idx + 1 :]
                    normalized.append(f"-{ch}")
                    if val:
                        if val.startswith("="):
                            val = val[1:]
                        if val:
                            normalized.append(val)
                    break
                normalized.append(f"-{ch}")
                idx += 1
        else:
            normalized.append(token)
    return normalized


def _pip_install_tokens(command: str) -> Iterable[list[str]]:
    """Yield normalized ``pip install`` token slices from executable shell text."""

    tokens = _shell_tokens(command)
    for index, token in enumerate(tokens):
        arguments_at: int | None = None
        invoked_by_python = (
            index >= 2  # noqa: PLR2004
            and tokens[index - 1].lower() == "-m"
            and PYTHON_NAME_RE.search(tokens[index - 2]) is not None
        )
        pre_flags: list[str] = []
        if PIP_NAME_RE.search(token) and not invoked_by_python:
            j = index + 1
            while j < len(tokens) and tokens[j] not in {"&", "&&", ";", "|", "||"}:
                if tokens[j].lower() == "install":
                    pre_flags = tokens[index + 1 : j]
                    arguments_at = j + 1
                    break
                j += 1
        elif PYTHON_NAME_RE.search(token) and index + 2 < len(tokens):
            if tokens[index + 1].lower() == "-m" and PIP_NAME_RE.search(tokens[index + 2]):
                j = index + 3
                while j < len(tokens) and tokens[j] not in {"&", "&&", ";", "|", "||"}:
                    if tokens[j].lower() == "install":
                        pre_flags = tokens[index + 3 : j]
                        arguments_at = j + 1
                        break
                    j += 1
        if arguments_at is None:
            continue
        end = arguments_at
        while end < len(tokens) and tokens[end] not in {"&", "&&", ";", "|", "||"}:
            end += 1
        raw_tail = [*pre_flags, *tokens[arguments_at:end]]
        normalized_tail = _normalize_pip_tokens(raw_tail)
        yield ["pip", "install", *normalized_tail]


def _is_container_recipe(relative: Path | str) -> bool:
    rel = Path(relative)
    name = rel.name.lower()
    suffix = rel.suffix.lower()
    if suffix in {".md", ".rst", ".txt", ".json", ".yaml", ".yml", ".toml", ".html", ".xml"}:
        return False
    if name in {"dockerfile", "containerfile"}:
        return True
    if name.startswith(("dockerfile.", "containerfile.")):
        return True
    if suffix in {".dockerfile", ".containerfile"}:
        return True
    return False


def _resolve_search_root(root: Path | None = None) -> Path:
    if root is not None:
        return root
    default_root = Path(__file__).resolve().parents[2]
    if (default_root / MANIFEST_PATH).exists():
        return default_root
    if (Path.cwd() / MANIFEST_PATH).exists():
        return Path.cwd()
    return default_root


def _is_local_source(  # noqa: PLR0911, PLR0912
    value: str,
    root: Path | None = None,
    consumer_path: Path | None = None,
) -> bool:
    if not value or not isinstance(value, str):
        return False

    clean_val = re.sub(r"\[[^\]]*\]$", "", value.strip("\"'"))
    if not clean_val:
        return False

    if "://" in clean_val or clean_val.startswith(
        ("git+", "hg+", "svn+", "bzr+", "git@", "ssh@", "http:", "https:", "ftp:")
    ):
        return False

    if re.search(r"^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+:", clean_val):
        return False

    if clean_val.startswith(("\\\\", "//")):
        return False

    search_root = _resolve_search_root(root)

    norm_val = clean_val.replace("\\", "/")

    if ".." in norm_val.split("/"):
        try:
            base_dir = (search_root / consumer_path).parent if consumer_path else search_root
            resolved = (base_dir / clean_val).resolve()
            search_root_resolved = search_root.resolve()
            if not str(resolved).startswith(str(search_root_resolved)):
                return False
        except (ValueError, OSError):
            return False

    if any(
        norm_val.lower().endswith(ext) for ext in (".whl", ".tar.gz", ".tgz", ".tar.bz2", ".zip")
    ):
        return True

    if norm_val.startswith(SUPPORTED_ABSOLUTE_PREFIXES) or norm_val.startswith(
        ("/opt/", "/wheels/")
    ):
        for pfx in ("/build/vmaf/", "/vmaf/"):
            if norm_val.startswith(pfx):
                sub = norm_val[len(pfx) :]
                if (search_root / sub).exists():
                    return True
        if consumer_path and _is_container_recipe(consumer_path):
            return True
        if norm_val.startswith(("/build/", "/tmp/")):  # noqa: S108
            return True

    try:
        cand_root = search_root / clean_val
        if cand_root.exists():
            return True
    except (ValueError, OSError):
        pass

    if consumer_path:
        try:
            cand_consumer = (search_root / consumer_path).parent / clean_val
            if cand_consumer.exists():
                return True
        except (ValueError, OSError):
            pass

    return False


def _package_arguments(tokens: list[str]) -> list[str]:
    packages: list[str] = []
    skip_value = False
    for token in tokens[2:]:
        lowered = token.lower()
        if skip_value:
            skip_value = False
            continue
        if lowered in OPTIONS_WITH_VALUES:
            skip_value = True
            continue
        if token.startswith("-"):
            continue
        packages.append(token)
    return packages


def _has_insecure_pip_flags(lowered: list[str]) -> bool:
    for token in lowered:
        if token in INSECURE_COMPILE_FLAGS:
            return True
        if token.startswith(INSECURE_COMPILE_PREFIXES):
            return True
    return False


def _has_requirement_or_constraint_flags(lowered: list[str]) -> bool:
    for token in lowered:
        if token in {"-r", "--requirement", "-c", "--constraint"}:
            return True
        if token.startswith(("-r=", "--requirement=", "-c=", "--constraint=")):
            return True
    return False


def _has_constraint_flags(lowered: list[str]) -> bool:
    for token in lowered:
        if token in {"-c", "--constraint"}:
            return True
        if token.startswith(("-c=", "--constraint=")):
            return True
    return False


def _has_editable_flags(lowered: list[str]) -> bool:
    for token in lowered:
        if token in {"-e", "--editable"}:
            return True
        if token.startswith(("-e=", "--editable=")):
            return True
    return False


def get_manifest_lock_targets(root: Path | None = None) -> set[str]:
    search_root = root
    if search_root is None:
        default_root = Path(__file__).resolve().parents[2]
        if (default_root / MANIFEST_PATH).exists():
            search_root = default_root
        elif (Path.cwd() / MANIFEST_PATH).exists():
            search_root = Path.cwd()
        else:
            search_root = default_root
    try:
        manifest = load_manifest(search_root)
    except ContractError:
        return set()
    targets: set[str] = set()
    for entry in manifest["locks"]:
        targets.add(entry["output"])
        for alias_item in entry.get("install_aliases", []):
            if isinstance(alias_item, dict):
                a_str = alias_item.get("alias")
                if isinstance(a_str, str):
                    targets.add(a_str)
            elif isinstance(alias_item, str):
                targets.add(alias_item)
    return targets


def _matches_consumer(bound_consumers: list[str], consumer_str: str) -> bool:
    normalized_consumer = _normalize_install_target(consumer_str)
    return any(
        _normalize_install_target(bound_consumer) == normalized_consumer
        for bound_consumer in bound_consumers
    )


def get_manifest_lock_targets_for_consumer(
    consumer: Path | str,
    manifest: dict[str, Any] | None = None,
    root: Path | None = None,
) -> set[str]:
    """Return all valid lock file targets (outputs and bound aliases) for a consumer."""
    search_root = root
    if manifest is None:
        if search_root is None:
            default_root = Path(__file__).resolve().parents[2]
            if (default_root / MANIFEST_PATH).exists():
                search_root = default_root
            elif (Path.cwd() / MANIFEST_PATH).exists():
                search_root = Path.cwd()
            else:
                search_root = default_root
        try:
            manifest = load_manifest(search_root)
        except ContractError:
            return set()
    targets: set[str] = set()
    consumer_str = _normalize_install_target(str(consumer))
    for entry in manifest["locks"]:
        targets.add(entry["output"])
        for alias_spec in entry.get("install_aliases", []):
            if isinstance(alias_spec, dict):
                alias_name = alias_spec.get("alias")
                if isinstance(alias_name, str) and _matches_consumer(
                    _extract_bound_consumers(alias_spec), consumer_str
                ):
                    targets.add(alias_name)
            elif isinstance(alias_spec, str):
                targets.add(alias_spec)
    return targets


def _is_valid_lock_target(target: str, valid_lock_outputs: set[str] | None = None) -> bool:
    if "://" in target or target.startswith(("git+", "hg+", "svn+", "bzr+")):
        return False
    t = target.strip("\"'")
    targets = valid_lock_outputs if valid_lock_outputs is not None else get_manifest_lock_targets()
    return _normalize_install_target(t) in {_normalize_install_target(item) for item in targets}


def _is_secure_hash_install(
    tokens: list[str], lowered: list[str], valid_lock_outputs: set[str] | None = None
) -> bool:
    if "--require-hashes" not in lowered:
        return False
    if _package_arguments(tokens):
        return False
    if (
        _has_editable_flags(lowered)
        or _has_insecure_pip_flags(lowered)
        or _has_constraint_flags(lowered)
    ):
        return False
    req_targets: list[str] = []
    for index, token in enumerate(tokens):
        t_low = token.lower()
        if t_low in {"-r", "--requirement"} and index + 1 < len(tokens):
            req_targets.append(tokens[index + 1])
        elif t_low.startswith(("-r=", "--requirement=")):
            req_targets.append(token.split("=", 1)[1])
    if not req_targets:
        return False
    return all(_is_valid_lock_target(t, valid_lock_outputs) for t in req_targets)


def _extract_editable_targets(tokens: list[str]) -> list[str]:
    targets: list[str] = []
    skip_next = False
    for idx, token in enumerate(tokens[2:], 2):
        if skip_next:
            skip_next = False
            continue
        lowered = token.lower()
        if lowered in {"-e", "--editable"}:
            if idx + 1 < len(tokens) and not tokens[idx + 1].startswith("-"):
                targets.append(tokens[idx + 1])
                skip_next = True
            else:
                targets.append("")
        elif lowered.startswith(("-e=", "--editable=")):
            targets.append(token.split("=", 1)[1])
    return targets


def _extract_editable_target(tokens: list[str], lowered: list[str] | None = None) -> str | None:
    targets = _extract_editable_targets(tokens)
    return targets[0] if targets else None


def _is_secure_install(  # noqa: PLR0911
    tokens: list[str],
    valid_lock_outputs: set[str] | None = None,
    root: Path | None = None,
    consumer_path: Path | None = None,
) -> bool:
    lowered = [token.lower() for token in tokens]
    if _is_secure_hash_install(tokens, lowered, valid_lock_outputs):
        return True

    if _has_insecure_pip_flags(lowered) or _has_requirement_or_constraint_flags(lowered):
        return False

    no_deps = "--no-deps" in lowered
    no_build_isolation = "--no-build-isolation" in lowered

    editable_targets = _extract_editable_targets(tokens)
    if editable_targets:
        if (
            not no_deps
            or not no_build_isolation
            or _package_arguments(tokens)
            or any(t == "" for t in editable_targets)
        ):
            return False
        return all(
            _is_local_source(t, root=root, consumer_path=consumer_path) for t in editable_targets
        )

    if _has_editable_flags(lowered):
        return False

    package_tokens = _package_arguments(tokens)
    if not package_tokens or not no_deps:
        return False

    is_whl = all(
        t.lower().endswith(".whl") and _is_local_source(t, root=root, consumer_path=consumer_path)
        for t in package_tokens
    )
    return is_whl or (
        no_build_isolation
        and all(_is_local_source(t, root=root, consumer_path=consumer_path) for t in package_tokens)
    )


def _extract_constant_strings(args: list[ast.expr]) -> list[str] | None:
    values: list[str] = []
    for arg in args:
        if not isinstance(arg, ast.Constant) or not isinstance(arg.value, str):
            return None
        values.append(arg.value)
    return values


def _parse_shell_runner_call(node: ast.Call) -> tuple[bool, list[str] | None]:
    c_index: int | None = None
    for idx, arg in enumerate(node.args[1:], 1):
        if isinstance(arg, ast.Constant) and arg.value == "-c":
            c_index = idx
            break
    if c_index is None:
        return False, None
    if c_index + 1 >= len(node.args):
        return True, None
    cmd_node = node.args[c_index + 1]
    if isinstance(cmd_node, ast.Constant) and isinstance(cmd_node.value, str):
        pip_tokens = list(_pip_install_tokens(cmd_node.value))
        if pip_tokens:
            return True, pip_tokens[0]
        return "pip" in cmd_node.value, None
    return True, None


def _parse_direct_pip_run_call(node: ast.Call, first: str) -> tuple[bool, list[str] | None]:
    cmd_args: list[ast.expr] | None = None
    if PIP_NAME_RE.search(first):
        cmd_args = node.args[1:]
    elif (
        PYTHON_NAME_RE.search(first)
        and len(node.args) >= 3  # noqa: PLR2004
        and isinstance(node.args[1], ast.Constant)
        and node.args[1].value == "-m"
        and isinstance(node.args[2], ast.Constant)
        and isinstance(node.args[2].value, str)
        and PIP_NAME_RE.search(node.args[2].value)
    ):
        cmd_args = node.args[3:]
    if cmd_args is None:
        return False, None
    if node.keywords:
        return True, None
    if (
        cmd_args
        and isinstance(cmd_args[0], ast.Constant)
        and isinstance(cmd_args[0].value, str)
        and cmd_args[0].value.lower() == "install"
    ):
        tail = _extract_constant_strings(cmd_args[1:])
        return True, None if tail is None else ["pip", "install", *_normalize_pip_tokens(tail)]
    return True, None


def _parse_nox_run_call(node: ast.Call) -> tuple[bool, list[str] | None]:
    if not node.args:
        return False, None
    first_arg = node.args[0]
    if not isinstance(first_arg, ast.Constant) or not isinstance(first_arg.value, str):
        return False, None
    first = first_arg.value
    first_lower = first.lower()
    first_base = first_lower.split("/")[-1].split("\\")[-1]

    if first_lower in SHELL_RUNNERS or first_base in {"sh", "bash", "zsh"}:
        return _parse_shell_runner_call(node)
    return _parse_direct_pip_run_call(node, first)


def _nox_assignment_parts(
    sub: ast.Assign | ast.AnnAssign,
) -> tuple[list[ast.expr], ast.expr | None]:
    if isinstance(sub, ast.Assign):
        return sub.targets, sub.value
    return [sub.target], sub.value


def _nox_method_reference(value: ast.expr, session_aliases: set[str]) -> str | None:
    if (
        isinstance(value, ast.Attribute)
        and isinstance(value.value, ast.Name)
        and value.value.id in session_aliases
        and value.attr in {"install", "run", "run_always"}
    ):
        return value.attr
    if not (
        isinstance(value, ast.Call)
        and isinstance(value.func, ast.Name)
        and value.func.id == "getattr"
        and len(value.args) >= 2  # noqa: PLR2004
    ):
        return None
    receiver, attr_node = value.args[:2]
    if not (
        isinstance(receiver, ast.Name)
        and receiver.id in session_aliases
        and isinstance(attr_node, ast.Constant)
        and isinstance(attr_node.value, str)
        and attr_node.value in {"install", "run", "run_always"}
    ):
        return None
    return attr_node.value


def _scan_nox_assign(
    sub: ast.Assign | ast.AnnAssign,
    session_aliases: set[str],
    method_aliases: dict[str, str],
) -> Iterable[tuple[int, list[str] | None]]:
    targets, value = _nox_assignment_parts(sub)
    if value is None:
        return
    if isinstance(value, ast.Name) and value.id in session_aliases:
        for target in targets:
            if isinstance(target, ast.Name):
                session_aliases.add(target.id)
                yield sub.lineno, None
        return
    method = _nox_method_reference(value, session_aliases)
    if method is not None:
        for target in targets:
            if isinstance(target, ast.Name):
                method_aliases[target.id] = method
                yield sub.lineno, None


def _scan_nox_getattr_call(
    sub: ast.Call, session_aliases: set[str]
) -> Iterable[tuple[int, list[str] | None]]:
    if not (
        isinstance(sub.func, ast.Call)
        and isinstance(sub.func.func, ast.Name)
        and sub.func.func.id == "getattr"
        and len(sub.func.args) >= 2  # noqa: PLR2004
    ):
        return
    rec = sub.func.args[0]
    attr_node = sub.func.args[1]
    if (
        isinstance(rec, ast.Name)
        and rec.id in session_aliases
        and isinstance(attr_node, ast.Constant)
        and isinstance(attr_node.value, str)
    ):
        if attr_node.value == "install":
            if sub.keywords:
                yield sub.lineno, None
            else:
                args = _extract_constant_strings(sub.args)
                yield (
                    sub.lineno,
                    None if args is None else ["pip", "install", *_normalize_pip_tokens(args)],
                )
        elif attr_node.value in {"run", "run_always"}:
            is_pip, tokens = _parse_nox_run_call(sub)
            if is_pip:
                yield sub.lineno, tokens


def _scan_nox_call(
    sub: ast.Call, session_aliases: set[str], method_aliases: dict[str, str]
) -> Iterable[tuple[int, list[str] | None]]:
    if isinstance(sub.func, ast.Name) and sub.func.id in method_aliases:
        attr = method_aliases[sub.func.id]
        if attr == "install":
            args = _extract_constant_strings(sub.args)
            yield (
                sub.lineno,
                (
                    None
                    if (sub.keywords or args is None)
                    else ["pip", "install", *_normalize_pip_tokens(args)]
                ),
            )
        else:
            yield sub.lineno, None
        return

    yield from _scan_nox_getattr_call(sub, session_aliases)

    if not isinstance(sub.func, ast.Attribute):
        return
    rec = sub.func.value
    if not (isinstance(rec, ast.Name) and rec.id in session_aliases):
        return

    attr = sub.func.attr
    if attr == "install":
        args = _extract_constant_strings(sub.args)
        yield (
            sub.lineno,
            (
                None
                if (sub.keywords or args is None)
                else ["pip", "install", *_normalize_pip_tokens(args)]
            ),
        )
    elif attr in {"run", "run_always"}:
        is_pip, tokens = _parse_nox_run_call(sub)
        if is_pip:
            yield sub.lineno, tokens


def _nox_install_tokens(text: str) -> Iterable[tuple[int, list[str] | None]]:
    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        yield error.lineno or 1, None
        return

    for node in tree.body:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        session_param: str | None = None
        if node.args.args:
            session_param = node.args.args[0].arg
        elif node.args.posonlyargs:
            session_param = node.args.posonlyargs[0].arg

        if not session_param:
            continue

        session_aliases: set[str] = {session_param}
        method_aliases: dict[str, str] = {}

        for sub in ast.walk(node):
            if isinstance(sub, (ast.Assign, ast.AnnAssign)):
                yield from _scan_nox_assign(sub, session_aliases, method_aliases)
            elif isinstance(sub, ast.Call):
                yield from _scan_nox_call(sub, session_aliases, method_aliases)


def scan_install_commands(
    path: Path,
    text: str,
    valid_lock_outputs: set[str] | None = None,
    root: Path | None = None,
) -> list[str]:
    """Return unsafe executable pip-install commands in one supported surface."""
    search_root = root
    if search_root is None:
        default_root = Path(__file__).resolve().parents[2]
        if (default_root / MANIFEST_PATH).exists():
            search_root = default_root
        elif (Path.cwd() / MANIFEST_PATH).exists():
            search_root = Path.cwd()
        else:
            search_root = default_root

    if valid_lock_outputs is None:
        valid_lock_outputs = get_manifest_lock_targets_for_consumer(path, root=search_root)

    findings: list[str] = []
    if path.name.lower() == "noxfile.py":
        for line, tokens in _nox_install_tokens(text):
            if tokens is not None and _is_secure_install(
                tokens, valid_lock_outputs, root=search_root, consumer_path=path
            ):
                continue
            findings.append(
                f"{path}:{line}: nox session.install must use literal arguments and the "
                "hash-locked or local-source install policy"
            )
        return findings
    for line, command in logical_lines(text):
        stripped = command.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        for tokens in _pip_install_tokens(command):
            if _is_secure_install(tokens, valid_lock_outputs, root=search_root, consumer_path=path):
                continue
            findings.append(
                f"{path}:{line}: pip install must use --require-hashes with a lock file, "
                "or install a local wheel with --no-deps, "
                "or install a local editable/source tree with both --no-deps and --no-build-isolation"
            )
    return findings


def _is_repo_local_target(target: str, root: Path | None = None) -> bool:
    clean = target.strip("\"'`$(){} \t\r\n")
    if not clean or clean.startswith(("-", "http:", "https:", "git@", "ssh:")):
        return False
    if clean == "." or clean.startswith(("./", "../")):
        return True
    if any(clean == p.rstrip("/") or clean.startswith(p) for p in REPO_LOCAL_WORKFLOW_PREFIXES):
        return True
    search_root = _resolve_search_root(root)
    try:
        return (search_root / clean).exists()
    except (ValueError, OSError):
        return False


def _extract_flag_value(
    token: str, next_token: str | None, flags: set[str], prefixes: tuple[str, ...]
) -> str | None:
    if token in flags and next_token is not None:
        return next_token
    if token.startswith(prefixes):
        return token.split("=", 1)[1]
    for flag in flags:
        if (
            flag.startswith("-")
            and not flag.startswith("--")
            and token.startswith(flag)
            and len(token) > len(flag)
        ):
            val = token[len(flag) :]
            return val.split("=", 1)[1] if val.startswith("=") else val
    return None


def _check_precheckout_token(
    token: str, next_token: str | None, root: Path | None = None
) -> list[str]:
    req = _extract_flag_value(token, next_token, WORKFLOW_REQ_FLAGS, WORKFLOW_REQ_PREFIXES)
    if req and _is_repo_local_target(req, root):
        return [f"consumes repo-local requirements file {req!r}"]
    edit = _extract_flag_value(token, next_token, WORKFLOW_EDIT_FLAGS, WORKFLOW_EDIT_PREFIXES)
    if edit and _is_repo_local_target(edit, root):
        return [f"consumes repo-local editable target {edit!r}"]
    clean = token.strip("\"'`();, \t")
    if clean.startswith(("scripts/", "./scripts/", "testdata/")):
        return [f"consumes repo-local helper file {clean!r}"]
    if clean.endswith((".sh", ".py", ".bash")) and _is_repo_local_target(clean, root):
        return [f"consumes repo-local helper file {clean!r}"]
    return []


def _check_precheckout_command_tokens(tokens: list[str], root: Path | None = None) -> list[str]:
    findings: list[str] = []
    norm = _normalize_pip_tokens(tokens)
    for idx, token in enumerate(norm):
        next_tok = norm[idx + 1] if idx + 1 < len(norm) else None
        findings.extend(_check_precheckout_token(token, next_tok, root))
    return findings


CHECKOUT_USES_RE = re.compile(r"^actions/checkout@[0-9a-fA-F]{40}$")
ACCEPTED_LOCAL_REPOSITORIES = {
    "${{ github.repository }}",
    "${{github.repository}}",
    "vmafx/vmafx",
}


def _is_valid_checkout_uses(uses_val: Any) -> bool:
    if not isinstance(uses_val, str):
        return False
    clean = uses_val.split("#", 1)[0].strip().strip("\"'")
    return bool(CHECKOUT_USES_RE.fullmatch(clean))


def _is_valid_checkout_repo(repo_val: Any) -> bool:
    if repo_val is None:
        return True
    clean = str(repo_val).strip().strip("\"'")
    if not clean:
        return True
    return clean.lower() in ACCEPTED_LOCAL_REPOSITORIES


def _is_root_checkout_path(path_val: Any) -> bool:
    if path_val is None:
        return True
    clean = str(path_val).strip().strip("\"'").rstrip("/")
    return clean in {"", "."}


def _is_truthy(val: Any) -> bool:
    if isinstance(val, bool):
        return val
    if isinstance(val, (int, float)):
        return val != 0
    if isinstance(val, str):
        return val.strip().lower() not in {"false", "0", "no", ""}
    return bool(val)


def _is_checkout_step(step: dict[str, Any]) -> bool:
    if not _is_valid_checkout_uses(step.get("uses")):
        return False
    if "_vmafx_fallback_inline_with" in step:
        raise ContractError("fallback parser: unsupported flow-style checkout with mapping")
    if step.get("if") is not None and bool(str(step.get("if")).strip()):
        return False
    if _is_truthy(step.get("continue-on-error")):
        return False
    with_dict = step.get("with")
    if isinstance(with_dict, dict):
        if not _is_valid_checkout_repo(with_dict.get("repository")):
            return False
        if not _is_root_checkout_path(with_dict.get("path")):
            return False
    return True


def _check_precheckout_pip_tokens(pip_tokens: list[str], root: Path | None = None) -> list[str]:
    findings: list[str] = []
    for edit in _extract_editable_targets(pip_tokens):
        if edit and _is_repo_local_target(edit, root):
            findings.append(f"consumes repo-local editable target {edit!r}")
    for pkg in _package_arguments(pip_tokens):
        if _is_repo_local_target(pkg, root):
            findings.append(f"consumes repo-local source target {pkg!r}")
    return findings


def _check_precheckout_pip_invocations(command: str, root: Path | None = None) -> list[str]:
    findings: list[str] = []
    for pip_tokens in _pip_install_tokens(command):
        findings.extend(_check_precheckout_pip_tokens(pip_tokens, root))
    return findings


def _step_consumes_repo_resources(step: dict[str, Any], root: Path | None = None) -> list[str]:
    findings: list[str] = []
    uses = str(step.get("uses", "")).strip()
    if uses.startswith(("./", ".\\")):
        findings.append(f"consumes local action {uses!r}")
    run = step.get("run")
    if isinstance(run, str):
        for _line_num, command in logical_lines(run):
            tokens = _shell_tokens(command)
            findings.extend(_check_precheckout_command_tokens(tokens, root))
            findings.extend(_check_precheckout_pip_invocations(command, root))
    return list(dict.fromkeys(findings))


def _parse_step_scalar(prop: str, val: str, step: dict[str, Any]) -> None:
    raw = val.split("#", 1)[0].strip()
    if raw.startswith(("[", "{", "*", "&", "!")):
        raise ContractError(f"fallback parser: unsupported flow-style {prop} value")
    clean = raw.strip("\"'")
    if prop in {"uses", "if", "continue-on-error"}:
        step[prop] = clean


def _start_step_block(prop: str, val: str, indent: int, step: dict[str, Any]) -> tuple[str, int]:
    clean = val.split("#", 1)[0].strip()
    if prop == "with":
        if clean:
            step["_vmafx_fallback_inline_with"] = clean
            return "", 0
        step.setdefault("with", {})
        return "with", indent
    if prop == "run":
        if clean.startswith(("[", "{", "*", "&", "!")):
            raise ContractError("fallback parser: unsupported flow-style run value")
        if clean.startswith(">"):
            step["run"] = ""
            return "run_folded", indent
        if clean.startswith("|"):
            step["run"] = ""
            return "run_literal", indent
        step["run"] = clean.strip("\"'")
        return "run_folded", indent
    return "", 0


def _parse_step_entry(
    raw_line: str,
    indent: int,
    step: dict[str, Any],
) -> tuple[str, int]:
    entry = _simple_yaml_mapping_entry(raw_line)
    if entry is None:
        return "", 0
    prop, val = entry
    if prop in {"uses", "if", "continue-on-error"}:
        _parse_step_scalar(prop, val, step)
        return "", 0
    if prop in {"with", "run"}:
        return _start_step_block(prop, val, indent, step)
    return "", 0


def _append_run_content(content: str, folded: bool, step: dict[str, Any]) -> None:
    if not content:
        step["run"] = step.get("run", "") + "\n"
        return
    cur = step.get("run", "")
    sep = " " if folded and cur and not cur.endswith("\n") else "\n"
    step["run"] = f"{cur}{sep}{content}" if cur else content


def _append_step_block_line(line: str, mode: str, step: dict[str, Any]) -> None:
    content = line.strip()
    if mode == "with":
        entry = _simple_yaml_mapping_entry(content)
        if entry is not None:
            key, raw_value = entry
            val = raw_value.split("#", 1)[0].strip().strip("\"'")
            step.setdefault("with", {})[key] = val
    elif mode in {"run_folded", "run_literal"}:
        _append_run_content(content, mode == "run_folded", step)


def _start_fallback_step(
    raw_step: str, spaces: int, job_steps: list[dict[str, Any]]
) -> tuple[dict[str, Any], str, int]:
    step: dict[str, Any] = {}
    job_steps.append(step)
    rest = raw_step.strip()
    if rest.startswith(("[", "{", "*", "&", "!", "<<:")):
        raise ContractError("fallback parser: unsupported flow-style step mapping")
    if rest and _simple_yaml_mapping_entry(rest) is None:
        raise ContractError("fallback parser: unsupported workflow step structure")
    mode, indent = _parse_step_entry(rest, spaces, step) if rest else ("", 0)
    return step, mode, indent


def _handle_fallback_step_line(
    line: str,
    cur_job: str | None,
    jobs: dict[str, list[dict[str, Any]]],
    cur_step: dict[str, Any] | None,
    mode: str,
    indent: int,
) -> tuple[dict[str, Any] | None, str, int]:
    spaces = len(line) - len(line.lstrip(" "))
    if mode and cur_step is not None:
        if not line.strip() or spaces > indent:
            _append_step_block_line(line, mode, cur_step)
            return cur_step, mode, indent
        mode, indent = "", 0
    m_step = re.match(r"^\s*-\s*(.*)$", line)
    if m_step and cur_job is not None:
        return _start_fallback_step(m_step.group(1), spaces, jobs[cur_job])
    if cur_step is not None:
        new_mode, new_indent = _parse_step_entry(line, spaces, cur_step)
        return cur_step, new_mode, new_indent
    return None, "", 0


def _simple_yaml_mapping_entry(line: str) -> tuple[str, str] | None:
    match = SIMPLE_YAML_MAPPING_RE.match(line.strip())
    if match is None:
        return None
    key = next(group for group in match.groups()[:3] if group is not None)
    return key, match.group(4)


def _parse_workflow_jobs_fallback(text: str) -> dict[str, list[dict[str, Any]]]:
    jobs: dict[str, list[dict[str, Any]]] = {}
    cur_job: str | None = None
    in_jobs = False
    in_steps = False
    cur_step: dict[str, Any] | None = None
    mode, indent = "", 0
    for line in text.splitlines():
        spaces = len(line) - len(line.lstrip(" "))
        mapping = _simple_yaml_mapping_entry(line)
        if spaces == 0 and mapping is not None and mapping[0] == "jobs":
            jobs_value = mapping[1].split("#", 1)[0].strip()
            if jobs_value:
                raise ContractError("fallback parser: unsupported flow-style jobs mapping")
            in_jobs = True
            continue
        if in_jobs and line and not line.startswith((" ", "#")):
            in_jobs = False
        if not in_jobs:
            continue
        job_mapping = mapping if spaces == WORKFLOW_JOB_INDENT else None
        if job_mapping and not job_mapping[1].split("#", 1)[0].strip():
            cur_job = job_mapping[0]
            jobs[cur_job] = []
            in_steps, cur_step, mode, indent = False, None, "", 0
        elif job_mapping:
            raise ContractError("fallback parser: unsupported inline job mapping")
        elif (
            cur_job is not None
            and spaces == WORKFLOW_STEPS_INDENT
            and mapping is not None
            and mapping[0] == "steps"
        ):
            if mapping[1].split("#", 1)[0].strip():
                raise ContractError("fallback parser: unsupported flow-style steps sequence")
            in_steps = True
        elif in_steps and re.match(r"^    \S", line):
            in_steps, cur_step, mode, indent = False, None, "", 0
        elif in_steps:
            cur_step, mode, indent = _handle_fallback_step_line(
                line, cur_job, jobs, cur_step, mode, indent
            )
    if not jobs:
        raise ContractError("fallback parser: workflow jobs mapping missing or unsupported")
    return jobs


def _load_workflow_jobs(text: str) -> dict[str, Any]:
    if yaml is not None:
        try:
            data = yaml.safe_load(text)
        except Exception as error:
            raise ContractError(f"malformed workflow YAML: {error}") from error
        if data is None:
            return {}
        if not isinstance(data, dict) or not isinstance(data.get("jobs"), dict):
            raise ContractError("malformed workflow YAML: root or jobs mapping missing")
        return cast(dict[str, Any], data["jobs"])
    return _parse_workflow_jobs_fallback(text)


def _scan_job_checkout_ordering(path: Path, job_id: str, raw_steps: Any, root: Path) -> list[str]:
    if not isinstance(raw_steps, list):
        return []
    findings: list[str] = []
    checked_out = False
    for step in raw_steps:
        if not isinstance(step, dict):
            continue
        if _is_checkout_step(step):
            checked_out = True
        elif not checked_out:
            for reason in _step_consumes_repo_resources(step, root):
                findings.append(f"{path}: job {job_id!r} {reason} before actions/checkout")
    return findings


def scan_workflow_checkout_ordering(path: Path, text: str, root: Path | None = None) -> list[str]:
    search_root = _resolve_search_root(root)
    jobs = _load_workflow_jobs(text)
    findings: list[str] = []
    for job_id, job_data in jobs.items():
        raw_steps = job_data.get("steps") if isinstance(job_data, dict) else job_data
        findings.extend(_scan_job_checkout_ordering(path, job_id, raw_steps, search_root))
    return findings


def tracked_consumer_paths(root: Path) -> list[Path]:
    git_dir = root / ".git"
    raw_paths: list[Path] = []
    if git_dir.exists():
        git_bin = shutil.which("git") or "git"
        try:
            result = subprocess.run(  # noqa: S603
                [git_bin, "-C", str(root), "ls-files", "-z"],
                check=True,
                capture_output=True,
            )
            for raw in result.stdout.split(b"\0"):
                if raw:
                    raw_paths.append(Path(os.fsdecode(raw)))
        except (subprocess.CalledProcessError, OSError) as error:
            raise ContractError(f"git ls-files failed in {root}: {error}") from error
    else:
        for path in root.rglob("*"):
            try:
                rel = path.relative_to(root)
            except ValueError:
                continue
            if any(
                part.startswith(".") and part not in {".github", ".devcontainer"}
                for part in rel.parts
            ):
                continue
            if path.is_file():
                raw_paths.append(rel)

    paths = []
    for relative in raw_paths:
        parts = relative.parts
        name = relative.name.lower()
        supported = (
            relative.suffix.lower() in {".sh", ".ps1"}
            or (
                parts[:2] == (".github", "workflows")
                and relative.suffix.lower() in {".yml", ".yaml"}
            )
            or _is_container_recipe(relative)
            or name in {"makefile", "gnumakefile", "noxfile.py"}
        )
        fixture = "tests" in parts or name.startswith(("test-", "test_"))
        if supported and not fixture:
            paths.append(relative)
    return sorted(paths)


def read_lock_metadata(text: str) -> dict[str, str]:
    metadata: dict[str, str] = {}
    for line in text.splitlines()[:8]:
        match = re.fullmatch(r"# vmafx-([a-z0-9-]+): (.+)", line)
        if match:
            metadata[match.group(1)] = match.group(2)
    return metadata


def validate_lock(root: Path, manifest: dict[str, Any], entry: dict[str, Any]) -> list[str]:
    entry_problems = validate_manifest_entry(entry)
    if entry_problems:
        return entry_problems
    problems: list[str] = []
    output = entry["output"]
    path = root / output
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        return [f"{output}: {error}"]
    metadata = read_lock_metadata(text)
    expected = entry_digest(root, entry)
    if metadata.get("input-sha256") != expected:
        problems.append(f"{output}: source fingerprint is stale; run make python-locks-write")
    if metadata.get("uv-version") != manifest.get("uv_version"):
        problems.append(f"{output}: generator version does not match manifest")

    requirements = 0
    for line, logical in logical_lines(text):
        stripped = logical.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith(("-", "--")):
            problems.append(
                f"{output}:{line}: lock file cannot contain unpinned directive {stripped.split()[0]!r}"
            )
            continue
        requirements += 1
        if not EXACT_REQUIREMENT_RE.match(stripped):
            problems.append(f"{output}:{line}: dependency is not exactly version-pinned")
        if not HASH_RE.search(stripped):
            problems.append(f"{output}:{line}: dependency has no sha256 artifact hash")
    if requirements == 0:
        problems.append(f"{output}: lock contains no requirements")
    return problems


def find_lock_files(root: Path) -> list[Path]:
    locks: list[Path] = []
    git_dir = root / ".git"
    if git_dir.exists():
        git_bin = shutil.which("git") or "git"
        try:
            result = subprocess.run(  # noqa: S603
                [
                    git_bin,
                    "-C",
                    str(root),
                    "ls-files",
                    "--cached",
                    "--others",
                    "--exclude-standard",
                    "-z",
                ],
                check=True,
                capture_output=True,
            )
            for raw in result.stdout.split(b"\0"):
                if not raw:
                    continue
                rel = Path(os.fsdecode(raw))
                if rel.name.endswith("-lock.txt") or (
                    rel.parent == Path("requirements/locks") and rel.suffix == ".txt"
                ):
                    locks.append(rel)
            return sorted(locks)
        except (subprocess.CalledProcessError, OSError) as error:
            raise ContractError(f"git ls-files failed in {root}: {error}") from error

    for path in root.rglob("*"):
        try:
            rel = path.relative_to(root)
        except ValueError:
            continue
        if any(part.startswith(".") for part in rel.parts):
            continue
        if path.is_file() and (
            rel.name.endswith("-lock.txt")
            or (rel.parent == Path("requirements/locks") and rel.suffix == ".txt")
        ):
            locks.append(rel)
    return sorted(locks)


def _check_alias_liveness(root: Path, entries: list[dict[str, Any]]) -> list[str]:
    problems: list[str] = []
    for entry in entries:
        for alias_item in entry.get("install_aliases", []):
            if not isinstance(alias_item, dict):
                continue
            alias_str = alias_item.get("alias")
            if not isinstance(alias_str, str):
                continue
            consumers = _extract_bound_consumers(alias_item)
            for consumer in consumers:
                consumer_path = root / consumer
                if not consumer_path.is_file():
                    problems.append(
                        f"{entry['output']}: install alias {alias_str!r} declared consumer {consumer!r} does not exist"
                    )
                else:
                    try:
                        content = consumer_path.read_text(encoding="utf-8")
                        if alias_str not in content:
                            problems.append(
                                f"{entry['output']}: install alias {alias_str!r} is not used in declared consumer {consumer!r}"
                            )
                    except (OSError, UnicodeDecodeError) as error:
                        problems.append(f"{consumer}: {error}")
    return problems


def check(root: Path) -> int:
    try:
        manifest = load_manifest(root)
        entries = manifest["locks"]
        problems = [
            problem for entry in entries for problem in validate_lock(root, manifest, entry)
        ]
        problems.extend(_check_alias_liveness(root, entries))
        outputs_paths = {str(Path(entry["output"])) for entry in entries}

        for lock_file in find_lock_files(root):
            if str(lock_file) not in outputs_paths:
                problems.append(f"unregistered lock file {lock_file} not present in manifest")

        for relative in tracked_consumer_paths(root):
            try:
                text = (root / relative).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError) as error:
                problems.append(f"{relative}: {error}")
                continue
            consumer_targets = get_manifest_lock_targets_for_consumer(relative, manifest, root)
            problems.extend(scan_install_commands(relative, text, consumer_targets, root=root))
            if relative.parts[:2] == (".github", "workflows") and relative.suffix.lower() in {
                ".yml",
                ".yaml",
            }:
                problems.extend(scan_workflow_checkout_ordering(relative, text, root=root))
    except (ContractError, subprocess.CalledProcessError) as error:
        problems = [str(error)]

    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"python dependency locks: OK ({len(manifest['locks'])} locks, hash-only installs)")
    return 0


def uv_version(binary: str) -> str:
    result = subprocess.run(  # noqa: S603
        [binary, "--version"], check=True, text=True, capture_output=True
    )
    match = re.fullmatch(r"uv ([0-9]+(?:\.[0-9]+){2})(?: .*)?\n?", result.stdout)
    if not match:
        raise ContractError(f"cannot parse {binary} --version output: {result.stdout!r}")
    return match.group(1)


def stamp_lock(
    root: Path, manifest: dict[str, Any], entry: dict[str, Any], generated: Path
) -> None:
    output = entry["output"]
    lines = generated.read_text(encoding="utf-8").splitlines()
    while lines and lines[0].startswith("#"):
        lines.pop(0)
    body = "\n".join(lines).lstrip("\n")
    header = "\n".join(
        [
            LOCK_HEADER,
            f"# vmafx-uv-version: {manifest['uv_version']}",
            f"# vmafx-input-sha256: {entry_digest(root, entry)}",
            f"# vmafx-inputs: {', '.join(entry['inputs'])}",
            "",
        ]
    )
    destination = root / output
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_text(header + body + "\n", encoding="utf-8")
    temporary.replace(destination)


def write(root: Path, uv_binary: str) -> int:
    manifest = load_manifest(root)
    actual = uv_version(uv_binary)
    if actual != manifest["uv_version"]:
        raise ContractError(
            f"uv {actual} does not match reviewed generator {manifest['uv_version']}"
        )
    with tempfile.TemporaryDirectory(prefix="vmafx-python-lock-") as temporary:
        temp_root = Path(temporary)
        for index, raw_entry in enumerate(manifest["locks"]):
            if not isinstance(raw_entry, dict):
                raise ContractError("manifest lock entry is not an object")
            generated = temp_root / f"{index}.txt"
            command = [
                uv_binary,
                "pip",
                "compile",
                *raw_entry["compile_args"],
                "-o",
                str(generated),
            ]
            result = subprocess.run(  # noqa: S603
                command, cwd=root, text=True, capture_output=True, check=False
            )
            if result.returncode != 0:
                detail = result.stderr.strip() or result.stdout.strip()
                raise ContractError(f"uv failed for {raw_entry['output']}: {detail}")
            stamp_lock(root, manifest, raw_entry, generated)
            print(f"wrote {raw_entry['output']}")
    return check(root)


def find_default_uv(root: Path) -> str:
    env_bin = os.environ.get("VMAFX_UV") or os.environ.get("UV_BINARY")
    if env_bin:
        return env_bin
    on_path = shutil.which("uv")
    if on_path:
        return on_path
    local_cached = root / ".workingdir/cache/scorecard-pins/uv-venv/bin/uv"
    if local_cached.is_file() and os.access(local_cached, os.X_OK):
        return str(local_cached)
    return "uv"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("check")
    writer = subparsers.add_parser("write")
    writer.add_argument("--uv", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    try:
        if args.command == "write":
            uv_bin = args.uv or find_default_uv(root)
            return write(root, uv_bin)
        return check(root)
    except (ContractError, OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

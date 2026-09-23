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
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any

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
    "-c",
    "--cache-dir",
    "--cert",
    "--client-cert",
    "--config-settings",
    "--constraint",
    "--extra-index-url",
    "--find-links",
    "--index-url",
    "--prefix",
    "--proxy",
    "-r",
    "--requirement",
    "--retries",
    "--root",
    "--src",
    "--target",
    "--timeout",
    "--trusted-host",
}


class ContractError(RuntimeError):
    """A lock or install surface violates the repository contract."""


INSECURE_COMPILE_FLAGS = {
    "-i",
    "--index-url",
    "--extra-index-url",
    "-f",
    "--find-links",
    "--trusted-host",
    "--no-index",
    "--default-index",
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


def _validate_manifest_output(output: Any) -> list[str]:
    if not isinstance(output, str) or not output:
        return ["manifest lock entry output must be a non-empty string"]
    if (
        "://" in output
        or Path(output).is_absolute()
        or ".." in Path(output).parts
        or output.startswith(("/", "\\"))
    ):
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
            or inp.startswith(("git+", "hg+", "svn+", "bzr+", "/", "\\"))
            or Path(inp).is_absolute()
            or ".." in Path(inp).parts
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


def validate_manifest_entry(entry: dict[str, Any]) -> list[str]:
    output = entry.get("output")
    output_problems = _validate_manifest_output(output)
    if output_problems or not isinstance(output, str):
        return output_problems
    return [
        *_validate_manifest_inputs(output, entry.get("inputs")),
        *_validate_manifest_compile_args(output, entry.get("compile_args")),
    ]


def load_manifest(root: Path) -> dict[str, Any]:
    path = root / MANIFEST_PATH
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"{MANIFEST_PATH}: {error}") from error
    if not isinstance(data, dict) or not isinstance(data.get("locks"), list):
        raise ContractError(f"{MANIFEST_PATH}: expected an object with a locks array")
    version = data.get("uv_version")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", version):
        raise ContractError(f"{MANIFEST_PATH}: uv_version must be an exact release")
    for raw_entry in data["locks"]:
        if not isinstance(raw_entry, dict):
            raise ContractError(f"{MANIFEST_PATH}: lock entry is not an object")
        entry_problems = validate_manifest_entry(raw_entry)
        if entry_problems:
            raise ContractError(f"{MANIFEST_PATH}: {'; '.join(entry_problems)}")
    return data


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
            content = path.read_bytes()
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
        if PIP_NAME_RE.search(token) and not invoked_by_python and index + 1 < len(tokens):
            if tokens[index + 1].lower() == "install":
                arguments_at = index + 2
        elif PYTHON_NAME_RE.search(token) and index + 3 < len(tokens):
            if [part.lower() for part in tokens[index + 1 : index + 4]] == [
                "-m",
                "pip",
                "install",
            ]:
                arguments_at = index + 4
        if arguments_at is None:
            continue
        end = arguments_at
        while end < len(tokens) and tokens[end] not in {"&", "&&", ";", "|", "||"}:
            end += 1
        yield ["pip", "install", *tokens[arguments_at:end]]


def _is_local_source(value: str) -> bool:
    if "://" in value or value.startswith(("git+", "hg+", "svn+", "bzr+")):
        return False
    return (
        value.startswith((".", "/", "~"))
        or "/" in value
        or "\\" in value
        or value.endswith((".whl", ".tar.gz", ".tgz", ".tar.bz2", ".zip"))
        or Path(value).exists()
    )


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


def _is_secure_hash_install(tokens: list[str], lowered: list[str]) -> bool:
    if "--require-hashes" not in lowered:
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
    return not any("://" in t or t.startswith(("git+", "hg+", "svn+", "bzr+")) for t in req_targets)


def _extract_editable_target(tokens: list[str], lowered: list[str]) -> str | None:
    editable_index = next(
        (index for index, token in enumerate(lowered) if token in {"-e", "--editable"}),
        None,
    )
    if editable_index is not None and editable_index + 1 < len(tokens):
        return tokens[editable_index + 1]
    for token in tokens:
        if token.lower().startswith(("--editable=", "-e=")):
            return token.split("=", 1)[1]
    return None


def _is_secure_install(tokens: list[str]) -> bool:
    lowered = [token.lower() for token in tokens]
    if _is_secure_hash_install(tokens, lowered):
        return True

    no_deps = "--no-deps" in lowered
    no_build_isolation = "--no-build-isolation" in lowered

    editable_target = _extract_editable_target(tokens, lowered)
    if editable_target is not None:
        return no_deps and no_build_isolation and _is_local_source(editable_target)

    package_tokens = _package_arguments(tokens)
    if not package_tokens:
        return False

    if all(token.lower().endswith(".whl") and _is_local_source(token) for token in package_tokens):
        return no_deps

    if all(_is_local_source(token) for token in package_tokens):
        return no_deps and no_build_isolation

    return False


def scan_install_commands(path: Path, text: str) -> list[str]:
    """Return unsafe executable pip-install commands in one supported surface."""

    findings: list[str] = []
    for line, command in logical_lines(text):
        stripped = command.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        # Shell help/error text is data, not an executed package install.
        prefix = re.sub(r"^(?:run:|RUN)\s*", "", stripped, flags=re.IGNORECASE).lstrip()
        if prefix.startswith(("echo ", "printf ", "#")):
            continue
        for tokens in _pip_install_tokens(command):
            if _is_secure_install(tokens):
                continue
            findings.append(
                f"{path}:{line}: pip install must use --require-hashes with a lock file, "
                "or install a local wheel with --no-deps, "
                "or install a local editable/source tree with both --no-deps and --no-build-isolation"
            )
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
        except (subprocess.CalledProcessError, OSError):
            pass

    if not raw_paths and not git_dir.exists():
        for path in root.rglob("*"):
            try:
                rel = path.relative_to(root)
            except ValueError:
                continue
            if any(part.startswith(".") for part in rel.parts):
                continue
            if path.is_file():
                raw_paths.append(rel)

    paths = []
    for relative in raw_paths:
        parts = relative.parts
        name = relative.name.lower()
        supported = (
            relative.suffix.lower() in {".sh", ".ps1"}
            or (parts[:2] == (".github", "workflows") and relative.suffix in {".yml", ".yaml"})
            or name.startswith(("dockerfile", "containerfile"))
            or name in {"makefile", "gnumakefile"}
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
        except (subprocess.CalledProcessError, OSError):
            pass

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


def check(root: Path) -> int:
    try:
        manifest = load_manifest(root)
        problems = []
        outputs: set[str] = set()
        for raw_entry in manifest["locks"]:
            if not isinstance(raw_entry, dict):
                problems.append("manifest lock entry is not an object")
                continue
            output = raw_entry.get("output")
            if isinstance(output, str) and output in outputs:
                problems.append(f"manifest repeats output {output}")
            elif isinstance(output, str):
                outputs.add(output)
            problems.extend(validate_lock(root, manifest, raw_entry))

        outputs_paths = {str(Path(o)) for o in outputs}
        for lock_file in find_lock_files(root):
            if str(lock_file) not in outputs_paths:
                problems.append(f"unregistered lock file {lock_file} not present in manifest")

        for relative in tracked_consumer_paths(root):
            try:
                text = (root / relative).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError) as error:
                problems.append(f"{relative}: {error}")
                continue
            problems.extend(scan_install_commands(relative, text))
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

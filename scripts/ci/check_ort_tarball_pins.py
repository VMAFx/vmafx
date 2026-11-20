#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard ONNX Runtime workflow sources and ClangBuildAnalyzer tarball pins.

The ONNX Runtime NuGet pin table is the single source for CPU and GPU installs.
ClangBuildAnalyzer downloads retain their existing digest and order checks.
Exit codes: 0 success, 1 findings, 2 I/O or configuration error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path, PurePosixPath
from typing import Any

EXIT_SUCCESS = 0
EXIT_FINDINGS = 1
EXIT_ERROR = 2

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT
sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

HEX_RE = re.compile(r"^[a-f0-9]{64}$")
STEP_START_RE = re.compile(r"^(\s*)-\s+(?:name|uses|run):")
GENERIC_STEP_RE = re.compile(r"^(\s*)-\s+")
TAR_EXTRACT_RE = re.compile(r"\btar\b.*?(?:-[a-zA-Z0-9]*x|--extract)")
SHA_CHECK_RE = re.compile(r"\bsha256sum\b.*?(?:--check|-c\b)")
GPU_MARKER_RE = re.compile(r"onnxruntime-[^\s\"'/]*-gpu")
PACKAGE_FLAG_RE = re.compile(r"--package(?:=|\s+)[\"']?([A-Za-z0-9_-]+)")
VERSION_FLAG_RE = re.compile(r"--version(?:=|\s+)[\"']?([^\"'\s#\\]+)")
MAX_COMMAND_LINES = 256
MAX_STEP_LINES = 10000
BINDING_RE = re.compile(
    r"^\s*(?:(?:export|ARG|ENV)\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*[:=]\s*[\"']?([^\"'\s#\\$}]+)"
)
INLINE_BINDING_RE = re.compile(r"(?:^|[\s;&|])([A-Za-z_][A-Za-z0-9_]*)=[\"']?([^\"'\s#\\;$}]+)")
HELPER_FLAGS: frozenset[str] = frozenset(
    {
        "--package",
        "--version",
        "--prefix",
        "--pkgconfig-dir",
        "--cache-dir",
        "--print-pin",
    }
)
CONTAINER_SHAPE_FINDING = "container ort_nuget_install.py instruction violates canonical RUN shape"
CONTAINER_RUN_RE = re.compile(r"^RUN\s+(?!-|\s*\[)(.+)$", re.I)
CONTAINER_FORBIDDEN_RE = re.compile(
    r"[`;|(){}]|<<|\$\(|(?<!&)&(?!&)|\beval\b|\b(?:sh|bash)\s+-[A-Za-z]*c\b|\benv\s"
)
CONTAINER_HELPER_TOKEN_RE = re.compile(
    r"(?:/build/vmaf/)?scripts/ci/ort_nuget_install\.py|scripts\.ci\.ort_nuget_install"
)
CONTAINER_HELPER_PATHS = frozenset(
    {
        "scripts/ci/ort_nuget_install.py",
        "/build/vmaf/scripts/ci/ort_nuget_install.py",
    }
)
CONTAINER_MODULE = "scripts.ci.ort_nuget_install"
MODULE_PREFIX_LEN = 2
FILE_PREFIX_LEN = 1
CONTAINER_EXPORT_RE = re.compile(r"^export\s+ORT_VERSION=([0-9]+(?:\.[0-9]+)+)$")
CONTAINER_SET_RE = re.compile(r"^set(?:\s+-[eux]+|\s+-o\s+pipefail)?$")
CONTAINER_FLAG_VALUE_RE = re.compile(r"^[A-Za-z0-9_./-]+$")
CONTAINER_LITERAL_VERSION_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)+$")
CONTAINER_VERSION_VAR_RE = re.compile(r"^\$(?:ORT_VERSION|\{ORT_VERSION\})$")
NUGET_ADDRESS_RE = re.compile(
    r"(?:api\.nuget\.org/v3-flatcontainer/|nuget\.org/api/v2/package/)"
    r"(\$\{[A-Za-z_][A-Za-z0-9_]*\}|\$[A-Za-z_][A-Za-z0-9_]*|[^/\s\"']+)",
    re.I,
)
ORT_PACKAGE_RE = re.compile(r"microsoft\.ml\.onnxruntime(?:[.a-z0-9_-]*)", re.I)


class TrackedDownload:
    """Specification of a tracked release tarball download."""

    def __init__(
        self,
        name: str,
        marker: str,
        sha_var: str,
        version_var: str,
        has_gpu_flavour: bool = False,
    ) -> None:
        self.name = name
        self.marker = marker
        self.sha_var = sha_var
        self.version_var = version_var
        self.has_gpu_flavour = has_gpu_flavour
        self.sha_re = re.compile(rf"\b{re.escape(sha_var)}\b\s*[:=]\s*[\"']?([^\"'\s#\\]+)[\"']?")
        self.version_re = re.compile(
            rf"\b{re.escape(version_var)}\b\s*[:=]\s*[\"']?([0-9.]+)[\"']?"
        )


TRACKED_DOWNLOADS: tuple[TrackedDownload, ...] = (
    TrackedDownload(
        name="ONNX Runtime",
        marker="onnxruntime/releases/download",
        sha_var="ORT_SHA256",
        version_var="ORT_VERSION",
        has_gpu_flavour=True,
    ),
    TrackedDownload(
        name="ClangBuildAnalyzer",
        marker="aras-p/ClangBuildAnalyzer/archive",
        sha_var="CLANGBUILDANALYZER_SHA256",
        version_var="CLANGBUILDANALYZER_VERSION",
        has_gpu_flavour=False,
    ),
)


def _find_workflow_block(lines: list[str], site_idx: int) -> tuple[int, int]:
    """Find start and end line indices of the workflow step enclosing site_idx."""
    n = len(lines)
    start_idx = site_idx
    indent = 6
    for j in range(site_idx, -1, -1):
        m = STEP_START_RE.match(lines[j]) or GENERIC_STEP_RE.match(lines[j])
        if m:
            start_idx = j
            indent = len(m.group(1))
            break

    end_idx = n - 1
    for j in range(site_idx + 1, n):
        line = lines[j]
        m = GENERIC_STEP_RE.match(line)
        if m and len(m.group(1)) <= indent:
            end_idx = j - 1
            break
        m_key = re.match(r"^(\s*)([a-zA-Z0-9_-]+):\s*$", line)
        if m_key and len(m_key.group(1)) <= indent:
            end_idx = j - 1
            break
    return start_idx, end_idx


def _container_instruction(
    lines: list[str], site_idx: int
) -> tuple[int, int, str | None, str | None]:
    """Return start index, end index, spaced text and fused text for an instruction."""
    start_idx = site_idx
    for j in range(site_idx - 1, max(-1, site_idx - 2 - MAX_COMMAND_LINES), -1):
        stripped = lines[j].strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not stripped.endswith("\\"):
            break
        start_idx = j

    parts: list[str] = []
    end_idx = start_idx
    for idx in range(start_idx, min(len(lines), start_idx + MAX_COMMAND_LINES)):
        end_idx = idx
        stripped = lines[idx].strip()
        if idx > start_idx and (not stripped or stripped.startswith("#")):
            continue
        parts.append(stripped.rstrip("\\"))
        if not stripped.endswith("\\"):
            return start_idx, end_idx, " ".join(parts), "".join(parts)
    return start_idx, end_idx, None, None


def _find_docker_block(lines: list[str], site_idx: int) -> tuple[int, int]:
    """Find the build stage containing the instruction that starts at site_idx."""
    run_idx, end_idx, _, _ = _container_instruction(lines, site_idx)
    start_idx = 0
    for j in range(run_idx - 1, max(-1, run_idx - 1 - MAX_STEP_LINES), -1):
        if re.match(r"^\s*FROM\b", lines[j], re.I):
            start_idx = j + 1
            break
    return start_idx, max(site_idx, end_idx)


def _docker_bindings(lines: list[str], start_idx: int, run_idx: int) -> dict[str, set[str]]:
    """ARG and ENV literal values in the build stage, up to the RUN."""
    bindings: dict[str, set[str]] = {}
    if run_idx - start_idx > MAX_STEP_LINES:
        return bindings
    for line in lines[start_idx:run_idx]:
        match = re.match(
            r"^\s*(?:ARG|ENV)\s+([A-Za-z_][A-Za-z0-9_]*)" r"(?:\s*=\s*|\s+)[\"']?([^\"'\s#\\]+)",
            line,
            re.I,
        )
        if match:
            bindings.setdefault(match.group(1), set()).add(match.group(2))
    return bindings


def find_block_for_site(lines: list[str], site_idx: int, is_workflow: bool) -> tuple[int, int]:
    """Find the (start_idx, end_idx) 0-indexed bounds of the block enclosing site_idx."""
    if is_workflow:
        return _find_workflow_block(lines, site_idx)
    return _find_docker_block(lines, site_idx)


def _check_sha_pin(
    entry: TrackedDownload,
    block_lines: list[str],
    start_idx: int,
    site_line_num: int,
    path: Path,
) -> tuple[str | None, int, list[str]]:
    """Verify sha pin presence and validity in block."""
    sha_val: str | None = None
    sha_line_num: int = site_line_num
    findings: list[str] = []

    for offset, b_line in enumerate(block_lines):
        m = entry.sha_re.search(b_line)
        if m:
            sha_val = m.group(1).strip()
            sha_line_num = start_idx + offset + 1
            break

    if sha_val is None:
        findings.append(
            f"{path}:{site_line_num}: {entry.name}: missing {entry.sha_var} pin in block"
        )
    elif not HEX_RE.match(sha_val):
        findings.append(
            f"{path}:{sha_line_num}: {entry.name}: malformed {entry.sha_var} hex digest: '{sha_val}'"
        )

    return sha_val, sha_line_num, findings


def _check_verification_order(
    entry: TrackedDownload,
    block_lines: list[str],
    start_idx: int,
    site_line_num: int,
    path: Path,
) -> list[str]:
    """Verify sha256sum --check occurs after download and before tar -x."""
    findings: list[str] = []
    check_line_num: int | None = None
    tar_line_num: int | None = None

    for offset, b_line in enumerate(block_lines):
        line_no = start_idx + offset + 1
        if check_line_num is None and SHA_CHECK_RE.search(b_line):
            check_line_num = line_no
        if tar_line_num is None and TAR_EXTRACT_RE.search(b_line):
            tar_line_num = line_no

    if check_line_num is None:
        findings.append(f"{path}:{site_line_num}: {entry.name}: missing sha256sum --check in block")
    elif check_line_num <= site_line_num:
        findings.append(
            f"{path}:{check_line_num}: {entry.name}: sha256sum --check occurs before or at download line {site_line_num}"
        )

    if tar_line_num is not None and check_line_num is not None and tar_line_num <= check_line_num:
        findings.append(
            f"{path}:{tar_line_num}: {entry.name}: tar extraction occurs before sha256sum --check on line {check_line_num}"
        )

    return findings


def _extract_site_metadata(entry: TrackedDownload, block_text: str) -> tuple[str, str]:
    """Extract flavour (cpu/gpu/default) and version from block text."""
    flavour = "default"
    if entry.has_gpu_flavour:
        flavour = "gpu" if GPU_MARKER_RE.search(block_text) else "cpu"

    version_match = entry.version_re.search(block_text)
    if version_match:
        version = version_match.group(1)
    else:
        url_match = re.search(r"/v([0-9.]+)[/.]", block_text)
        version = url_match.group(1) if url_match else "unknown"
    return flavour, version


def _match_tracked_entry(line: str) -> TrackedDownload | None:
    """Return matching TrackedDownload for a line if present."""
    for entry in TRACKED_DOWNLOADS:
        if entry.marker in line:
            return entry
    return None


def _check_pin_entry(pin_file: Path, pkg_name: str, version: str, pin: Any) -> list[str]:
    """Validate a single version entry's sha256 and size."""
    if not isinstance(pin, dict):
        return [f"{pin_file}: malformed pin entry for {pkg_name} {version}"]
    findings: list[str] = []
    sha = pin.get("sha256")
    if not isinstance(sha, str) or not HEX_RE.fullmatch(sha):
        findings.append(f"{pin_file}: malformed sha256 pin for {pkg_name} {version}")
    size = pin.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        findings.append(f"{pin_file}: invalid size pin for {pkg_name} {version}")
    return findings


def _check_package_fields(pin_file: Path, name: str, data: dict[str, Any]) -> list[str]:
    """Check installer metadata for a package."""
    findings = []
    for field in ("source", "header_dir"):
        value = data.get(field)
        if not isinstance(value, str) or not value.strip():
            findings.append(f"{pin_file}: invalid {field} for package {name}")
    libraries = data.get("libraries")
    if (
        not isinstance(libraries, list)
        or not libraries
        or any(not isinstance(item, str) or not item.strip() for item in libraries)
    ):
        findings.append(f"{pin_file}: invalid libraries for package {name}")
    return findings


def _validate_pin_table(pin_file: Path) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Parse and validate ort-nuget-pins.json."""
    raw = json.loads(pin_file.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        return {}, [f"{pin_file}: root must be an object"]
    packages = raw.get("packages")
    if "packages" not in raw and "versions" in raw:
        packages = {"cpu": raw}
    if not isinstance(packages, dict) or not packages:
        return {}, [f"{pin_file}: missing packages table"]
    findings: list[str] = []
    legacy = "packages" not in raw
    valid = {}
    for pkg_name, pkg_data in packages.items():
        if not isinstance(pkg_data, dict):
            findings.append(f"{pin_file}: malformed package {pkg_name}")
            continue
        if not legacy:
            findings.extend(_check_package_fields(pin_file, pkg_name, pkg_data))
        versions = pkg_data.get("versions") if isinstance(pkg_data, dict) else None
        if not isinstance(versions, dict) or not versions:
            findings.append(f"{pin_file}: missing versions for package {pkg_name}")
            continue
        valid[pkg_name] = pkg_data
        for version, pin in versions.items():
            findings.extend(_check_pin_entry(pin_file, pkg_name, str(version), pin))
    return valid, findings


def _step_bindings(lines: list[str]) -> dict[str, set[str]] | None:
    """Collect literal YAML and shell bindings, ignoring comments."""
    if len(lines) > MAX_STEP_LINES:
        return None
    bindings: dict[str, set[str]] = {}
    for line in lines:
        match = BINDING_RE.match(line)
        if match:
            bindings.setdefault(match.group(1), set()).add(match.group(2))
    return bindings


def _helper_flag_pair(tokens: list[str], index: int) -> tuple[str, str, int] | None:
    """Split one flag/value pair, returning the next index."""
    argument = tokens[index]
    if argument == "--print-pin" or argument.startswith("--print-pin="):
        return None
    if not argument.startswith("--"):
        return None
    if "=" in argument:
        flag, value = argument.split("=", 1)
        return flag, value, index + 1
    if index + 1 >= len(tokens):
        return None
    return argument, tokens[index + 1], index + 2


def _apply_helper_flag(
    flag: str, value: str, package: str, version: str | None
) -> tuple[str, str | None] | None:
    """Validate one allowed helper flag and return the updated package/version."""
    if flag == "--package":
        if re.fullmatch(r"[A-Za-z0-9_-]+", value) is None:
            return None
        return value, version
    if flag == "--version":
        if (
            CONTAINER_LITERAL_VERSION_RE.fullmatch(value) is None
            and CONTAINER_VERSION_VAR_RE.fullmatch(value) is None
        ):
            return None
        return package, value
    if CONTAINER_FLAG_VALUE_RE.fullmatch(value) is None:
        return None
    return package, version


def _parse_helper_flags(tokens: list[str]) -> tuple[str, str | None] | None:
    """Parse the flags of one helper call; None when any flag is outside the allow-list."""
    package = "cpu"
    version: str | None = None
    seen: set[str] = set()
    index = 0
    while index < len(tokens):
        parsed = _helper_flag_pair(tokens, index)
        if parsed is None:
            return None
        flag, value, index = parsed
        if flag not in HELPER_FLAGS or flag in seen:
            return None
        seen.add(flag)
        applied = _apply_helper_flag(flag, value, package, version)
        if applied is None:
            return None
        package, version = applied
    if version is None or "--prefix" not in seen:
        return None
    return package, version


def _parse_helper_tokens(tokens: list[str]) -> tuple[str, str | None] | None:
    """Parse one helper simple command with either the file or module spelling."""
    if not tokens or tokens[0] != "python3":
        return None
    if len(tokens) > MODULE_PREFIX_LEN and tokens[1] == "-m":
        if tokens[2] != CONTAINER_MODULE:
            return None
        return _parse_helper_flags(tokens[3:])
    if len(tokens) > FILE_PREFIX_LEN and tokens[1] in CONTAINER_HELPER_PATHS:
        return _parse_helper_flags(tokens[2:])
    return None


def _container_binding(
    version: str | None, exported: dict[str, str], bindings: dict[str, set[str]]
) -> tuple[str | None, str]:
    """Resolve a helper's $ORT_VERSION from the RUN export or the build stage."""
    if version is not None and CONTAINER_LITERAL_VERSION_RE.fullmatch(version):
        return version, ""
    if "ORT_VERSION" in exported:
        return exported["ORT_VERSION"], ""
    values = bindings.get("ORT_VERSION", set())
    if len(values) > 1:
        return None, "ambiguous ORT_VERSION in build stage"
    if not values:
        return None, "missing ORT_VERSION in build stage"
    return next(iter(values)), ""


def _run_helper_body(command: str) -> str | None:
    """Return a canonical RUN body, or None when the instruction is not eligible."""
    run_match = CONTAINER_RUN_RE.fullmatch(command.strip())
    if run_match is None:
        return None
    body = run_match.group(1).replace("${ORT_VERSION}", "$ORT_VERSION")
    if CONTAINER_FORBIDDEN_RE.search(body):
        return None
    return body


def _parse_helper_segment(
    tokens: list[str],
    stripped: str,
    exported: dict[str, str],
    bindings: dict[str, set[str]],
) -> tuple[str, str | None, str] | None:
    """Parse a helper simple command inside a RUN instruction."""
    helper_tokens = CONTAINER_HELPER_TOKEN_RE.findall(stripped)
    if not helper_tokens or len(helper_tokens) != 1:
        return None
    parsed = _parse_helper_tokens(tokens)
    if parsed is None:
        return None
    package, version = parsed
    resolved, error = _container_binding(version, exported, bindings)
    return package, resolved, error


def _record_export(stripped: str, exported: dict[str, str]) -> bool:
    """Record a canonical ORT_VERSION export; False means invalid or not an export."""
    assignment = CONTAINER_EXPORT_RE.fullmatch(stripped)
    if not assignment:
        return False
    value = assignment.group(1)
    if "ORT_VERSION" in exported and exported["ORT_VERSION"] != value:
        return False
    exported["ORT_VERSION"] = value
    return True


def _split_segment(segment: str) -> list[str] | None:
    """Strip and shell-split one RUN segment; None for blank or malformed input."""
    stripped = segment.strip()
    if not stripped:
        return None
    try:
        return shlex.split(stripped)
    except ValueError:
        return None


def _parse_container_helper_instruction(
    command: str, bindings: dict[str, set[str]]
) -> list[tuple[str, str | None, str]] | None:
    """Parse a canonical RUN instruction into helper calls and their resolved versions."""
    body = _run_helper_body(command)
    if body is None:
        return None
    raw_segments = body.split("&&", MAX_COMMAND_LINES + 1)
    if len(raw_segments) > MAX_COMMAND_LINES:
        return None
    exported: dict[str, str] = {}
    calls: list[tuple[str, str | None, str]] = []
    for segment in raw_segments:
        stripped = segment.strip()
        tokens = _split_segment(segment)
        if tokens is None:
            return None
        call = _parse_helper_segment(tokens, stripped, exported, bindings)
        if call is not None:
            calls.append(call)
            continue
        if _record_export(stripped, exported):
            continue
        if CONTAINER_SET_RE.fullmatch(stripped) or stripped == "ldconfig":
            continue
        return None
    if not calls:
        return None
    return calls


def _container_helper_token_lines(lines: list[str], start_idx: int, end_idx: int) -> list[int]:
    """Line numbers of helper tokens in an instruction, in call order."""
    found: list[int] = []
    for idx in range(start_idx, end_idx + 1):
        if lines[idx].lstrip().startswith("#"):
            continue
        found.extend([idx + 1] * len(CONTAINER_HELPER_TOKEN_RE.findall(lines[idx])))
    return found


def _helper_command(
    lines: list[str], site_idx: int, end_idx: int, is_workflow: bool = True
) -> list[str] | None:
    """Collect shell helper call(s) at site_idx, rejecting excessively long continuations."""
    if not is_workflow:
        return None
    parts: list[str] = []
    for idx in range(site_idx, min(end_idx + 1, site_idx + MAX_COMMAND_LINES)):
        line = lines[idx].strip()
        parts.append(line.rstrip("\\"))
        if not line.endswith("\\"):
            return [" ".join(parts)]
    return None


def _resolve_helper_version(
    lines: list[str],
    start_idx: int,
    site_idx: int,
    call_text: str,
) -> tuple[str | None, str]:
    """Resolve the literal or bound ORT_VERSION for a helper call."""
    ver_match = VERSION_FLAG_RE.search(call_text)
    version = ver_match.group(1) if ver_match else None
    if version not in ("$ORT_VERSION", "${ORT_VERSION}"):
        return version, ""
    prefix = call_text.partition("ort_nuget_install.py")[0]
    inline = dict(INLINE_BINDING_RE.findall(prefix))
    if "ORT_VERSION" in inline:
        return inline["ORT_VERSION"], ""
    bindings = _step_bindings(lines[start_idx : site_idx + 1])
    if bindings is None:
        return None, "step binding limit exceeded"
    values = bindings.get("ORT_VERSION", set())
    if len(values) > 1:
        return None, "ambiguous ORT_VERSION in step"
    return next(iter(values), None), ""


def _check_helper_call(
    lines: list[str],
    start_idx: int,
    bound_end: int,
    call_text: str,
    packages: dict[str, dict[str, Any]],
) -> str:
    """Verify the package and version of a single helper invocation."""
    pkg_match = PACKAGE_FLAG_RE.search(call_text)
    if "--package" in call_text and pkg_match is None:
        return "invalid ONNX Runtime package argument"
    pkg_name = pkg_match.group(1) if pkg_match else "cpu"
    version, message = _resolve_helper_version(lines, start_idx, bound_end, call_text)
    if message:
        return message
    if pkg_name not in packages:
        return f"unknown ONNX Runtime package '{pkg_name}' in step"
    if version is None:
        return "missing ORT_VERSION in step calling ort_nuget_install.py"
    if version not in packages[pkg_name].get("versions", {}):
        return f"unpinned ORT_VERSION '{version}' for package '{pkg_name}'"
    return ""


def _check_container_helper_step(
    lines: list[str],
    site_idx: int,
    path: Path,
    packages: dict[str, dict[str, Any]],
) -> list[str]:
    """Validate every helper call and every other simple command in one container RUN."""
    run_idx, end_idx, command, fused = _container_instruction(lines, site_idx)
    line_no = run_idx + 1
    if command is None or fused is None:
        return [f"{path}:{line_no}: container command continuation exceeds limit"]
    start_idx, _ = _find_docker_block(lines, run_idx)
    bindings = _docker_bindings(lines, start_idx, run_idx)
    calls = _parse_container_helper_instruction(command, bindings)
    if calls is None:
        return [f"{path}:{line_no}: {CONTAINER_SHAPE_FINDING}"]
    token_lines = _container_helper_token_lines(lines, run_idx, end_idx)
    if len(token_lines) != len(calls):
        return [f"{path}:{line_no}: {CONTAINER_SHAPE_FINDING}"]
    findings: list[str] = []
    for (package, version, error), call_line in zip(calls, token_lines, strict=True):
        if error:
            findings.append(f"{path}:{call_line}: {error}")
        elif package not in packages:
            findings.append(f"{path}:{call_line}: unknown ONNX Runtime package '{package}' in step")
        elif version is None:
            findings.append(f"{path}:{call_line}: missing ORT_VERSION in build stage")
        elif version not in packages[package].get("versions", {}):
            findings.append(
                f"{path}:{call_line}: unpinned ORT_VERSION '{version}' for package '{package}'"
            )
    return findings


def _check_helper_step(
    lines: list[str],
    site_idx: int,
    path: Path,
    packages: dict[str, dict[str, Any]],
    is_workflow: bool,
) -> list[str]:
    """Verify helper invocation(s) whose token is on site_idx."""
    if not is_workflow:
        return _check_container_helper_step(lines, site_idx, path, packages)
    start_idx, end_idx = find_block_for_site(lines, site_idx, is_workflow)
    calls = _helper_command(lines, site_idx, end_idx, is_workflow)
    line_no = site_idx + 1
    if calls is None:
        return [f"{path}:{line_no}: helper command continuation exceeds limit"]
    bound_end = site_idx
    findings: list[str] = []
    for call_text in calls:
        message = _check_helper_call(lines, start_idx, bound_end, call_text, packages)
        if message:
            findings.append(f"{path}:{line_no}: {message}")
    return findings


def _check_nuget_download(  # noqa: PLR0911
    lines: list[str], idx: int, path: Path, line: str, is_workflow: bool
) -> list[str]:
    """Reject direct runtime downloads and unresolved NuGet package variables."""
    lower = line.lower()
    if not any(marker in lower for marker in ("curl ", "wget ", "https://")):
        return []
    if ORT_PACKAGE_RE.search(line):
        return [f"{path}:{idx + 1}: ONNX Runtime download must use helper"]
    match = NUGET_ADDRESS_RE.search(line)
    if not match or not match.group(1).startswith("$"):
        return []
    name = match.group(1)[1:].strip("{}")
    start_idx, end_idx = find_block_for_site(lines, idx, is_workflow)
    bindings = _step_bindings(lines[start_idx : end_idx + 1])
    if bindings is None:
        return [f"{path}:{idx + 1}: step binding limit exceeded"]
    values = bindings.get(name, set())
    if not values:
        return [f"{path}:{idx + 1}: NuGet download with unresolved package"]
    if any(ORT_PACKAGE_RE.fullmatch(value) for value in values):
        return [f"{path}:{idx + 1}: ONNX Runtime download must use helper"]
    return []


def _scan_workflow_line(
    lines: list[str],
    idx: int,
    path: Path,
    packages: dict[str, dict[str, Any]] | None,
    is_workflow: bool,
    line: str | None = None,
) -> list[str]:
    """Check a line or container command for ONNX Runtime source rules."""
    line = lines[idx] if line is None else line
    if line.lstrip().startswith("#"):
        return []
    if is_workflow:
        line = re.split(r"\s+#", line, maxsplit=1)[0]
    lower = line.lower()
    findings: list[str] = []
    if "onnxruntime/releases/download" in lower:
        findings.append(f"{path}:{idx + 1}: old ONNX Runtime release source forbidden")
    findings.extend(_check_nuget_download(lines, idx, path, line, is_workflow))
    if "ort_nuget_install.py" in line and packages is not None:
        findings.extend(_check_helper_step(lines, idx, path, packages, is_workflow))
    return findings


def _scan_container_line(
    lines: list[str], idx: int, path: Path, packages: dict[str, dict[str, Any]] | None
) -> list[str]:
    """Check a container instruction once, including continued RUN and ADD commands."""
    line = lines[idx]
    if not line.strip() or line.lstrip().startswith("#"):
        return []
    start_idx, _, command, fused = _container_instruction(lines, idx)
    if command is None or fused is None:
        if idx != start_idx:
            return []
        window = " ".join(
            line.strip().rstrip("\\")
            for line in lines[start_idx : start_idx + MAX_COMMAND_LINES + 1]
        )
        if not CONTAINER_HELPER_TOKEN_RE.search(window):
            return _scan_workflow_line(lines, idx, path, None, False, window)
        return [f"{path}:{idx + 1}: container command continuation exceeds limit"]
    if idx != start_idx:
        return []
    findings = _scan_workflow_line(lines, idx, path, None, False, f"{command} {fused}")
    if packages is not None and CONTAINER_HELPER_TOKEN_RE.search(fused):
        if not CONTAINER_HELPER_TOKEN_RE.search(command):
            findings.append(f"{path}:{idx + 1}: {CONTAINER_SHAPE_FINDING}")
        else:
            findings.extend(_check_helper_step(lines, idx, path, packages, False))
    return findings


def scan_file(
    path: Path,
    packages: dict[str, dict[str, Any]] | None = None,
) -> tuple[list[str], list[tuple[str, str, str, str, Path, int]]]:
    """Scan a file for tracked tarball download sites."""
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as err:
        raise OSError(f"Cannot read {path}: {err}") from err

    lines = content.splitlines()
    is_workflow = path.suffix in (".yml", ".yaml") and not path.name.lower().startswith(
        ("dockerfile", "containerfile")
    )
    findings: list[str] = []
    site_records: list[tuple[str, str, str, str, Path, int]] = []

    for i, line in enumerate(lines):
        if is_workflow:
            findings.extend(_scan_workflow_line(lines, i, path, packages, True))
        else:
            findings.extend(_scan_container_line(lines, i, path, packages))
        entry = _match_tracked_entry(line)
        if entry is None or entry.name == "ONNX Runtime":
            continue

        site_line_num = i + 1
        start_idx, end_idx = find_block_for_site(lines, i, is_workflow)
        block_lines = lines[start_idx : end_idx + 1]
        block_text = "\n".join(block_lines)

        sha_val, sha_line_num, sha_findings = _check_sha_pin(
            entry, block_lines, start_idx, site_line_num, path
        )
        findings.extend(sha_findings)
        findings.extend(
            _check_verification_order(entry, block_lines, start_idx, site_line_num, path)
        )

        flavour, version = _extract_site_metadata(entry, block_text)
        if sha_val and HEX_RE.match(sha_val):
            site_records.append((entry.name, flavour, version, sha_val, path, sha_line_num))

    return findings, site_records


def _is_target_path(relative: str) -> bool:
    """Whether a repository-relative path is a workflow or container file."""
    path = PurePosixPath(relative)
    if path.parts[:2] == (".github", "workflows") and path.suffix in (".yml", ".yaml"):
        return True
    name = path.name
    return name in {"Dockerfile", "Containerfile"} or name.startswith(
        ("Dockerfile.", "Containerfile.")
    )


def _tracked_files(root: Path) -> list[Path] | None:
    """Tracked files under root, or None when root is not a readable git worktree."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    try:
        result = run_command(
            ("git", "-C", str(root), "ls-files", "-z"),
            allowed_executables=("git",),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
            max_output_bytes=64 * 1_048_576,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return [root / name for name in result.stdout.split("\0") if name]


def _find_target_files(repo_root: Path) -> list[Path]:
    """Find tracked workflow and container files under repo_root."""
    tracked = _tracked_files(repo_root)
    if tracked is not None:
        return sorted(
            path
            for path in tracked
            if path.is_file() and _is_target_path(path.relative_to(repo_root).as_posix())
        )
    patterns = [
        ".github/workflows/*.yml",
        ".github/workflows/*.yaml",
        "Dockerfile*",
        "Containerfile*",
        "**/Dockerfile*",
        "**/Containerfile*",
    ]
    target_files: list[Path] = []
    for pat in patterns:
        for path in sorted(repo_root.glob(pat)):
            if path.is_file() and _is_target_path(path.relative_to(repo_root).as_posix()):
                target_files.append(path)
    return sorted(set(target_files))


def _load_pins_for_check(
    root: Path | None, files: list[Path] | None
) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Resolve and validate the ONNX Runtime NuGet pin file."""
    base = root if root is not None else DEFAULT_ROOT
    pin_file = base / "scripts/ci/ort-nuget-pins.json"
    if not pin_file.exists() and files is not None:
        pin_file = DEFAULT_ROOT / "scripts/ci/ort-nuget-pins.json"
    packages, pin_findings = _validate_pin_table(pin_file)
    return packages, pin_findings


def check_tarball_pins(
    root: Path | None = None,
    files: list[Path] | None = None,
) -> tuple[int, list[str]]:
    """Scan candidate files for tracked download verification."""
    target_files = files if files else _find_target_files(root if root else DEFAULT_ROOT)
    all_findings: list[str] = []
    all_sites: list[tuple[str, str, str, str, Path, int]] = []

    try:
        packages, pin_findings = _load_pins_for_check(root, files)
        all_findings.extend(pin_findings)
    except (OSError, ValueError, KeyError, TypeError) as err:
        return EXIT_ERROR, [f"check_ort_tarball_pins: error: {err}"]

    for path in target_files:
        if path.name == "ort-nuget-pins.json":
            continue
        try:
            findings, sites = scan_file(path, packages=packages)
            all_findings.extend(findings)
            all_sites.extend(sites)
        except OSError as err:
            return EXIT_ERROR, [f"check_ort_tarball_pins: error: {err}"]

    known_pairs: dict[tuple[str, str, str], tuple[str, Path, int]] = {}
    for entry_name, flavour, version, sha_val, path, line_no in all_sites:
        key = (entry_name, flavour, version)
        if key in known_pairs:
            orig_sha, orig_path, orig_line = known_pairs[key]
            if orig_sha != sha_val:
                all_findings.append(
                    f"{path}:{line_no}: {entry_name}: conflicting sha256 for ({flavour}, {version}): "
                    f"'{sha_val}' vs '{orig_sha}' at {orig_path}:{orig_line}"
                )
        else:
            known_pairs[key] = (sha_val, path, line_no)

    if all_findings:
        return EXIT_FINDINGS, all_findings
    return EXIT_SUCCESS, []


check_ort_pins = check_tarball_pins


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify SHA-256 verification on all tracked release tarball downloads (ADR-3334, ADR-3336)."
    )
    parser.add_argument("--root", type=Path, default=None, help="Root directory to scan from.")
    parser.add_argument("files", nargs="*", type=Path, help="Specific files to scan.")
    args = parser.parse_args()

    files = [f.resolve() for f in args.files] if args.files else None
    root = args.root.resolve() if args.root else None

    if root and not root.is_dir():
        print(f"check_ort_tarball_pins: error: root directory not found: {root}", file=sys.stderr)
        return EXIT_ERROR

    if files:
        for f in files:
            if not f.exists():
                print(f"check_ort_tarball_pins: error: file not found: {f}", file=sys.stderr)
                return EXIT_ERROR

    code, findings = check_tarball_pins(root=root, files=files)
    if code == EXIT_ERROR:
        for err in findings:
            print(err, file=sys.stderr)
        return EXIT_ERROR

    if code == EXIT_FINDINGS:
        for finding in findings:
            print(finding)
        return EXIT_FINDINGS

    return EXIT_SUCCESS


if __name__ == "__main__":
    sys.exit(main())

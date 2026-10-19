#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify that tracked release tarball downloads have verified sha256 pins (ADR-3334, ADR-3336).

Scans workflow files and Dockerfiles for tracked release tarball downloads:
- ONNX Runtime C/C++ release tarballs (ADR-3334)
- ClangBuildAnalyzer release tarballs (ADR-3336)

For each download site, verifies that:
1. The corresponding SHA256 variable is pinned to a valid 64-character lowercase hex digest.
2. A `sha256sum --check` verification occurs after the download and before
   any `tar -x` extraction.
3. Cross-site consistency: identical (name, flavour, version) tuples share the same
   digest everywhere. Single source of truth is the sites themselves.

Exit codes:
  0: all sites valid
  1: verification findings detected (file:line: reason)
  2: own error (unreadable file, I/O error, missing required directory)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

EXIT_SUCCESS = 0
EXIT_FINDINGS = 1
EXIT_ERROR = 2

HEX_RE = re.compile(r"^[a-f0-9]{64}$")
STEP_START_RE = re.compile(r"^(\s*)-\s+(?:name|uses|run):")
GENERIC_STEP_RE = re.compile(r"^(\s*)-\s+")
RUN_START_RE = re.compile(r"^\s*RUN\b")
DOCKER_INSTRUCTION_RE = re.compile(
    r"^\s*(?:FROM|RUN|COPY|ADD|USER|WORKDIR|ENTRYPOINT|CMD|VOLUME|EXPOSE|LABEL)\b"
)
TAR_EXTRACT_RE = re.compile(r"\btar\b.*?(?:-[a-zA-Z0-9]*x|--extract)")
SHA_CHECK_RE = re.compile(r"\bsha256sum\b.*?(?:--check|-c\b)")
GPU_MARKER_RE = re.compile(r"onnxruntime-[^\s\"'/]*-gpu")


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


def _find_docker_block(lines: list[str], site_idx: int) -> tuple[int, int]:
    """Find start and end line indices of Dockerfile RUN and preceding ARGs."""
    n = len(lines)
    run_idx = site_idx
    for j in range(site_idx, -1, -1):
        if RUN_START_RE.match(lines[j]):
            run_idx = j
            break

    start_idx = run_idx
    for j in range(run_idx - 1, -1, -1):
        line = lines[j]
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or re.match(r"^\s*(?:ARG|ENV)\b", line):
            start_idx = j
            continue
        break

    end_idx = site_idx
    for j in range(run_idx, n):
        end_idx = j
        if not lines[j].rstrip().endswith("\\"):
            break

    return start_idx, end_idx


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


def scan_file(path: Path) -> tuple[list[str], list[tuple[str, str, str, str, Path, int]]]:
    """Scan a file for tracked tarball download sites."""
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as err:
        raise OSError(f"Cannot read {path}: {err}") from err

    lines = content.splitlines()
    is_workflow = path.suffix in (".yml", ".yaml")
    findings: list[str] = []
    site_records: list[tuple[str, str, str, str, Path, int]] = []

    for i, line in enumerate(lines):
        entry = _match_tracked_entry(line)
        if entry is None:
            continue

        site_line_num = i + 1
        start_idx, end_idx = find_block_for_site(lines, i, is_workflow)
        block_lines = lines[start_idx : end_idx + 1]
        block_text = "\n".join(block_lines)

        sha_val, sha_line_num, sha_findings = _check_sha_pin(
            entry, block_lines, start_idx, site_line_num, path
        )
        findings.extend(sha_findings)

        order_findings = _check_verification_order(
            entry, block_lines, start_idx, site_line_num, path
        )
        findings.extend(order_findings)

        flavour, version = _extract_site_metadata(entry, block_text)
        if sha_val and HEX_RE.match(sha_val):
            site_records.append((entry.name, flavour, version, sha_val, path, sha_line_num))

    return findings, site_records


def _find_target_files(repo_root: Path) -> list[Path]:
    """Find all candidate files under repo_root."""
    target_files: list[Path] = []
    patterns = [
        ".github/workflows/*.yml",
        "dev/Containerfile",
        "Dockerfile",
        "Dockerfile.*",
        "docker/Dockerfile.*",
    ]
    for pat in patterns:
        for p in sorted(repo_root.glob(pat)):
            if p.is_file():
                target_files.append(p)
    return target_files


def check_tarball_pins(
    root: Path | None = None,
    files: list[Path] | None = None,
) -> tuple[int, list[str]]:
    """Scan candidate files for tracked download verification."""
    target_files = files if files else _find_target_files(root if root else Path())
    all_findings: list[str] = []
    all_sites: list[tuple[str, str, str, str, Path, int]] = []

    for path in target_files:
        try:
            findings, sites = scan_file(path)
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

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold vendored documentation assets to the hashes in their vendor.json.

Every directory under ``docs/`` that holds a ``vendor.json`` is a vendored
third-party asset (a font, a script). The manifest names the upstream release,
its licence file and the SHA-256 of every file copied from it (HISS-11). This
check fails when a listed file is missing or differs from its hash, when the
directory holds a file the manifest does not list, or when the licence file is
not among the listed files.

Exit status: 0 when every manifest matches, 1 on findings, 2 when a manifest
cannot be read.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = "vendor.json"
REQUIRED_KEYS = ("name", "version", "source", "source_sha256", "license", "license_file", "files")


class ManifestError(Exception):
    """A vendor.json that cannot be read or lacks a required key."""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as err:
        raise ManifestError(f"{path}: cannot read: {err}") from err
    if not isinstance(data, dict):
        raise ManifestError(f"{path}: top level is not an object")
    missing = [key for key in REQUIRED_KEYS if key not in data]
    if missing:
        raise ManifestError(f"{path}: missing keys {missing}")
    if not isinstance(data["files"], dict) or not data["files"]:
        raise ManifestError(f"{path}: 'files' must be a non-empty object")
    return data


def check_files(directory: Path, files: dict[str, Any]) -> list[str]:
    findings = []
    for name, entry in sorted(files.items()):
        target = directory / name
        expected = entry.get("sha256") if isinstance(entry, dict) else None
        if not expected:
            findings.append(f"{target}: no sha256 in the manifest")
        elif not target.is_file():
            findings.append(f"{target}: listed in {MANIFEST} but missing")
        elif sha256(target) != expected:
            findings.append(f"{target}: sha256 {sha256(target)} differs from {expected}")
    return findings


def check_directory(manifest_path: Path) -> list[str]:
    data = load_manifest(manifest_path)
    directory = manifest_path.parent
    files = data["files"]
    findings = check_files(directory, files)
    if data["license_file"] not in files:
        findings.append(f"{manifest_path}: licence file {data['license_file']!r} is not listed")
    present = {p.name for p in directory.iterdir() if p.is_file() and p.name != MANIFEST}
    for extra in sorted(present - set(files)):
        findings.append(f"{directory / extra}: not listed in {MANIFEST}")
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)
    manifests = sorted((args.root / "docs").rglob(MANIFEST))
    findings: list[str] = []
    try:
        for manifest in manifests:
            findings.extend(check_directory(manifest))
    except ManifestError as err:
        print(f"check_vendored_assets: {err}", file=sys.stderr)
        return 2
    for line in findings:
        print(f"check_vendored_assets: {line}", file=sys.stderr)
    if findings:
        return 1
    print(
        f"check_vendored_assets: {len(manifests)} vendored asset directories match their manifests"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

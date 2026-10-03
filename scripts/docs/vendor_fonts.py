#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write a vendored documentation font directory from its upstream release.

The documentation site serves its fonts itself (ADR-1508). Each directory under
``docs/assets/fonts/`` has a ``vendor.json`` naming the release archive, its
SHA-256 and, per file, the archive member it comes from. Font files are subset
to the Unicode ranges in the manifest so a page loads about a third of the
upstream bytes; the licence text is copied unchanged.

    pip install fonttools==4.60.1 brotli==1.1.0
    curl -LO <source from vendor.json>
    python3 scripts/docs/vendor_fonts.py --manifest docs/assets/fonts/inter/vendor.json \\
        --archive Inter-4.1.zip

``--check`` rebuilds every file in memory and fails when one differs from the
committed copy. ``scripts/docs/check_vendored_assets.py`` (in
``make docs-fragments-check``) checks the committed hashes without the archive.

Exit status: 0 on success, 1 when the archive or a member does not match the
manifest or ``--check`` finds a difference, 2 on a usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import zipfile
from pathlib import Path
from typing import Any


class VendorError(Exception):
    """The archive or the manifest does not match."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def subset_font(data: bytes, subset: dict[str, Any]) -> bytes:
    """Subset a WOFF2 font to the manifest's Unicode ranges and layout features."""
    try:
        from fontTools import subset as ft_subset  # noqa: PLC0415 - optional tool dependency
    except ImportError as err:
        raise VendorError(
            "fontTools is missing: pip install fonttools==4.60.1 brotli==1.1.0"
        ) from err
    options = ft_subset.Options()
    options.layout_features = subset.get("layout_features", ["*"])
    options.flavor = "woff2"
    # load_font and save_font keep the head table's timestamp, so a rebuild is
    # byte-identical (a plain TTFont.save() stamps the current time).
    font = ft_subset.load_font(io.BytesIO(data), options)
    subsetter = ft_subset.Subsetter(options=options)
    subsetter.populate(unicodes=ft_subset.parse_unicodes(subset["unicodes"]))
    subsetter.subset(font)
    out = io.BytesIO()
    ft_subset.save_font(font, out, options)
    return out.getvalue()


def build_files(manifest: dict[str, Any], archive: bytes) -> dict[str, bytes]:
    if sha256(archive) != manifest["source_sha256"]:
        raise VendorError(
            f"archive sha256 {sha256(archive)} differs from {manifest['source_sha256']}"
        )
    built = {}
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        for name, entry in sorted(manifest["files"].items()):
            data = zf.read(entry["from"])
            if entry.get("from_sha256") and sha256(data) != entry["from_sha256"]:
                raise VendorError(
                    f"{entry['from']}: sha256 {sha256(data)} differs from {entry['from_sha256']}"
                )
            if name.endswith(".woff2") and "subset" in manifest:
                data = subset_font(data, manifest["subset"])
            built[name] = data
    return built


def check(directory: Path, built: dict[str, bytes]) -> list[str]:
    findings = []
    for name, data in sorted(built.items()):
        target = directory / name
        if not target.is_file() or target.read_bytes() != data:
            findings.append(f"{target}: differs from a fresh build from the archive")
    return findings


def write(manifest_path: Path, manifest: dict[str, Any], built: dict[str, bytes]) -> None:
    for name, data in built.items():
        (manifest_path.parent / name).write_bytes(data)
        manifest["files"][name]["sha256"] = sha256(data)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        archive = args.archive.read_bytes()
    except (OSError, json.JSONDecodeError) as err:
        print(f"vendor_fonts: {err}", file=sys.stderr)
        return 2
    try:
        built = build_files(manifest, archive)
    except (VendorError, KeyError, zipfile.BadZipFile) as err:
        print(f"vendor_fonts: {err}", file=sys.stderr)
        return 1
    if args.check:
        findings = check(args.manifest.parent, built)
        for line in findings:
            print(f"vendor_fonts: {line}", file=sys.stderr)
        return 1 if findings else 0
    write(args.manifest, manifest, built)
    print(f"vendor_fonts: wrote {len(built)} files to {args.manifest.parent}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

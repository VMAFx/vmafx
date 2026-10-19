#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write the vendored KaTeX directory from its pinned npm release (ADR-2705).

The documentation site renders formulas with KaTeX served from the site itself,
never from a CDN. ``docs/javascripts/vendor/katex/vendor.json`` names the npm
tarball, its npm integrity (SHA-512) and its SHA-256, and per file the archive
member it comes from. Two changes are made to the release, both recorded in the
manifest: only the WOFF2 fonts are kept, and the ``@font-face`` sources of
``katex.min.css`` lose their WOFF and TrueType fallbacks (every browser that
runs the site reads WOFF2).

    curl -LO <source from vendor.json>
    python3 scripts/docs/vendor_katex.py --manifest docs/javascripts/vendor/katex/vendor.json \\
        --archive katex-<version>.tgz

``--check`` rebuilds every file in memory and fails when one differs from the
committed copy. ``scripts/docs/check_vendored_assets.py`` (in
``make docs-fragments-check``) checks the committed hashes without the archive.

Exit status: 0 on success, 1 when the archive or a member does not match the
manifest or ``--check`` finds a difference, 2 on a usage error.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import re
import sys
import tarfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.docs.vendor_fonts import VendorError, sha256  # noqa: E402 - needs ROOT on sys.path

# `src:url(fonts/X.woff2) format("woff2"),url(fonts/X.woff) ...,url(fonts/X.ttf) ...`
FALLBACKS = re.compile(r',url\(fonts/[A-Za-z0-9_-]+\.(?:woff|ttf)\) format\("(?:woff|truetype)"\)')


def npm_integrity(data: bytes) -> str:
    return "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode("ascii")


def woff2_only(css: bytes) -> bytes:
    """Drop the WOFF and TrueType fallbacks from the @font-face sources."""
    text, count = FALLBACKS.subn("", css.decode("utf-8"))
    if count == 0:
        raise VendorError("katex.min.css has no WOFF or TrueType fallback to drop")
    return text.encode("utf-8")


def build_files(manifest: dict[str, Any], archive: bytes) -> dict[str, bytes]:
    if sha256(archive) != manifest["source_sha256"]:
        raise VendorError(
            f"archive sha256 {sha256(archive)} differs from {manifest['source_sha256']}"
        )
    if npm_integrity(archive) != manifest["source_integrity"]:
        raise VendorError(
            f"archive integrity {npm_integrity(archive)} differs from {manifest['source_integrity']}"
        )
    built = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tf:
        for name, entry in sorted(manifest["files"].items()):
            member = tf.extractfile(entry["from"])
            if member is None:
                raise VendorError(f"{entry['from']}: not a file in the archive")
            data = member.read()
            if entry.get("from_sha256") and sha256(data) != entry["from_sha256"]:
                raise VendorError(
                    f"{entry['from']}: sha256 {sha256(data)} differs from {entry['from_sha256']}"
                )
            if name == "katex.min.css":
                data = woff2_only(data)
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
        target = manifest_path.parent / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
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
        print(f"vendor_katex: {err}", file=sys.stderr)
        return 2
    try:
        built = build_files(manifest, archive)
    except (VendorError, KeyError, tarfile.TarError) as err:
        print(f"vendor_katex: {err}", file=sys.stderr)
        return 1
    if args.check:
        findings = check(args.manifest.parent, built)
        for line in findings:
            print(f"vendor_katex: {line}", file=sys.stderr)
        return 1 if findings else 0
    write(args.manifest, manifest, built)
    print(f"vendor_katex: wrote {len(built)} files to {args.manifest.parent}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

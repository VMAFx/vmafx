#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Resolve, download, and verify Intel NEO compute stack packages.

Derives the matched set of intel-opencl-icd, libze-intel-gpu1, libigdgmm12,
intel-igc-core-2, and intel-igc-opencl-2 deb packages from a pinned
intel/compute-runtime release tag. See ADR-1145.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def make_request(url: str, token: Optional[str] = None) -> bytes:
    """Execute an HTTP GET with GitHub API / standard headers and rate-limit handling."""
    headers = {
        "User-Agent": "vmaf-dev-container-build",
        "Accept": "application/vnd.github+json",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.read()
    except urllib.error.HTTPError as err:
        body = ""
        try:
            body = err.read().decode("utf-8", errors="replace")
        except Exception:
            pass

        if err.code in (403, 429) or "rate limit" in body.lower():
            print(
                f"\nFATAL: GitHub API rate limit exceeded while accessing {url}.\n"
                f"Response: {body}\n"
                f"Remedy: provide GITHUB_TOKEN via --build-arg GITHUB_TOKEN=... or wait for rate limit reset.",
                file=sys.stderr,
            )
            sys.exit(1)
        elif err.code == 404:
            print(
                f"\nFATAL: Resource not found (HTTP 404): {url}\nResponse: {body}",
                file=sys.stderr,
            )
            sys.exit(1)
        else:
            print(
                f"\nFATAL: HTTP error {err.code} while accessing {url}: {err.reason}\nResponse: {body}",
                file=sys.stderr,
            )
            sys.exit(1)
    except Exception as err:
        print(f"\nFATAL: Network error while accessing {url}: {err}", file=sys.stderr)
        sys.exit(1)


def sha256_file(path: Path) -> str:
    """Compute sha256 hex digest of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def download_file(url: str, dest_path: Path, token: Optional[str] = None, max_retries: int = 3) -> None:
    """Download a file directly to dest_path using curl."""
    cmd = [
        "curl",
        "-fsSL",
        "--retry",
        str(max_retries),
        "--connect-timeout",
        "30",
        "-o",
        str(dest_path),
        url,
    ]
    if token and "api.github.com" in url:
        cmd.extend(["-H", f"Authorization: Bearer {token}"])

    try:
        subprocess.run(cmd, check=True)
    except Exception as err:
        print(f"FATAL: curl failed to download {url}: {err}", file=sys.stderr, flush=True)
        sys.exit(1)




def resolve_and_fetch(neo_ver: str, output_dir: Path, token: Optional[str] = None) -> None:
    """Resolve deb URLs, download, and verify against published checksums."""
    output_dir.mkdir(parents=True, exist_ok=True)
    api_url = f"https://api.github.com/repos/intel/compute-runtime/releases/tags/{neo_ver}"
    print(f"Querying Intel compute-runtime release metadata for {neo_ver}...")
    release_raw = make_request(api_url, token=token)
    try:
        release_data = json.loads(release_raw.decode("utf-8"))
    except json.JSONDecodeError as err:
        print(f"FATAL: Failed to parse GitHub API JSON from {api_url}: {err}", file=sys.stderr)
        sys.exit(1)

    assets: Dict[str, str] = {
        a["name"]: a["browser_download_url"] for a in release_data.get("assets", [])
    }
    body: str = release_data.get("body", "")

    # 1. Resolve gmmlib deb
    gmm_matches = [
        n
        for n in assets
        if re.match(r"^(intel-igdgmm12|libigdgmm12)_[0-9].*_amd64\.deb$", n)
        and not n.endswith(".ddeb")
    ]
    if not gmm_matches:
        print(f"FATAL: No gmmlib deb asset found in compute-runtime {neo_ver}", file=sys.stderr)
        sys.exit(1)
    gmm_name = gmm_matches[0]
    gmm_url = assets[gmm_name]
    gmm_ver_match = re.search(r"_(.+)_amd64\.deb$", gmm_name)
    gmm_ver = gmm_ver_match.group(1) if gmm_ver_match else "unknown"

    # 2. Resolve opencl-icd deb
    icd_matches = [
        n
        for n in assets
        if re.match(r"^intel-opencl-icd_[0-9].*_amd64\.deb$", n)
        and not n.endswith(".ddeb")
        and "legacy" not in n
    ]
    if not icd_matches:
        print(f"FATAL: No intel-opencl-icd deb asset found in compute-runtime {neo_ver}", file=sys.stderr)
        sys.exit(1)
    icd_name = icd_matches[0]
    icd_url = assets[icd_name]

    # 3. Resolve libze-intel-gpu1 deb
    ze_matches = [
        n
        for n in assets
        if re.match(r"^(intel-level-zero-gpu|libze-intel-gpu1)_[0-9].*_amd64\.deb$", n)
        and not n.endswith(".ddeb")
        and "legacy" not in n
    ]
    if not ze_matches:
        print(f"FATAL: No libze-intel-gpu1 / level-zero-gpu deb asset found in compute-runtime {neo_ver}", file=sys.stderr)
        sys.exit(1)
    ze_name = ze_matches[0]
    ze_url = assets[ze_name]

    # 4. Resolve sum file
    sum_matches = [n for n in assets if n.endswith(".sum") or "sha256" in n]
    if not sum_matches:
        print(f"FATAL: No .sum / sha256 checksum asset found in compute-runtime {neo_ver}", file=sys.stderr)
        sys.exit(1)
    sum_name = sum_matches[0]
    sum_url = assets[sum_name]

    # 5. Resolve IGC debs (check assets first, then release body)
    igc_core_matches = [n for n in assets if re.match(r"^intel-igc-core-2_[0-9].*_amd64\.deb$", n)]
    igc_opencl_matches = [n for n in assets if re.match(r"^intel-igc-opencl-2_[0-9].*_amd64\.deb$", n)]
    igc_core_url = assets[igc_core_matches[0]] if igc_core_matches else None
    igc_opencl_url = assets[igc_opencl_matches[0]] if igc_opencl_matches else None

    if not igc_core_url:
        m = re.search(r"https://github\.com/[^\s]+/intel-igc-core-2_[^\s]+_amd64\.deb", body)
        if m:
            igc_core_url = m.group(0)
    if not igc_opencl_url:
        m = re.search(r"https://github\.com/[^\s]+/intel-igc-opencl-2_[^\s]+_amd64\.deb", body)
        if m:
            igc_opencl_url = m.group(0)

    if not igc_core_url or not igc_opencl_url:
        print(
            f"FATAL: Could not resolve IGC deb packages for compute-runtime {neo_ver} from assets or body.",
            file=sys.stderr,
        )
        sys.exit(1)

    igc_core_name = igc_core_url.split("/")[-1].replace("%2B", "+")
    igc_opencl_name = igc_opencl_url.split("/")[-1].replace("%2B", "+")
    igc_ver_match = re.search(r"intel-igc-core-2_(.+)_amd64\.deb$", igc_core_name)
    igc_ver = igc_ver_match.group(1) if igc_ver_match else "unknown"

    print("========================================================================")
    print(f"Intel NEO Compute Stack Resolution (ADR-1145)")
    print(f"Pinned NEO_VER: {neo_ver}")
    print(f"Resolved packages:")
    print(f"  - intel-opencl-icd: {icd_name}")
    print(f"  - libze-intel-gpu1: {ze_name}")
    print(f"  - gmmlib:           {gmm_name} (derived GMMLIB_VER: {gmm_ver})")
    print(f"  - intel-igc-core-2: {igc_core_name} (derived IGC_VER: {igc_ver})")
    print(f"  - intel-igc-opencl: {igc_opencl_name}")
    print(f"  - checksum file:    {sum_name}")
    print("========================================================================")

    # 6. Fetch checksums
    print(f"Downloading checksum file: {sum_url}...")
    sum_data = make_request(sum_url, token=token).decode("utf-8")
    expected_shas: Dict[str, str] = {}
    for line in sum_data.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2:
            expected_shas[parts[1]] = parts[0]

    # Fetch IGC checksums from IGC release if not in compute-runtime's sum file
    needed_igc = [f for f in (igc_core_name, igc_opencl_name) if f not in expected_shas]
    if needed_igc:
        tag_match = re.search(r"/releases/download/([^/]+)/", igc_core_url)
        if tag_match:
            igc_tag = tag_match.group(1)
            print(f"Fetching IGC checksums from intel-graphics-compiler tag {igc_tag}...")
            igc_api_url = f"https://api.github.com/repos/intel/intel-graphics-compiler/releases/tags/{igc_tag}"
            igc_raw = make_request(igc_api_url, token=token)
            igc_data = json.loads(igc_raw.decode("utf-8"))
            for sha, fname in re.findall(r"([a-f0-9]{64})\s+([^\s]+\.deb)", igc_data.get("body", "")):
                expected_shas[fname] = sha

    # 7. Download and verify every deb
    targets: List[Tuple[str, str]] = [
        (gmm_name, gmm_url),
        (icd_name, icd_url),
        (ze_name, ze_url),
        (igc_core_name, igc_core_url),
        (igc_opencl_name, igc_opencl_url),
    ]

    for deb_name, deb_url in targets:
        if deb_name not in expected_shas:
            print(f"FATAL: Checksum for {deb_name} not found in published release hashes!", file=sys.stderr)
            sys.exit(1)

        target_path = output_dir / deb_name
        print(f"Downloading {deb_name} from {deb_url}...")
        download_file(deb_url, target_path, token=token)

        actual_sha = sha256_file(target_path)
        expected_sha = expected_shas[deb_name]
        if actual_sha.lower() != expected_sha.lower():
            print(
                f"FATAL: Checksum mismatch for {deb_name}!\n  Expected: {expected_sha}\n  Actual:   {actual_sha}",
                file=sys.stderr,
            )
            sys.exit(1)
        print(f"  Verified {deb_name}: sha256={actual_sha} (OK)")

    # Also save the full checksum file in output_dir for auditing
    with open(output_dir / "SHA256SUMS", "w", encoding="utf-8") as f:
        for deb_name, _ in targets:
            f.write(f"{expected_shas[deb_name]}  {deb_name}\n")

    print("\nAll Intel NEO deb packages downloaded and verified successfully.")


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser(description="Fetch and verify Intel NEO deb packages.")
    parser.add_argument("--neo-ver", required=True, help="Pinned compute-runtime release tag (e.g. 26.31.39395.13)")
    parser.add_argument("--output-dir", type=Path, default=Path("."), help="Directory to save downloaded debs")
    parser.add_argument("--github-token", default=os.getenv("GITHUB_TOKEN", ""), help="Optional GitHub token for rate limiting")

    args = parser.parse_args()
    token = args.github_token.strip() or None
    resolve_and_fetch(args.neo_ver, args.output_dir, token=token)


if __name__ == "__main__":
    main()

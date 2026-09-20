#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Resolve, download, and verify Intel NEO compute stack packages.

Derives the matched set of intel-opencl-icd, libze-intel-gpu1, libigdgmm12,
intel-igc-core-2, and intel-igc-opencl-2 deb packages from a pinned
intel/compute-runtime release tag. See ADR-1145.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import http.client
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any, NoReturn
from urllib.parse import unquote, urlsplit

HTTP_NOT_FOUND = 404
ASCII_CONTROL_LIMIT = 32
ASCII_DELETE = 127
MAX_METADATA_BYTES = 8 * 1024 * 1024
HTTP_TIMEOUT_SECONDS = 60
ALLOWED_DOWNLOAD_HOSTS = {"api.github.com", "github.com"}


def _validate_https_url(url: str) -> None:
    """Reject credentials, fragments, non-HTTPS schemes, and non-GitHub hosts."""
    parsed = urlsplit(url)
    hostname = parsed.hostname or ""
    github_content = hostname.endswith(".githubusercontent.com")
    if (
        parsed.scheme != "https"
        or (hostname not in ALLOWED_DOWNLOAD_HOSTS and not github_content)
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port not in {None, 443}
        or not parsed.path.startswith("/")
        or parsed.fragment
    ):
        raise ValueError(f"refusing non-GitHub HTTPS URL: {url}")


class _HttpsOnlyRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep urllib redirects inside the same validated HTTPS trust boundary."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: IO[bytes],
        code: int,
        msg: str,
        headers: http.client.HTTPMessage,
        newurl: str,
    ) -> urllib.request.Request | None:
        _validate_https_url(newurl)
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected is not None and urlsplit(req.full_url).hostname != urlsplit(newurl).hostname:
            redirected.remove_header("Authorization")
        return redirected


class _GitHubAuthorizationHandler(urllib.request.BaseHandler):
    """Attach API credentials only to exact api.github.com requests."""

    def __init__(self, token: str | None) -> None:
        self._token = token

    def https_request(self, request: urllib.request.Request) -> urllib.request.Request:
        if self._token and urlsplit(request.full_url).hostname == "api.github.com":
            request.add_unredirected_header("Authorization", f"Bearer {self._token}")
        return request


def _opener(url: str, token: str | None) -> urllib.request.OpenerDirector:
    _validate_https_url(url)
    opener = urllib.request.build_opener(
        _HttpsOnlyRedirectHandler(), _GitHubAuthorizationHandler(token)
    )
    opener.addheaders = [
        ("User-Agent", "vmaf-dev-container-build"),
        ("Accept", "application/vnd.github+json"),
    ]
    return opener


def make_request(url: str, token: str | None = None) -> bytes:
    """Execute an HTTP GET with GitHub API / standard headers and rate-limit handling."""
    try:
        with _opener(url, token).open(url, timeout=HTTP_TIMEOUT_SECONDS) as response:
            body = response.read(MAX_METADATA_BYTES + 1)
            if not isinstance(body, bytes):
                raise ValueError("GitHub response body is not bytes")
            if len(body) > MAX_METADATA_BYTES:
                raise ValueError("GitHub response exceeds the metadata size limit")
            return body
    except urllib.error.HTTPError as err:
        body = ""
        with contextlib.suppress(OSError, UnicodeError, ValueError):
            body = err.read().decode("utf-8", errors="replace")

        if err.code in (403, 429) or "rate limit" in body.lower():
            print(
                f"\nFATAL: GitHub API rate limit exceeded while accessing {url}.\n"
                f"Response: {body}\n"
                "Remedy: export GITHUB_TOKEN and pass it with "
                "--secret id=github_token,env=GITHUB_TOKEN, or wait for rate limit reset.",
                file=sys.stderr,
            )
            sys.exit(1)
        elif err.code == HTTP_NOT_FOUND:
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
    except (http.client.HTTPException, OSError, ValueError) as err:
        print(f"\nFATAL: Network error while accessing {url}: {err}", file=sys.stderr)
        sys.exit(1)


def sha256_file(path: Path) -> str:
    """Compute sha256 hex digest of a file."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def download_file(
    url: str, dest_path: Path, token: str | None = None, max_retries: int = 3
) -> None:
    """Download a GitHub HTTPS asset atomically, retrying transient I/O failures."""
    last_error: Exception | None = None
    for _attempt in range(max_retries + 1):
        temporary_path: Path | None = None
        try:
            with (
                _opener(url, token).open(url, timeout=HTTP_TIMEOUT_SECONDS) as response,
                tempfile.NamedTemporaryFile(
                    dir=dest_path.parent, prefix=f".{dest_path.name}.", delete=False
                ) as temporary,
            ):
                temporary_path = Path(temporary.name)
                shutil.copyfileobj(response, temporary)
                temporary.flush()
                os.fsync(temporary.fileno())
            temporary_path.replace(dest_path)
            return
        except (http.client.HTTPException, OSError, ValueError) as error:
            last_error = error
            if temporary_path is not None:
                with contextlib.suppress(FileNotFoundError):
                    temporary_path.unlink()
    print(f"FATAL: download failed for {url}: {last_error}", file=sys.stderr, flush=True)
    sys.exit(1)


def _fatal(message: str) -> NoReturn:
    print(f"FATAL: {message}", file=sys.stderr)
    raise SystemExit(1)


def _decode_json(raw: bytes, source: str) -> dict[str, Any]:
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        _fatal(f"failed to parse JSON from {source}: {error}")
    if not isinstance(document, dict):
        _fatal(f"JSON from {source} is not an object")
    return document


def _release_assets(document: dict[str, Any], source: str) -> tuple[dict[str, str], str]:
    records = document.get("assets")
    body = document.get("body", "")
    if not isinstance(records, list) or not isinstance(body, str):
        _fatal(f"release metadata from {source} has invalid assets or body fields")
    assets: dict[str, str] = {}
    for record in records:
        if not isinstance(record, dict):
            _fatal(f"release metadata from {source} contains a non-object asset")
        name = record.get("name")
        url = record.get("browser_download_url")
        if not isinstance(name, str) or not isinstance(url, str):
            _fatal(f"release metadata from {source} contains an invalid asset")
        _validate_https_url(url)
        try:
            safe_name = _validate_asset_filename(name)
            url_name = _asset_filename(url)
        except ValueError as error:
            _fatal(f"release metadata from {source} contains an invalid asset: {error}")
        if safe_name != url_name:
            _fatal(
                f"release metadata from {source} names asset {safe_name!r} "
                f"but its URL ends in {url_name!r}"
            )
        assets[name] = url
    return assets, body


def _first_asset(
    assets: dict[str, str], pattern: str, description: str, *, reject: tuple[str, ...] = ()
) -> tuple[str, str]:
    for name, url in assets.items():
        if re.match(pattern, name) and not any(marker in name for marker in reject):
            return name, url
    _fatal(f"no {description} asset found")


def _body_asset(body: str, package: str) -> str | None:
    match = re.search(rf"https://github\.com/[^\s]+/{package}_[^\s]+_amd64\.deb", body)
    if match is None:
        return None
    url = match.group(0)
    _validate_https_url(url)
    return url


def _validate_asset_filename(name: str) -> str:
    has_control_character = any(
        ord(character) < ASCII_CONTROL_LIMIT or ord(character) == ASCII_DELETE for character in name
    )
    if (
        not name
        or name in {".", ".."}
        or Path(name).name != name
        or "\\" in name
        or has_control_character
    ):
        raise ValueError(f"unsafe release asset filename: {name!r}")
    return name


def _asset_filename(url: str) -> str:
    return _validate_asset_filename(unquote(Path(urlsplit(url).path).name))


def _package_version(name: str, pattern: str) -> str:
    match = re.search(pattern, name)
    return match.group(1) if match else "unknown"


@dataclass(frozen=True)
class StackAssets:
    gmm: tuple[str, str]
    icd: tuple[str, str]
    level_zero: tuple[str, str]
    igc_core: tuple[str, str]
    igc_opencl: tuple[str, str]
    checksum: tuple[str, str]

    @property
    def targets(self) -> list[tuple[str, str]]:
        return [self.gmm, self.icd, self.level_zero, self.igc_core, self.igc_opencl]


def _resolve_stack(document: dict[str, Any], neo_ver: str, source: str) -> StackAssets:
    assets, body = _release_assets(document, source)
    gmm = _first_asset(
        assets,
        r"^(intel-igdgmm12|libigdgmm12)_[0-9].*_amd64\.deb$",
        f"gmmlib deb in compute-runtime {neo_ver}",
        reject=(".ddeb",),
    )
    icd = _first_asset(
        assets,
        r"^intel-opencl-icd_[0-9].*_amd64\.deb$",
        f"intel-opencl-icd deb in compute-runtime {neo_ver}",
        reject=(".ddeb", "legacy"),
    )
    level_zero = _first_asset(
        assets,
        r"^(intel-level-zero-gpu|libze-intel-gpu1)_[0-9].*_amd64\.deb$",
        f"libze-intel-gpu1 / level-zero-gpu deb in compute-runtime {neo_ver}",
        reject=(".ddeb", "legacy"),
    )
    checksum = _first_asset(
        assets, r"^.*(?:\.sum|sha256.*)$", f"checksum in compute-runtime {neo_ver}"
    )
    igc_core = _optional_igc_asset(assets, body, "intel-igc-core-2", neo_ver)
    igc_opencl = _optional_igc_asset(assets, body, "intel-igc-opencl-2", neo_ver)
    return StackAssets(gmm, icd, level_zero, igc_core, igc_opencl, checksum)


def _optional_igc_asset(
    assets: dict[str, str], body: str, package: str, neo_ver: str
) -> tuple[str, str]:
    pattern = rf"^{package}_[0-9].*_amd64\.deb$"
    direct = next(((name, url) for name, url in assets.items() if re.match(pattern, name)), None)
    if direct is not None:
        return direct
    url = _body_asset(body, package)
    if url is None:
        _fatal(f"could not resolve {package} for compute-runtime {neo_ver} from assets or body")
    return _asset_filename(url), url


def _print_resolution(neo_ver: str, stack: StackAssets) -> None:
    gmm_version = _package_version(stack.gmm[0], r"_(.+)_amd64\.deb$")
    igc_version = _package_version(stack.igc_core[0], r"intel-igc-core-2_(.+)_amd64\.deb$")
    print("========================================================================")
    print("Intel NEO Compute Stack Resolution (ADR-1145)")
    print(f"Pinned NEO_VER: {neo_ver}")
    print("Resolved packages:")
    print(f"  - intel-opencl-icd: {stack.icd[0]}")
    print(f"  - libze-intel-gpu1: {stack.level_zero[0]}")
    print(f"  - gmmlib:           {stack.gmm[0]} (derived GMMLIB_VER: {gmm_version})")
    print(f"  - intel-igc-core-2: {stack.igc_core[0]} (derived IGC_VER: {igc_version})")
    print(f"  - intel-igc-opencl: {stack.igc_opencl[0]}")
    print(f"  - checksum file:    {stack.checksum[0]}")
    print("========================================================================")


def _record_checksum(checksums: dict[str, str], filename: str, digest: str, source: str) -> None:
    normalized_digest = digest.lower()
    previous_digest = checksums.get(filename)
    if previous_digest is not None and previous_digest != normalized_digest:
        _fatal(f"conflicting checksums for {filename} in {source}")
    checksums[filename] = normalized_digest


def _parse_checksums(raw: bytes, source: str) -> dict[str, str]:
    try:
        text = raw.decode("utf-8")
    except UnicodeError as error:
        _fatal(f"checksum file from {source} is not UTF-8: {error}")
    checksums: dict[str, str] = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([a-fA-F0-9]{64})\s+\*?(\S+)", line.strip())
        if match is not None:
            digest, filename = match.groups()
            _record_checksum(checksums, filename, digest, source)
    return checksums


def _add_igc_checksums(checksums: dict[str, str], stack: StackAssets, token: str | None) -> None:
    if all(name in checksums for name, _url in (stack.igc_core, stack.igc_opencl)):
        return
    tag_match = re.search(r"/releases/download/([^/]+)/", stack.igc_core[1])
    if tag_match is None:
        return
    igc_tag = tag_match.group(1)
    print(f"Fetching IGC checksums from intel-graphics-compiler tag {igc_tag}...")
    source = f"https://api.github.com/repos/intel/intel-graphics-compiler/releases/tags/{igc_tag}"
    document = _decode_json(make_request(source, token=token), source)
    body = document.get("body", "")
    if not isinstance(body, str):
        _fatal(f"IGC release metadata from {source} has an invalid body")
    for sha, filename in re.findall(r"([a-fA-F0-9]{64})\s+([^\s]+\.deb)", body):
        _record_checksum(checksums, filename, sha, source)


def _download_and_verify(
    output_dir: Path,
    targets: list[tuple[str, str]],
    checksums: dict[str, str],
    token: str | None,
) -> None:
    for deb_name, deb_url in targets:
        expected_sha = checksums.get(deb_name)
        if expected_sha is None:
            _fatal(f"checksum for {deb_name} not found in published release hashes")
        target_path = output_dir / deb_name
        print(f"Downloading {deb_name} from {deb_url}...")
        download_file(deb_url, target_path, token=token)
        actual_sha = sha256_file(target_path)
        if actual_sha.lower() != expected_sha.lower():
            target_path.unlink(missing_ok=True)
            _fatal(
                f"checksum mismatch for {deb_name}!\n"
                f"  Expected: {expected_sha}\n  Actual:   {actual_sha}"
            )
        print(f"  Verified {deb_name}: sha256={actual_sha} (OK)")


def _write_checksum_audit(
    output_dir: Path, targets: list[tuple[str, str]], checksums: dict[str, str]
) -> None:
    with (output_dir / "SHA256SUMS").open("w", encoding="utf-8") as stream:
        for deb_name, _url in targets:
            stream.write(f"{checksums[deb_name]}  {deb_name}\n")


def resolve_and_fetch(neo_ver: str, output_dir: Path, token: str | None = None) -> None:
    """Resolve deb URLs, download, and verify against published checksums."""
    output_dir.mkdir(parents=True, exist_ok=True)
    source = f"https://api.github.com/repos/intel/compute-runtime/releases/tags/{neo_ver}"
    print(f"Querying Intel compute-runtime release metadata for {neo_ver}...")
    stack = _resolve_stack(_decode_json(make_request(source, token=token), source), neo_ver, source)
    _print_resolution(neo_ver, stack)
    print(f"Downloading checksum file: {stack.checksum[1]}...")
    checksums = _parse_checksums(make_request(stack.checksum[1], token=token), stack.checksum[1])
    _add_igc_checksums(checksums, stack, token)
    _download_and_verify(output_dir, stack.targets, checksums, token)
    _write_checksum_audit(output_dir, stack.targets, checksums)
    print("\nAll Intel NEO deb packages downloaded and verified successfully.")


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser(description="Fetch and verify Intel NEO deb packages.")
    parser.add_argument(
        "--neo-ver", required=True, help="Pinned compute-runtime release tag (e.g. 26.31.39395.13)"
    )
    parser.add_argument(
        "--output-dir", type=Path, default=Path(), help="Directory to save downloaded debs"
    )
    parser.add_argument(
        "--github-token",
        default=os.getenv("GITHUB_TOKEN", ""),
        help="Optional GitHub token for rate limiting",
    )

    args = parser.parse_args()
    token = args.github_token.strip() or None
    resolve_and_fetch(args.neo_ver, args.output_dir, token=token)


if __name__ == "__main__":
    main()

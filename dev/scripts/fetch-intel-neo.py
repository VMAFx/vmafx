#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Download and verify Intel NEO and Level Zero packages from a pinned lock file.

Default mode reads ``dev/scripts/intel-neo-lock.json`` (or ``--lock PATH``) and
downloads each selected ``.deb`` directly from ``https://github.com/...`` with
zero ``api.github.com`` requests, verifying every download against the locked
SHA-256 digest and writing ``SHA256SUMS`` to ``--output-dir`` (ADR-3555).

``--write-lock PATH`` resolves the matched Intel NEO compute stack (ADR-1145)
and Level Zero loader packages (ADR-1368) via the GitHub Releases API, records
upstream SHA-256 digests without downloading ``.deb`` archives, and writes the
sorted lock file. The payload is validated before the lock is replaced.

Lock file schema (``dev/scripts/intel-neo-lock.json``):
  - ``neo_version`` (str): pinned ``intel/compute-runtime`` release tag.
  - ``level_zero_version`` (str): pinned ``oneapi-src/level-zero`` version.
  - ``level_zero_distro`` (str): Ubuntu build suffix (``"u24.04"``).
  - ``neo`` (object): exact keys ``gmm``, ``icd``, ``level_zero``, ``igc_core``,
    ``igc_opencl``, ``ocloc`` mapped to asset records (matching ``StackAssets``).
  - ``level_zero`` (object): exact keys ``libze1``, ``libze-dev`` mapped to
    asset records (matching ``level_zero_packages``).
  - Each asset record is ``{"name": str, "sha256": str, "url": str}``, where
    ``url`` is ``https://github.com/<owner>/<repo>/releases/download/<tag>/<name>``
    and ``sha256`` is 64 lowercase hexadecimal characters.
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
from urllib.parse import SplitResult, unquote, urlsplit

HTTP_NOT_FOUND = 404
ASCII_CONTROL_LIMIT = 32
ASCII_DELETE = 127
MAX_METADATA_BYTES = 8 * 1024 * 1024
MAX_LOCK_BYTES = 1024 * 1024
HTTP_TIMEOUT_SECONDS = 60
RELEASE_DOWNLOAD_PATH_SEGMENTS = 7
ALLOWED_DOWNLOAD_HOSTS = {"api.github.com", "github.com"}
DEFAULT_LOCK_PATH = Path(__file__).with_name("intel-neo-lock.json")
REFRESH_LOCK_HINT = (
    "Refresh with: bash -c '. ./build-config.env && python3 dev/scripts/fetch-intel-neo.py "
    '--neo-ver "$INTEL_NEO_VERSION" --level-zero-ver "$LEVEL_ZERO_VERSION" '
    "--write-lock dev/scripts/intel-neo-lock.json'"
)


def _is_release_download_path(path: str) -> bool:
    parts = path.split("/")
    return (
        len(parts) == RELEASE_DOWNLOAD_PATH_SEGMENTS
        and parts[0] == ""
        and parts[3:5] == ["releases", "download"]
        and all(part and part not in {".", ".."} for part in parts[1:])
    )


def _has_unsafe_url_parts(parsed: SplitResult) -> bool:
    return (
        parsed.scheme != "https"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port not in {None, 443}
        or bool(parsed.fragment)
    )


def _validate_https_url(url: str, *, release_asset_only: bool = False) -> None:
    """Reject credentials, fragments, non-HTTPS schemes, and non-GitHub hosts."""
    parsed = urlsplit(url)
    hostname = parsed.hostname if parsed.hostname is not None else ""
    if _has_unsafe_url_parts(parsed):
        raise ValueError(f"refusing non-GitHub HTTPS URL: {url}")
    if release_asset_only:
        if hostname != "github.com" or parsed.query or not _is_release_download_path(parsed.path):
            raise ValueError(f"refusing non-GitHub release asset URL: {url}")
        return
    github_host = hostname in ALLOWED_DOWNLOAD_HOSTS or hostname.endswith(".githubusercontent.com")
    if not github_host or not parsed.path.startswith("/"):
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
            _fatal(
                f"GitHub API rate limit exceeded while accessing {url}.\n"
                f"Response: {body}\n"
                "Remedy: export GITHUB_TOKEN and pass it with "
                "--secret id=github_token,env=GITHUB_TOKEN, or wait for rate limit reset.",
                leading_newline=True,
            )
        elif err.code == HTTP_NOT_FOUND:
            _fatal(
                f"Resource not found (HTTP 404): {url}\nResponse: {body}",
                leading_newline=True,
            )
        else:
            _fatal(
                f"HTTP error {err.code} while accessing {url}: {err.reason}\nResponse: {body}",
                leading_newline=True,
            )
    except (http.client.HTTPException, OSError, ValueError) as err:
        _fatal(f"Network error while accessing {url}: {err}", leading_newline=True)


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
    _fatal(f"download failed for {url}: {last_error}")


def _fatal(message: str, *, leading_newline: bool = False) -> NoReturn:
    """Report ``message`` on stderr and end the run with status 1."""
    prefix = "\n" if leading_newline else ""
    print(f"{prefix}FATAL: {message}", file=sys.stderr, flush=True)
    raise SystemExit(1)


def _lock_fatal(lock_path: Path, message: str) -> NoReturn:
    """Report an invalid or stale lock file naming both the path and refresh command."""
    _fatal(f"lock file {lock_path}: {message}\n{REFRESH_LOCK_HINT}")


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


COMPONENTS = ("runtime", "ocloc")

# The Level Zero release builds its debs per Ubuntu release. The u24.04 build
# needs glibc 2.39, so it also installs on Debian 13 (glibc 2.41) and Ubuntu
# 26.04, the two bases that install it; dev/Containerfile uses the same build.
LEVEL_ZERO_DISTRO = "u24.04"
SHA256_DIGEST = re.compile(r"sha256:([0-9a-f]{64})")
LOCK_SHA256 = re.compile(r"[0-9a-f]{64}")
LOCK_NEO_ROLES = ("gmm", "icd", "level_zero", "igc_core", "igc_opencl", "ocloc")
LOCK_LEVEL_ZERO_PACKAGES = ("libze1", "libze-dev")
LOCK_TOP_KEYS = frozenset(
    {"neo_version", "level_zero_version", "level_zero_distro", "neo", "level_zero"}
)
LOCK_ASSET_KEYS = frozenset({"name", "url", "sha256"})


@dataclass(frozen=True)
class StackAssets:
    gmm: tuple[str, str]
    icd: tuple[str, str]
    level_zero: tuple[str, str]
    igc_core: tuple[str, str]
    igc_opencl: tuple[str, str]
    ocloc: tuple[str, str]
    checksum: tuple[str, str]

    @property
    def targets(self) -> list[tuple[str, str]]:
        return self.select("runtime")

    def select(self, components: str) -> list[tuple[str, str]]:
        """Return the packages one component set installs.

        ocloc declares no package dependency on IGC but loads libigc and
        libigdfcl at run time, so both IGC packages travel with it.
        """
        compiler = [self.igc_core, self.igc_opencl, self.ocloc]
        if components == "ocloc":
            return compiler
        if components == "runtime":
            return [self.gmm, self.icd, self.level_zero, *compiler]
        _fatal(f"unknown component set {components!r}; expected one of {COMPONENTS}")


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
    ocloc = _first_asset(
        assets,
        r"^intel-ocloc_[0-9].*_amd64\.deb$",
        f"intel-ocloc deb in compute-runtime {neo_ver}",
        reject=(".ddeb", "legacy"),
    )
    checksum = _first_asset(
        assets, r"^.*(?:\.sum|sha256.*)$", f"checksum in compute-runtime {neo_ver}"
    )
    igc_core = _optional_igc_asset(assets, body, "intel-igc-core-2", neo_ver)
    igc_opencl = _optional_igc_asset(assets, body, "intel-igc-opencl-2", neo_ver)
    return StackAssets(gmm, icd, level_zero, igc_core, igc_opencl, ocloc, checksum)


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
    print(f"  - intel-ocloc:      {stack.ocloc[0]}")
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


def level_zero_packages(dev: bool) -> tuple[str, ...]:
    """Return the Level Zero loader packages to fetch: runtime, plus headers when asked."""
    return ("libze1", "libze-dev") if dev else ("libze1",)


def _resolve_level_zero(
    document: dict[str, Any], version: str, packages: tuple[str, ...], source: str
) -> tuple[list[tuple[str, str]], dict[str, str]]:
    """Pick the loader debs of one level-zero release and the digests GitHub records for them."""
    records = document.get("assets")
    if not isinstance(records, list):
        _fatal(f"release metadata from {source} has an invalid assets field")
    by_name = {
        record.get("name"): record
        for record in records
        if isinstance(record, dict) and isinstance(record.get("name"), str)
    }
    targets: list[tuple[str, str]] = []
    checksums: dict[str, str] = {}
    for package in packages:
        name = f"{package}_{version}+{LEVEL_ZERO_DISTRO}_amd64.deb"
        record = by_name.get(name)
        if record is None:
            _fatal(f"no {name} asset in level-zero release {version}")
        url = record.get("browser_download_url")
        digest = record.get("digest")
        if not isinstance(url, str) or not isinstance(digest, str):
            _fatal(f"release metadata from {source} gives {name} no URL or no digest")
        match = SHA256_DIGEST.fullmatch(digest)
        if match is None:
            _fatal(f"release metadata from {source} gives {name} a non-SHA-256 digest {digest!r}")
        _validate_https_url(url)
        try:
            url_name = _asset_filename(url)
        except ValueError as error:
            _fatal(f"release metadata from {source} contains an invalid asset: {error}")
        if url_name != name:
            _fatal(
                f"release metadata from {source} names {name!r} but its URL ends in {url_name!r}"
            )
        targets.append((name, url))
        _record_checksum(checksums, name, match.group(1), source)
    return targets, checksums


@dataclass(frozen=True)
class LockedStack:
    neo_version: str
    level_zero_version: str
    level_zero_distro: str
    stack: StackAssets
    level_zero_assets: dict[str, tuple[str, str]]
    checksums: dict[str, str]


def _no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _read_lock_json(lock_path: Path) -> dict[str, Any]:
    try:
        with lock_path.open("rb") as stream:
            raw = stream.read(MAX_LOCK_BYTES + 1)
    except OSError as error:
        _lock_fatal(lock_path, f"cannot read lock file ({error})")
    if len(raw) > MAX_LOCK_BYTES:
        _lock_fatal(lock_path, "lock file exceeds size limit")
    try:
        document = json.loads(raw.decode("utf-8"), object_pairs_hook=_no_duplicate_keys)
    except (UnicodeError, json.JSONDecodeError, ValueError) as error:
        _lock_fatal(lock_path, f"invalid JSON ({error})")
    if not isinstance(document, dict):
        _lock_fatal(lock_path, "top-level JSON value must be an object")
    return document


def _check_exact_keys(
    mapping: dict[str, Any], expected: frozenset[str], context: str, lock_path: Path
) -> None:
    unknown = sorted(set(mapping) - expected)
    if unknown:
        _lock_fatal(lock_path, f"unknown key(s) in {context}: {', '.join(unknown)}")
    missing = sorted(expected - set(mapping))
    if missing:
        _lock_fatal(lock_path, f"missing key(s) in {context}: {', '.join(missing)}")


def _parse_lock_asset(raw: Any, context: str, lock_path: Path) -> tuple[tuple[str, str], str]:
    if not isinstance(raw, dict):
        _lock_fatal(lock_path, f"{context} must be an object")
    _check_exact_keys(raw, LOCK_ASSET_KEYS, context, lock_path)
    name, url, sha256 = raw["name"], raw["url"], raw["sha256"]
    if not isinstance(name, str) or not isinstance(url, str) or not isinstance(sha256, str):
        _lock_fatal(lock_path, f"{context} fields must be strings")
    if LOCK_SHA256.fullmatch(sha256) is None:
        _lock_fatal(lock_path, f"{context} has invalid sha256 digest {sha256!r}")
    try:
        _validate_https_url(url, release_asset_only=True)
        safe_name = _validate_asset_filename(name)
        url_name = _asset_filename(url)
    except ValueError as error:
        _lock_fatal(lock_path, f"{context} has invalid URL or filename ({error})")
    if safe_name != url_name:
        _lock_fatal(lock_path, f"{context} name {safe_name!r} does not match URL {url_name!r}")
    return (safe_name, url), sha256


def _parse_lock_group(
    raw_group: Any,
    expected_keys: tuple[str, ...],
    group_name: str,
    lock_path: Path,
    checksums: dict[str, str],
) -> dict[str, tuple[str, str]]:
    if not isinstance(raw_group, dict):
        _lock_fatal(lock_path, f"{group_name} must be an object")
    _check_exact_keys(raw_group, frozenset(expected_keys), group_name, lock_path)
    parsed: dict[str, tuple[str, str]] = {}
    for key in expected_keys:
        asset, digest = _parse_lock_asset(raw_group[key], f"{group_name}.{key}", lock_path)
        _record_checksum(checksums, asset[0], digest, str(lock_path))
        parsed[key] = asset
    return parsed


def _parse_lock_document(document: dict[str, Any], lock_path: Path) -> LockedStack:
    _check_exact_keys(document, LOCK_TOP_KEYS, "top-level object", lock_path)
    neo_ver = document["neo_version"]
    lz_ver = document["level_zero_version"]
    lz_distro = document["level_zero_distro"]
    if not all(isinstance(v, str) and v for v in (neo_ver, lz_ver, lz_distro)):
        _lock_fatal(lock_path, "version and distro fields must be non-empty strings")
    if lz_distro != LEVEL_ZERO_DISTRO:
        _lock_fatal(
            lock_path,
            f"level_zero_distro {lz_distro!r} does not match expected {LEVEL_ZERO_DISTRO!r}",
        )
    checksums: dict[str, str] = {}
    neo_map = _parse_lock_group(document["neo"], LOCK_NEO_ROLES, "neo", lock_path, checksums)
    lz_map = _parse_lock_group(
        document["level_zero"], LOCK_LEVEL_ZERO_PACKAGES, "level_zero", lock_path, checksums
    )
    for package, (asset_name, _url) in lz_map.items():
        expected_name = f"{package}_{lz_ver}+{lz_distro}_amd64.deb"
        if asset_name != expected_name:
            _lock_fatal(
                lock_path, f"level_zero.{package} names {asset_name!r}, expected {expected_name!r}"
            )
    stack = StackAssets(
        gmm=neo_map["gmm"],
        icd=neo_map["icd"],
        level_zero=neo_map["level_zero"],
        igc_core=neo_map["igc_core"],
        igc_opencl=neo_map["igc_opencl"],
        ocloc=neo_map["ocloc"],
        checksum=(lock_path.name, ""),
    )
    return LockedStack(neo_ver, lz_ver, lz_distro, stack, lz_map, checksums)


def load_lock(lock_path: Path) -> LockedStack:
    """Read and validate ``lock_path`` without performing any network request."""
    return _parse_lock_document(_read_lock_json(lock_path), lock_path)


def fetch_from_lock(
    lock_path: Path,
    neo_ver: str,
    output_dir: Path,
    components: str = "runtime",
    level_zero_ver: str | None = None,
    level_zero_dev: bool = False,
) -> None:
    """Download and verify selected packages from ``lock_path`` without API calls."""
    locked = load_lock(lock_path)
    if locked.neo_version != neo_ver:
        _lock_fatal(
            lock_path, f"neo_version {locked.neo_version!r} does not match --neo-ver {neo_ver!r}"
        )
    if level_zero_ver is not None and locked.level_zero_version != level_zero_ver:
        _lock_fatal(
            lock_path,
            f"level_zero_version {locked.level_zero_version!r} does not match "
            f"--level-zero-ver {level_zero_ver!r}",
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    _print_resolution(neo_ver, locked.stack)
    targets = list(locked.stack.select(components))
    if level_zero_ver is not None:
        targets.extend(locked.level_zero_assets[pkg] for pkg in level_zero_packages(level_zero_dev))
    _download_and_verify(output_dir, targets, locked.checksums, token=None)
    _write_checksum_audit(output_dir, targets, locked.checksums)
    print(f"\nIntel NEO '{components}' deb packages downloaded and verified from {lock_path}.")


def _lock_entry(asset: tuple[str, str], checksums: dict[str, str]) -> dict[str, str]:
    name, url = asset
    digest = checksums.get(name)
    if digest is None:
        _fatal(f"checksum for {name} not found in published release hashes")
    return {"name": name, "sha256": digest, "url": url}


def _atomic_write_json(path: Path, serialized: str) -> None:
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(serialized)
            temporary.flush()
            os.fsync(temporary.fileno())
        temporary_path.replace(path)
    except OSError as error:
        if temporary_path is not None:
            with contextlib.suppress(FileNotFoundError):
                temporary_path.unlink()
        _fatal(f"failed to write lock file {path}: {error}")


def write_lock(
    lock_path: Path, neo_ver: str, level_zero_ver: str, token: str | None = None
) -> None:
    """Resolve NEO and Level Zero releases via GitHub API and write ``lock_path``."""
    neo_source = f"https://api.github.com/repos/intel/compute-runtime/releases/tags/{neo_ver}"
    print(f"Querying Intel compute-runtime release metadata for {neo_ver}...")
    stack = _resolve_stack(
        _decode_json(make_request(neo_source, token=token), neo_source), neo_ver, neo_source
    )
    _print_resolution(neo_ver, stack)
    print(f"Downloading checksum file: {stack.checksum[1]}...")
    checksums = _parse_checksums(make_request(stack.checksum[1], token=token), stack.checksum[1])
    _add_igc_checksums(checksums, stack, token)
    lz_source = (
        f"https://api.github.com/repos/oneapi-src/level-zero/releases/tags/v{level_zero_ver}"
    )
    print(f"Querying Level Zero loader release metadata for v{level_zero_ver}...")
    lz_targets, lz_checksums = _resolve_level_zero(
        _decode_json(make_request(lz_source, token=token), lz_source),
        level_zero_ver,
        level_zero_packages(dev=True),
        lz_source,
    )
    for name, digest in lz_checksums.items():
        _record_checksum(checksums, name, digest, lz_source)
    neo_entries = {role: _lock_entry(getattr(stack, role), checksums) for role in LOCK_NEO_ROLES}
    lz_entries = {
        pkg: _lock_entry(asset, checksums)
        for pkg, asset in zip(LOCK_LEVEL_ZERO_PACKAGES, lz_targets, strict=True)
    }
    payload: dict[str, Any] = {
        "level_zero": lz_entries,
        "level_zero_distro": LEVEL_ZERO_DISTRO,
        "level_zero_version": level_zero_ver,
        "neo": neo_entries,
        "neo_version": neo_ver,
    }
    serialized = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if len(serialized.encode("utf-8")) > MAX_LOCK_BYTES:
        _lock_fatal(lock_path, "lock file exceeds size limit")
    parsed = json.loads(serialized, object_pairs_hook=_no_duplicate_keys)
    _parse_lock_document(parsed, lock_path)
    _atomic_write_json(lock_path, serialized)
    load_lock(lock_path)
    print(f"\nWrote verified Intel NEO and Level Zero lock file to {lock_path}.")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fetch and verify Intel NEO deb packages.")
    parser.add_argument(
        "--neo-ver",
        required=True,
        help="Pinned intel/compute-runtime release tag (e.g. 26.35.39758.10)",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=Path(), help="Directory to save downloaded debs"
    )
    parser.add_argument(
        "--github-token",
        default=os.getenv("GITHUB_TOKEN", ""),
        help="Optional GitHub token for --write-lock API lookups (ignored in lock mode)",
    )
    parser.add_argument(
        "--components",
        choices=COMPONENTS,
        default="runtime",
        help=(
            "runtime: GPU runtime plus ocloc (a host that runs SYCL kernels); "
            "ocloc: the offline compiler and its IGC libraries only (an AOT build host)"
        ),
    )
    parser.add_argument(
        "--level-zero-ver",
        default="",
        help="Also fetch the Level Zero loader (libze1) from this oneapi-src/level-zero release",
    )
    parser.add_argument(
        "--level-zero-dev",
        action="store_true",
        help="With --level-zero-ver, also fetch the loader's headers and link library (libze-dev)",
    )
    lock_group = parser.add_mutually_exclusive_group()
    lock_group.add_argument(
        "--lock",
        type=Path,
        default=None,
        help=f"Read pinned asset URLs and SHA-256 sums from PATH (default: {DEFAULT_LOCK_PATH})",
    )
    lock_group.add_argument(
        "--write-lock",
        type=Path,
        default=None,
        help="Resolve release metadata via GitHub API and write the sorted lock JSON to PATH",
    )
    return parser


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(line_buffering=True)

    parser = _build_parser()
    args = parser.parse_args()
    level_zero_ver = args.level_zero_ver.strip() or None
    if args.level_zero_dev and not level_zero_ver:
        parser.error("--level-zero-dev needs --level-zero-ver")
    if args.write_lock is not None:
        if level_zero_ver is None:
            parser.error("--write-lock needs --level-zero-ver")
        token = args.github_token.strip() or None
        write_lock(args.write_lock, args.neo_ver, level_zero_ver, token=token)
        return

    lock_path = args.lock if args.lock is not None else DEFAULT_LOCK_PATH
    fetch_from_lock(
        lock_path,
        args.neo_ver,
        args.output_dir,
        components=args.components,
        level_zero_ver=level_zero_ver,
        level_zero_dev=args.level_zero_dev,
    )


if __name__ == "__main__":
    main()

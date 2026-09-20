# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Hermetic tests for the Intel NEO release fetcher."""

from __future__ import annotations

import importlib.util
import io
import json
import sys
from email.message import Message
from http.client import IncompleteRead
from pathlib import Path
from typing import BinaryIO
from urllib.request import Request

import pytest

_SCRIPT_PATH = Path(__file__).with_name("fetch-intel-neo.py")
_SPEC = importlib.util.spec_from_file_location("fetch_intel_neo", _SCRIPT_PATH)
assert _SPEC is not None and _SPEC.loader is not None
fetch_intel_neo = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = fetch_intel_neo
_SPEC.loader.exec_module(fetch_intel_neo)


def _release_asset(name: str) -> dict[str, str]:
    return {
        "name": name,
        "browser_download_url": (
            "https://github.com/intel/compute-runtime/releases/download/neo/" + name
        ),
    }


class _InterruptedResponse(io.BytesIO):
    def read(self, _size: int | None = -1) -> bytes:
        raise IncompleteRead(b"partial", 10)


class _FakeOpener:
    def __init__(self, responses: list[BinaryIO]) -> None:
        self._responses = iter(responses)

    def open(self, _url: str, *, timeout: int) -> BinaryIO:
        assert timeout == fetch_intel_neo.HTTP_TIMEOUT_SECONDS
        return next(self._responses)


def test_url_boundary_accepts_expected_github_hosts() -> None:
    urls = [
        "https://api.github.com/repos/intel/compute-runtime/releases/latest",
        "https://github.com/intel/compute-runtime/releases/download/v1/package.deb",
        "https://release-assets.githubusercontent.com/file?sp=read&sig=signed",
    ]

    for url in urls:
        fetch_intel_neo._validate_https_url(url)


def test_url_boundary_rejects_untrusted_or_ambiguous_urls() -> None:
    urls = [
        "http://github.com/intel/package.deb",
        "https://example.com/package.deb",
        "https://api.github.com.example.com/package.deb",
        "https://token@github.com/package.deb",
        "https://github.com:444/package.deb",
        "https://github.com/package.deb#fragment",
        "https://github.com",
    ]

    for url in urls:
        with pytest.raises(ValueError):
            fetch_intel_neo._validate_https_url(url)


def test_authorization_handler_only_adds_token_to_exact_api_host() -> None:
    handler = fetch_intel_neo._GitHubAuthorizationHandler("secret")
    api_request = Request("https://api.github.com/repos/intel/compute-runtime")
    asset_request = Request("https://github.com/intel/compute-runtime/releases/package.deb")

    handler.https_request(api_request)
    handler.https_request(asset_request)

    assert api_request.get_header("Authorization") == "Bearer secret"
    assert asset_request.get_header("Authorization") is None


def test_redirect_does_not_forward_api_token_to_asset_host() -> None:
    """A GitHub API credential must not cross into a release-asset request."""
    request = Request(
        "https://api.github.com/repos/intel/compute-runtime/releases/latest",
        headers={"Authorization": "Bearer secret"},
    )

    redirected = fetch_intel_neo._HttpsOnlyRedirectHandler().redirect_request(
        request,
        io.BytesIO(),
        302,
        "Found",
        Message(),
        "https://release-assets.githubusercontent.com/github-production-release-asset/file.deb",
    )

    assert redirected is not None
    assert redirected.get_header("Authorization") is None


def test_redirect_rejects_url_outside_github_https_boundary() -> None:
    request = Request("https://github.com/intel/compute-runtime/releases/latest")

    with pytest.raises(ValueError, match="refusing non-GitHub HTTPS URL"):
        fetch_intel_neo._HttpsOnlyRedirectHandler().redirect_request(
            request,
            io.BytesIO(),
            302,
            "Found",
            Message(),
            "https://example.com/untrusted.deb",
        )


def test_asset_filename_rejects_percent_encoded_path_separator_or_nul() -> None:
    base = "https://github.com/intel/intel-graphics-compiler/releases/download/v1/"
    filenames = [
        "intel-igc-core-2_1%2Fescape_amd64.deb",
        "intel-igc-core-2_1%00escape_amd64.deb",
    ]

    for filename in filenames:
        with pytest.raises(ValueError, match="unsafe release asset filename"):
            fetch_intel_neo._asset_filename(base + filename)


def test_release_metadata_rejects_unsafe_asset_name() -> None:
    document = {
        "assets": [
            {
                "name": "../sha256.sum",
                "browser_download_url": (
                    "https://github.com/intel/compute-runtime/releases/download/v1/sha256.sum"
                ),
            }
        ],
        "body": "",
    }

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._release_assets(document, "fixture release")


def test_stack_resolution_uses_matched_igc_assets_from_release_body() -> None:
    gmm = "libigdgmm12_22.10.0_amd64.deb"
    icd = "intel-opencl-icd_26.31.39395.13_amd64.deb"
    level_zero = "libze-intel-gpu1_26.31.39395.13_amd64.deb"
    checksum = "ww31.sum"
    igc_core = "intel-igc-core-2_2.40.13+22418_amd64.deb"
    igc_opencl = "intel-igc-opencl-2_2.40.13+22418_amd64.deb"
    igc_base = "https://github.com/intel/intel-graphics-compiler/releases/download/v2.40.13/"
    document = {
        "assets": [_release_asset(name) for name in (gmm, icd, level_zero, checksum)],
        "body": (
            f"{igc_base}{igc_core.replace('+', '%2B')}\n"
            f"{igc_base}{igc_opencl.replace('+', '%2B')}\n"
        ),
    }

    stack = fetch_intel_neo._resolve_stack(document, "neo", "fixture release")

    assert [name for name, _url in stack.targets] == [
        gmm,
        icd,
        level_zero,
        igc_core,
        igc_opencl,
    ]
    assert stack.checksum[0] == checksum


def test_download_retries_interrupted_stream_without_exposing_partial_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    destination = tmp_path / "package.deb"
    destination.write_bytes(b"previous-good-content")
    opener = _FakeOpener([_InterruptedResponse(), io.BytesIO(b"complete-package")])
    monkeypatch.setattr(fetch_intel_neo, "_opener", lambda _url, _token: opener)

    fetch_intel_neo.download_file(
        "https://github.com/intel/compute-runtime/releases/download/v1/package.deb",
        destination,
        max_retries=1,
    )

    assert destination.read_bytes() == b"complete-package"
    assert list(tmp_path.glob(".package.deb.*")) == []


def test_download_failure_preserves_existing_file_and_cleans_temporaries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    destination = tmp_path / "package.deb"
    destination.write_bytes(b"previous-good-content")
    opener = _FakeOpener([_InterruptedResponse(), _InterruptedResponse()])
    monkeypatch.setattr(fetch_intel_neo, "_opener", lambda _url, _token: opener)

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo.download_file(
            "https://github.com/intel/compute-runtime/releases/download/v1/package.deb",
            destination,
            max_retries=1,
        )

    assert destination.read_bytes() == b"previous-good-content"
    assert list(tmp_path.glob(".package.deb.*")) == []


def test_checksum_parser_accepts_sha256sum_binary_marker_and_rejects_bad_digest() -> None:
    good_digest = "a" * 64
    raw = f"{'z' * 64}  invalid.deb\n{good_digest} *package.deb\n".encode()

    checksums = fetch_intel_neo._parse_checksums(raw, "fixture.sum")

    assert checksums == {"package.deb": good_digest}


def test_checksum_parser_rejects_conflicting_duplicate() -> None:
    raw = f"{'a' * 64}  package.deb\n{'b' * 64}  package.deb\n".encode()

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._parse_checksums(raw, "fixture.sum")


def test_igc_checksum_merge_rejects_conflict(monkeypatch: pytest.MonkeyPatch) -> None:
    core_name = "intel-igc-core-2_1_amd64.deb"
    opencl_name = "intel-igc-opencl-2_1_amd64.deb"
    release_url = "https://github.com/intel/intel-graphics-compiler/releases/download/v1/"
    stack = fetch_intel_neo.StackAssets(
        ("gmm.deb", "https://github.com/intel/gmm.deb"),
        ("icd.deb", "https://github.com/intel/icd.deb"),
        ("level-zero.deb", "https://github.com/intel/level-zero.deb"),
        (core_name, release_url + core_name),
        (opencl_name, release_url + opencl_name),
        ("checksums.sum", "https://github.com/intel/checksums.sum"),
    )
    release = {
        "body": f"{'b' * 64}  {core_name}\n{'c' * 64}  {opencl_name}\n",
    }
    monkeypatch.setattr(
        fetch_intel_neo,
        "make_request",
        lambda _url, token=None: json.dumps(release).encode(),
    )

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._add_igc_checksums({core_name: "a" * 64}, stack, None)


def test_checksum_mismatch_removes_untrusted_download(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    package_name = "package.deb"
    package_path = tmp_path / package_name

    def write_corrupt_download(_url: str, destination: Path, **_kwargs: object) -> None:
        destination.write_bytes(b"corrupt")

    monkeypatch.setattr(fetch_intel_neo, "download_file", write_corrupt_download)

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._download_and_verify(
            tmp_path,
            [(package_name, "https://github.com/intel/project/releases/download/v1/package.deb")],
            {package_name: "a" * 64},
            None,
        )

    assert not package_path.exists()

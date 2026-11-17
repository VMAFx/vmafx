# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Hermetic tests for the Intel NEO release fetcher."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sys
from email.message import Message
from http.client import IncompleteRead
from pathlib import Path
from typing import Any, BinaryIO
from urllib.parse import unquote, urlsplit
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
    ocloc = "intel-ocloc_26.31.39395.13_amd64.deb"
    checksum = "ww31.sum"
    igc_core = "intel-igc-core-2_2.40.13+22418_amd64.deb"
    igc_opencl = "intel-igc-opencl-2_2.40.13+22418_amd64.deb"
    igc_base = "https://github.com/intel/intel-graphics-compiler/releases/download/v2.40.13/"
    release_names = (gmm, icd, level_zero, "intel-ocloc-dbgsym_26.31.39395.13_amd64.ddeb", ocloc)
    document = {
        "assets": [_release_asset(name) for name in (*release_names, checksum)],
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
        ocloc,
    ]
    assert [name for name, _url in stack.select("ocloc")] == [igc_core, igc_opencl, ocloc]
    assert stack.checksum[0] == checksum


def test_unknown_component_set_is_fatal() -> None:
    asset = ("x.deb", "https://github.com/intel/x.deb")
    stack = fetch_intel_neo.StackAssets(asset, asset, asset, asset, asset, asset, asset)

    with pytest.raises(SystemExit, match="1"):
        stack.select("everything")


def test_release_without_ocloc_is_fatal() -> None:
    names = (
        "libigdgmm12_22.10.0_amd64.deb",
        "intel-opencl-icd_26.31.39395.13_amd64.deb",
        "libze-intel-gpu1_26.31.39395.13_amd64.deb",
        "intel-ocloc-dbgsym_26.31.39395.13_amd64.ddeb",
        "ww31.sum",
    )
    document = {"assets": [_release_asset(name) for name in names], "body": ""}

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._resolve_stack(document, "neo", "fixture release")


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
        ("ocloc.deb", "https://github.com/intel/ocloc.deb"),
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


def _level_zero_asset(name: str, digest: str = "sha256:" + "d" * 64) -> dict[str, str]:
    return {
        "name": name,
        "browser_download_url": (
            "https://github.com/oneapi-src/level-zero/releases/download/v1.34.0/"
            + name.replace("+", "%2B")
        ),
        "digest": digest,
    }


def test_level_zero_resolution_takes_loader_debs_and_github_digests() -> None:
    names = (
        "libze1_1.34.0+u22.04_amd64.deb",
        "libze1_1.34.0+u24.04_amd64.deb",
        "libze1_1.34.0+u24.04_amd64.deb.sig",
        "libze-dev_1.34.0+u24.04_amd64.deb",
        "level-zero-win-sdk-1.34.0.zip",
    )
    document = {"assets": [_level_zero_asset(name) for name in names]}

    runtime, runtime_sums = fetch_intel_neo._resolve_level_zero(
        document, "1.34.0", fetch_intel_neo.level_zero_packages(dev=False), "fixture"
    )
    build, build_sums = fetch_intel_neo._resolve_level_zero(
        document, "1.34.0", fetch_intel_neo.level_zero_packages(dev=True), "fixture"
    )

    assert [name for name, _url in runtime] == ["libze1_1.34.0+u24.04_amd64.deb"]
    assert runtime_sums == {"libze1_1.34.0+u24.04_amd64.deb": "d" * 64}
    assert [name for name, _url in build] == [
        "libze1_1.34.0+u24.04_amd64.deb",
        "libze-dev_1.34.0+u24.04_amd64.deb",
    ]
    assert set(build_sums) == {name for name, _url in build}


def test_level_zero_release_without_the_loader_deb_is_fatal() -> None:
    document = {"assets": [_level_zero_asset("libze1_1.34.0+u22.04_amd64.deb")]}

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._resolve_level_zero(document, "1.34.0", ("libze1",), "fixture")


def test_level_zero_asset_without_a_sha256_digest_is_fatal() -> None:
    name = "libze1_1.34.0+u24.04_amd64.deb"
    for digest in ("", "md5:" + "d" * 32, "sha256:" + "d" * 63, "sha256:" + "D" * 64):
        document = {"assets": [_level_zero_asset(name, digest)]}
        with pytest.raises(SystemExit, match="1"):
            fetch_intel_neo._resolve_level_zero(document, "1.34.0", ("libze1",), "fixture")


def test_level_zero_asset_url_must_name_the_same_file() -> None:
    record = _level_zero_asset("libze1_1.34.0+u24.04_amd64.deb")
    record["browser_download_url"] = (
        "https://github.com/oneapi-src/level-zero/releases/download/v1.34.0/other.deb"
    )

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo._resolve_level_zero({"assets": [record]}, "1.34.0", ("libze1",), "fixture")


def test_level_zero_dev_without_a_version_is_rejected(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(sys, "argv", ["fetch-intel-neo.py", "--neo-ver", "1", "--level-zero-dev"])

    with pytest.raises(SystemExit, match="2"):
        fetch_intel_neo.main()


_FIXTURE_NEO_VER = "26.35.39758.10"
_FIXTURE_LZ_VER = "1.34.0"


def _fixture_deb_bytes(filename: str) -> bytes:
    return f"deb-payload:{filename}\n".encode("utf-8")


def _fixture_sha256(filename: str) -> str:
    return hashlib.sha256(_fixture_deb_bytes(filename)).hexdigest()


def _fixture_api_responses(
    neo_ver: str = _FIXTURE_NEO_VER, lz_ver: str = _FIXTURE_LZ_VER
) -> tuple[dict[str, bytes], dict[str, Any]]:
    neo_base = f"https://github.com/intel/compute-runtime/releases/download/{neo_ver}/"
    igc_base = "https://github.com/intel/intel-graphics-compiler/releases/download/v2.40.13/"
    gmm = "libigdgmm12_22.10.0_amd64.deb"
    icd = f"intel-opencl-icd_{neo_ver}_amd64.deb"
    lz_gpu = f"libze-intel-gpu1_{neo_ver}_amd64.deb"
    ocloc = f"intel-ocloc_{neo_ver}_amd64.deb"
    igc_core = "intel-igc-core-2_2.40.13+22418_amd64.deb"
    igc_opencl = "intel-igc-opencl-2_2.40.13+22418_amd64.deb"
    sum_name = "ww35.sum"
    neo_doc = {
        "assets": [
            {"name": n, "browser_download_url": neo_base + n}
            for n in (gmm, icd, lz_gpu, ocloc, sum_name)
        ],
        "body": (
            f"{igc_base}{igc_core.replace('+', '%2B')}\n"
            f"{igc_base}{igc_opencl.replace('+', '%2B')}\n"
        ),
    }
    sum_body = "\n".join(f"{_fixture_sha256(n)}  {n}" for n in (gmm, icd, lz_gpu, ocloc)) + "\n"
    igc_doc = {"body": "\n".join(f"{_fixture_sha256(n)}  {n}" for n in (igc_core, igc_opencl))}
    lz_names = (f"libze1_{lz_ver}+u24.04_amd64.deb", f"libze-dev_{lz_ver}+u24.04_amd64.deb")
    lz_doc = {
        "assets": [
            {
                "name": n,
                "browser_download_url": (
                    f"https://github.com/oneapi-src/level-zero/releases/download/v{lz_ver}/"
                    + n.replace("+", "%2B")
                ),
                "digest": "sha256:" + _fixture_sha256(n),
            }
            for n in lz_names
        ]
    }
    responses = {
        f"https://api.github.com/repos/intel/compute-runtime/releases/tags/{neo_ver}": (
            json.dumps(neo_doc).encode("utf-8")
        ),
        neo_base + sum_name: sum_body.encode("utf-8"),
        "https://api.github.com/repos/intel/intel-graphics-compiler/releases/tags/v2.40.13": (
            json.dumps(igc_doc).encode("utf-8")
        ),
        f"https://api.github.com/repos/oneapi-src/level-zero/releases/tags/v{lz_ver}": (
            json.dumps(lz_doc).encode("utf-8")
        ),
    }
    return responses, neo_doc


def _write_fixture_lock(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    neo_ver: str = _FIXTURE_NEO_VER,
    lz_ver: str = _FIXTURE_LZ_VER,
) -> Path:
    responses, _ = _fixture_api_responses(neo_ver, lz_ver)
    lock_path = tmp_path / "intel-neo-lock.json"
    monkeypatch.setattr(fetch_intel_neo, "make_request", lambda url, token=None: responses[url])
    fetch_intel_neo.write_lock(lock_path, neo_ver, lz_ver)
    return lock_path


class _RecordingAssetOpener:
    def __init__(self, seen_urls: list[str]) -> None:
        self._seen_urls = seen_urls

    def open(self, url: str, *, timeout: int) -> io.BytesIO:
        assert timeout == fetch_intel_neo.HTTP_TIMEOUT_SECONDS
        fetch_intel_neo._validate_https_url(url)
        self._seen_urls.append(url)
        filename = unquote(Path(urlsplit(url).path).name)
        return io.BytesIO(_fixture_deb_bytes(filename))


def test_planted_missing_lock_fails_closed_with_refresh_hint(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    missing = tmp_path / "missing-intel-neo-lock.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "fetch-intel-neo.py",
            "--neo-ver",
            _FIXTURE_NEO_VER,
            "--lock",
            str(missing),
            "--output-dir",
            str(tmp_path / "out"),
        ],
    )

    with pytest.raises(SystemExit) as exc:
        fetch_intel_neo.main()

    err = capsys.readouterr().err
    assert exc.value.code != 0
    assert str(missing) in err
    assert "--write-lock" in err


def test_planted_tampered_lock_sha256_fails_naming_asset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    data = json.loads(lock_path.read_text(encoding="utf-8"))
    tampered_name = data["neo"]["ocloc"]["name"]
    data["neo"]["ocloc"]["sha256"] = "0" * 64
    lock_path.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    seen_urls: list[str] = []
    monkeypatch.setattr(
        fetch_intel_neo, "_opener", lambda _url, _token: _RecordingAssetOpener(seen_urls)
    )
    out_dir = tmp_path / "debs"

    with pytest.raises(SystemExit) as exc:
        fetch_intel_neo.fetch_from_lock(lock_path, _FIXTURE_NEO_VER, out_dir, components="ocloc")

    err = capsys.readouterr().err
    assert exc.value.code != 0
    assert tampered_name in err
    assert not (out_dir / tampered_name).exists()


def test_planted_lock_mode_makes_no_api_github_requests(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    seen_urls: list[str] = []
    seen_tokens: list[str | None] = []

    def fake_opener(url: str, token: str | None) -> _RecordingAssetOpener:
        seen_urls.append(url)
        seen_tokens.append(token)
        return _RecordingAssetOpener(seen_urls)

    monkeypatch.setattr(fetch_intel_neo, "_opener", fake_opener)
    monkeypatch.setattr(
        fetch_intel_neo,
        "make_request",
        lambda url, token=None: seen_urls.append(url) or b"",
    )
    invocations = (
        ["--components", "ocloc"],
        ["--components", "runtime"],
        ["--components", "ocloc", "--level-zero-ver", _FIXTURE_LZ_VER, "--level-zero-dev"],
        ["--components", "runtime", "--level-zero-ver", _FIXTURE_LZ_VER, "--github-token", "tok"],
    )
    for idx, extra in enumerate(invocations):
        out = tmp_path / f"out-{idx}"
        monkeypatch.setattr(
            sys,
            "argv",
            [
                "fetch-intel-neo.py",
                "--neo-ver",
                _FIXTURE_NEO_VER,
                "--lock",
                str(lock_path),
                "--output-dir",
                str(out),
                *extra,
            ],
        )
        fetch_intel_neo.main()
        assert (out / "SHA256SUMS").is_file()

    assert seen_urls
    assert all(urlsplit(u).hostname == "github.com" for u in seen_urls)
    assert all(tok is None for tok in seen_tokens)


def test_planted_version_drift_check_fails_when_config_bumps_without_lock(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    checker_path = Path(__file__).resolve().parents[2] / "scripts/ci/check-workflow-versions.py"
    spec = importlib.util.spec_from_file_location(
        "check_workflow_versions_under_test", checker_path
    )
    assert spec is not None and spec.loader is not None
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)

    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    repo = tmp_path / "repo"
    scripts_dir = repo / "dev" / "scripts"
    scripts_dir.mkdir(parents=True)
    (scripts_dir / "fetch-intel-neo.py").write_text("# stub\n", encoding="utf-8", newline="\n")
    assert checker.check_intel_neo_lock(repo, _FIXTURE_NEO_VER, _FIXTURE_LZ_VER) != []

    (scripts_dir / "intel-neo-lock.json").write_text(
        lock_path.read_text(encoding="utf-8"), encoding="utf-8", newline="\n"
    )
    assert checker.check_intel_neo_lock(repo, _FIXTURE_NEO_VER, _FIXTURE_LZ_VER) == []
    assert any(
        "INTEL_NEO_VERSION" in p
        for p in checker.check_intel_neo_lock(repo, "99.0.0", _FIXTURE_LZ_VER)
    )
    assert any(
        "LEVEL_ZERO_VERSION" in p
        for p in checker.check_intel_neo_lock(repo, _FIXTURE_NEO_VER, "9.9.9")
    )


def test_write_lock_round_trip_matches_select_and_is_deterministic(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    responses, neo_doc = _fixture_api_responses()
    monkeypatch.setattr(fetch_intel_neo, "make_request", lambda url, token=None: responses[url])
    monkeypatch.setattr(
        fetch_intel_neo,
        "download_file",
        lambda *_a, **_kw: (_ for _ in ()).throw(AssertionError("download_file called")),
    )
    lock_a = tmp_path / "lock-a.json"
    lock_b = tmp_path / "lock-b.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "fetch-intel-neo.py",
            "--neo-ver",
            _FIXTURE_NEO_VER,
            "--level-zero-ver",
            _FIXTURE_LZ_VER,
            "--write-lock",
            str(lock_a),
        ],
    )
    fetch_intel_neo.main()
    fetch_intel_neo.write_lock(lock_b, _FIXTURE_NEO_VER, _FIXTURE_LZ_VER)
    assert lock_a.read_bytes() == lock_b.read_bytes()

    locked = fetch_intel_neo.load_lock(lock_a)
    expected_stack = fetch_intel_neo._resolve_stack(neo_doc, _FIXTURE_NEO_VER, "fixture")
    for comp in ("ocloc", "runtime"):
        assert [n for n, _u in locked.stack.select(comp)] == [
            n for n, _u in expected_stack.select(comp)
        ]
    for dev in (False, True):
        pkgs = fetch_intel_neo.level_zero_packages(dev)
        assert [locked.level_zero_assets[p][0] for p in pkgs] == [
            f"{p}_{_FIXTURE_LZ_VER}+u24.04_amd64.deb" for p in pkgs
        ]


def test_lock_version_and_distro_mismatch_fail_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo.fetch_from_lock(lock_path, "99.1.0", tmp_path / "out")
    assert "--write-lock" in capsys.readouterr().err

    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo.fetch_from_lock(
            lock_path, _FIXTURE_NEO_VER, tmp_path / "out", level_zero_ver="9.9.9"
        )
    assert "--write-lock" in capsys.readouterr().err

    bad_distro = json.loads(lock_path.read_text(encoding="utf-8"))
    bad_distro["level_zero_distro"] = "u22.04"
    bad_path = tmp_path / "bad-distro.json"
    bad_path.write_text(json.dumps(bad_distro), encoding="utf-8", newline="\n")
    with pytest.raises(SystemExit, match="1"):
        fetch_intel_neo.load_lock(bad_path)
    assert str(bad_path) in capsys.readouterr().err


def test_lock_rejects_non_github_release_asset_urls(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    base_data = json.loads(lock_path.read_text(encoding="utf-8"))
    ocloc_name = base_data["neo"]["ocloc"]["name"]
    bad_urls = (
        f"https://api.github.com/repos/intel/compute-runtime/releases/download/v1/{ocloc_name}",
        f"https://release-assets.githubusercontent.com/intel/compute-runtime/releases/download/v1/{ocloc_name}",
        f"https://example.com/intel/compute-runtime/releases/download/v1/{ocloc_name}",
        f"https://github.com/intel/compute-runtime/archive/v1/{ocloc_name}",
        f"https://github.com/intel/compute-runtime/releases/download/v1/{ocloc_name}?token=1",
        "https://github.com/intel/compute-runtime/releases/download/v1/other_amd64.deb",
    )
    for idx, bad_url in enumerate(bad_urls):
        mutated = json.loads(json.dumps(base_data))
        mutated["neo"]["ocloc"]["url"] = bad_url
        candidate = tmp_path / f"bad-url-{idx}.json"
        candidate.write_text(json.dumps(mutated), encoding="utf-8", newline="\n")
        with pytest.raises(SystemExit, match="1"):
            fetch_intel_neo.load_lock(candidate)


def test_lock_rejects_uppercase_short_or_non_hex_digest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    base_data = json.loads(lock_path.read_text(encoding="utf-8"))
    bad_digests = (
        "A" * 64,
        "a" * 63 + "F",
        "a" * 63,
        "a" * 65,
        "sha256:" + "a" * 64,
        "g" * 64,
    )
    for idx, bad_digest in enumerate(bad_digests):
        mutated = json.loads(json.dumps(base_data))
        mutated["neo"]["ocloc"]["sha256"] = bad_digest
        candidate = tmp_path / f"bad-sha-{idx}.json"
        candidate.write_text(json.dumps(mutated), encoding="utf-8", newline="\n")
        with pytest.raises(SystemExit, match="1"):
            fetch_intel_neo.load_lock(candidate)


def test_lock_rejects_unknown_keys_duplicate_keys_and_malformed_json(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    base_text = lock_path.read_text(encoding="utf-8")
    raw_cases = [
        "{malformed",
        "[]",
        '{"neo_version": "1", "neo_version": "2"}',
    ]
    for extra_target in ("top", "neo", "level_zero", "asset", "missing"):
        doc = json.loads(base_text)
        if extra_target == "top":
            doc["unexpected"] = True
        elif extra_target == "neo":
            doc["neo"]["extra"] = doc["neo"]["ocloc"]
        elif extra_target == "level_zero":
            doc["level_zero"]["extra"] = doc["level_zero"]["libze1"]
        elif extra_target == "asset":
            doc["neo"]["ocloc"]["extra"] = "1"
        else:
            del doc["neo"]["ocloc"]
        raw_cases.append(json.dumps(doc))
    for idx, text in enumerate(raw_cases):
        candidate = tmp_path / f"schema-{idx}.json"
        candidate.write_text(text, encoding="utf-8", newline="\n")
        with pytest.raises(SystemExit, match="1"):
            fetch_intel_neo.load_lock(candidate)


def test_cli_lock_argument_boundaries(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for argv in (
        ["fetch-intel-neo.py", "--neo-ver", "1", "--write-lock", str(tmp_path / "out.json")],
        [
            "fetch-intel-neo.py",
            "--neo-ver",
            "1",
            "--level-zero-ver",
            "1",
            "--lock",
            str(tmp_path / "a.json"),
            "--write-lock",
            str(tmp_path / "b.json"),
        ],
    ):
        monkeypatch.setattr(sys, "argv", argv)
        with pytest.raises(SystemExit, match="2"):
            fetch_intel_neo.main()


def test_write_lock_refuses_invalid_payload_without_replacing_lock(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    lock_path = _write_fixture_lock(tmp_path, monkeypatch)
    original_bytes = lock_path.read_bytes()
    responses, _ = _fixture_api_responses()
    lz_url = f"https://api.github.com/repos/oneapi-src/level-zero/releases/tags/v{_FIXTURE_LZ_VER}"
    lz_doc = json.loads(responses[lz_url].decode("utf-8"))
    lz_doc["assets"][0]["browser_download_url"] += "?download=1"
    responses[lz_url] = json.dumps(lz_doc).encode("utf-8")
    monkeypatch.setattr(fetch_intel_neo, "make_request", lambda url, token=None: responses[url])

    with pytest.raises(SystemExit) as exc:
        fetch_intel_neo.write_lock(lock_path, _FIXTURE_NEO_VER, _FIXTURE_LZ_VER)

    assert exc.value.code != 0
    assert lock_path.read_bytes() == original_bytes
    assert list(tmp_path.glob(".intel-neo-lock.json.*")) == []

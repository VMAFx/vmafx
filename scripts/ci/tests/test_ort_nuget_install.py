# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the installer with local packages and injected HTTPS responses."""

from __future__ import annotations

import hashlib
import http.client
import io
import json
import zipfile
from pathlib import Path
from typing import BinaryIO
from unittest.mock import patch

import pytest

from scripts.ci import ort_nuget_install as ort
from scripts.lib.safe_subprocess import CommandFailed, TextCommandResult


def package(
    path: Path,
    extra: str = "",
    header_dir: str = "build/native/include",
    libraries: tuple[str, ...] = ort.LIBRARIES,
) -> bytes:
    with zipfile.ZipFile(path, "w") as z:
        if header_dir != "build/native/include":
            z.writestr("build/native/include/", "")
        z.writestr(f"{header_dir}/onnxruntime_c_api.h", "header")
        for name in libraries:
            z.writestr("runtimes/linux-x64/native/" + name, "library")
        for name in ort.NOTICES:
            z.writestr(name, "notice")
        if extra:
            z.writestr(extra, "extra")
    return path.read_bytes()


class Response:
    def __init__(self, data: bytes, status: int = 200) -> None:
        self.stream = io.BytesIO(data)
        self.status = status
        self.length: int | None = None

    def read(self, size: int) -> bytes:
        return self.stream.read(size)


class Connection:
    def __init__(self, response: Response) -> None:
        self.response = response

    def request(self, method: str, path: str) -> None:
        pass

    def getresponse(self) -> Response:
        return self.response

    def close(self) -> None:
        pass


def test_good_install_and_ignored_member(tmp_path: Path) -> None:
    archive = tmp_path / "pkg.zip"
    package(archive, "other/ignored")
    prefix = tmp_path / "prefix"
    with patch.object(ort, "soname", return_value="libonnxruntime.so.1"):
        ort.install(archive, prefix, tmp_path / "pc", "1.2.3")
    assert (prefix / "include/onnxruntime_c_api.h").read_text() == "header"
    assert (prefix / "lib/libonnxruntime.so.1").readlink() == Path("libonnxruntime.so")
    assert (prefix / "LICENSE").exists() and (prefix / "ThirdPartyNotices.txt").exists()
    assert not (prefix / "other").exists()
    assert (tmp_path / "pc/libonnxruntime.pc").read_text() == (
        f"prefix={prefix}\nexec_prefix=${{prefix}}\nlibdir=${{exec_prefix}}/lib\n"
        "includedir=${prefix}/include\n\nName: libonnxruntime\n"
        "Description: ONNX Runtime C API\nVersion: 1.2.3\n"
        "Libs: -L${libdir} -lonnxruntime\nCflags: -I${includedir}\n"
    )


def test_gpu_package_layout(tmp_path: Path) -> None:
    archive = tmp_path / "gpu.nupkg"
    package(
        archive,
        header_dir="buildTransitive/native/include",
        libraries=ort.GPU_LIBRARIES,
    )
    prefix = tmp_path / "gpu-prefix"
    with patch.object(ort, "soname", return_value="libonnxruntime.so.1.22.0"):
        ort.install(
            archive,
            prefix,
            tmp_path / "pc",
            "1.22.0",
            header_dir="buildTransitive/native/include",
            libraries=ort.GPU_LIBRARIES,
        )
    assert (prefix / "include/onnxruntime_c_api.h").read_text() == "header"
    for lib in ort.GPU_LIBRARIES:
        assert (prefix / "lib" / lib).read_text() == "library"
    assert (prefix / "lib/libonnxruntime.so.1.22.0").readlink() == Path("libonnxruntime.so")
    assert (prefix / "LICENSE").exists() and (prefix / "ThirdPartyNotices.txt").exists()


def test_unknown_package_name_refused(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc = ort.main(
        ["--package", "unknown-pkg", "--version", "1.22.0", "--prefix", str(tmp_path / "prefix")]
    )
    assert rc == 1
    assert "unknown ONNX Runtime package: unknown-pkg" in capsys.readouterr().err


def test_cache_hit_used_without_network(tmp_path: Path) -> None:
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir()
    cached = cache_dir / "microsoft.ml.onnxruntime.gpu.linux.1.22.0.nupkg"
    data = package(
        cached,
        header_dir="buildTransitive/native/include",
        libraries=ort.GPU_LIBRARIES,
    )
    sha = hashlib.sha256(data).hexdigest()
    pins = tmp_path / "pins.json"
    pins.write_text(
        json.dumps(
            {
                "packages": {
                    "gpu": {
                        "source": "https://example.test/{version}/microsoft.ml.onnxruntime.gpu.linux.{version}.nupkg",
                        "header_dir": "buildTransitive/native/include",
                        "libraries": list(ort.GPU_LIBRARIES),
                        "versions": {"1.22.0": {"sha256": sha, "size": len(data)}},
                    }
                }
            }
        )
    )
    prefix = tmp_path / "prefix"
    with (
        patch.object(ort, "PINS", pins),
        patch.object(ort, "soname", return_value="libonnxruntime.so.1"),
        patch.object(
            http.client,
            "HTTPSConnection",
            side_effect=AssertionError("network must not be called on cache hit"),
        ) as net,
    ):
        rc = ort.main(
            [
                "--package",
                "gpu",
                "--version",
                "1.22.0",
                "--prefix",
                str(prefix),
                "--pkgconfig-dir",
                str(tmp_path / "pc"),
                "--cache-dir",
                str(cache_dir),
            ]
        )
    assert rc == 0
    assert net.call_count == 0
    for lib in ort.GPU_LIBRARIES:
        assert (prefix / "lib" / lib).exists()


def test_cache_wrong_digest_deleted_and_refetched(tmp_path: Path) -> None:
    good = package(tmp_path / "good.nupkg")
    sha = hashlib.sha256(good).hexdigest()
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir()
    cached = cache_dir / "pkg.nupkg"
    cached.write_bytes(b"x" * len(good))
    with patch.object(
        http.client, "HTTPSConnection", return_value=Connection(Response(good))
    ) as net:
        out = ort.fetch_cached("https://example.test/pkg.nupkg", cache_dir, len(good), sha)
    assert net.call_count == 1
    assert out == cached
    assert cached.read_bytes() == good


def test_print_pin_writes_nothing_under_prefix(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    data = package(tmp_path / "source.nupkg")
    sha = hashlib.sha256(data).hexdigest()
    prefix = tmp_path / "untouched-prefix"
    pins_before = ort.PINS.read_text(encoding="utf-8")
    with patch.object(http.client, "HTTPSConnection", return_value=Connection(Response(data))):
        rc = ort.main(["--package", "gpu", "--print-pin", "1.99.0", "--prefix", str(prefix)])
    assert rc == 0
    out = json.loads(capsys.readouterr().out.strip())
    assert out == {"1.99.0": {"sha256": sha, "size": len(data)}}
    assert not prefix.exists()
    assert ort.PINS.read_text(encoding="utf-8") == pins_before


def test_download_digest_and_size_refused(tmp_path: Path) -> None:
    data = package(tmp_path / "source.zip")
    output = tmp_path / "download"
    actual = hashlib.sha256(data).hexdigest()
    for size, digest, error in (
        (len(data), "0" * 64, actual),
        (len(data) + 1, actual, "size mismatch"),
    ):
        with patch.object(http.client, "HTTPSConnection", return_value=Connection(Response(data))):
            with pytest.raises(ValueError, match=error) as exc:
                ort.download("https://example.test/package", output, size, digest)
        if size == len(data):
            assert digest in str(exc.value) and actual in str(exc.value)
        assert not output.exists()


def test_missing_version_and_source_gone(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    pins = tmp_path / "pins.json"
    pins.write_text(json.dumps({"versions": {}}))
    with patch.object(ort, "PINS", pins):
        assert ort.main(["--version", "missing", "--prefix", str(tmp_path / "prefix")]) == 1
    assert "unknown ONNX Runtime version" in capsys.readouterr().err
    with patch.object(http.client, "HTTPSConnection", return_value=Connection(Response(b"", 404))):
        with pytest.raises(ValueError, match=r"source gone.*example\.test"):
            ort.download("https://example.test/package", tmp_path / "download", 1, "0" * 64)


def test_unsafe_member_and_member_limit(tmp_path: Path) -> None:
    archive = tmp_path / "pkg.zip"
    package(archive, "../escape")
    with pytest.raises(ValueError, match="unsafe member path"):
        ort.install(archive, tmp_path / "prefix", tmp_path / "pc", "1")
    assert not (tmp_path / "prefix").exists()
    with patch.object(ort, "MAX_MEMBERS", 1), zipfile.ZipFile(archive) as z:
        with pytest.raises(ValueError, match="too many members"):
            ort.selected_members(z)


def test_retry_limit(tmp_path: Path) -> None:
    with patch.object(http.client, "HTTPSConnection", side_effect=OSError("offline")) as conn:
        with pytest.raises(ValueError, match="after 3 attempts"):
            ort.download("https://example.test/package", tmp_path / "download", 1, "0" * 64)
    assert conn.call_count == ort.ATTEMPTS


def test_incomplete_body_retries_but_complete_mismatch_is_terminal(tmp_path: Path) -> None:
    data = b"complete body"
    digest = hashlib.sha256(data).hexdigest()
    partial = Response(data[:3])
    partial.length = len(data)
    complete = Response(data)
    complete.length = len(data)
    with patch.object(
        http.client, "HTTPSConnection", side_effect=[Connection(partial), Connection(complete)]
    ) as net:
        ort.download("https://example.test/pkg", tmp_path / "pkg", len(data), digest)
    assert net.call_count == len([partial, complete])
    assert (tmp_path / "pkg").read_bytes() == data
    complete = Response(data)
    complete.length = len(data)
    with patch.object(http.client, "HTTPSConnection", return_value=Connection(complete)) as net:
        with pytest.raises(ValueError, match="size mismatch"):
            ort.download("https://example.test/pkg", tmp_path / "pkg", len(data) + 1, digest)
    assert net.call_count == 1
    with patch.object(
        http.client, "HTTPSConnection", return_value=Connection(Response(data))
    ) as net:
        with pytest.raises(ValueError, match="sha256 mismatch"):
            ort.download("https://example.test/pkg", tmp_path / "pkg", len(data), "0" * 64)
    assert net.call_count == 1


def test_print_pin_incomplete_response_fails(capsys: pytest.CaptureFixture[str]) -> None:
    response = Response(b"short")
    response.length = 10
    with patch.object(http.client, "HTTPSConnection", return_value=Connection(response)):
        rc = ort.main(["--print-pin", "1.30.0"])
    captured = capsys.readouterr()
    assert rc == 1 and captured.out == ""
    assert "ort_nuget_install:" in captured.err


def test_readelf_failure_is_reported(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    archive = tmp_path / "package.nupkg"
    data = package(archive)
    pin = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    pkg = {
        "source": "https://example.test/pkg",
        "header_dir": "build/native/include",
        "libraries": list(ort.LIBRARIES),
        "versions": {"1": pin},
    }
    error = CommandFailed(TextCommandResult(("readelf",), 1, "", "bad ELF"))
    with (
        patch.object(ort, "_load_package", return_value=pkg),
        patch.object(ort, "fetch_cached", return_value=archive),
        patch.object(ort, "run_command", side_effect=error),
    ):
        rc = ort.main(
            [
                "--version",
                "1",
                "--prefix",
                str(tmp_path / "prefix"),
                "--cache-dir",
                str(tmp_path),
                "--pkgconfig-dir",
                str(tmp_path / "pc"),
            ]
        )
    captured = capsys.readouterr()
    assert rc == 1 and "ort_nuget_install:" in captured.err and "bad ELF" in captured.err
    assert "Traceback" not in captured.err


def test_replaced_cache_path_not_extracted(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    archive = tmp_path / "package.nupkg"
    data = package(archive)
    pin = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    pkg = {
        "source": "https://example.test/pkg",
        "header_dir": "build/native/include",
        "libraries": list(ort.LIBRARIES),
        "versions": {"1": pin},
    }

    def replace_cache(*args: object) -> Path:
        archive.write_bytes(b"short")
        return archive

    with (
        patch.object(ort, "_load_package", return_value=pkg),
        patch.object(ort, "fetch_cached", side_effect=replace_cache),
    ):
        rc = ort.main(
            ["--version", "1", "--prefix", str(tmp_path / "prefix"), "--cache-dir", str(tmp_path)]
        )
    assert rc == 1 and "mismatch" in capsys.readouterr().err
    assert not (tmp_path / "prefix").exists()


def test_cache_replaced_after_verification_keeps_verified_handle(tmp_path: Path) -> None:
    archive = tmp_path / "package.nupkg"
    data = package(archive)
    pin = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    pkg = {
        "source": "https://example.test/pkg",
        "header_dir": "build/native/include",
        "libraries": list(ort.LIBRARIES),
        "versions": {"1": pin},
    }
    verify = ort.verify_handle

    def replace_after_check(handle: BinaryIO, size: int, expected: str) -> bool:
        result = verify(handle, size, expected)
        replacement = tmp_path / "replacement"
        replacement.write_bytes(b"short")
        replacement.replace(archive)
        return result

    with (
        patch.object(ort, "_load_package", return_value=pkg),
        patch.object(ort, "fetch_cached", return_value=archive),
        patch.object(ort, "verify_handle", side_effect=replace_after_check),
        patch.object(ort, "soname", return_value="libonnxruntime.so.1"),
    ):
        rc = ort.main(
            [
                "--version",
                "1",
                "--prefix",
                str(tmp_path / "prefix"),
                "--cache-dir",
                str(tmp_path),
                "--pkgconfig-dir",
                str(tmp_path / "pc"),
            ]
        )
    assert rc == 0
    assert archive.read_bytes() == b"short"
    assert (tmp_path / "prefix/include/onnxruntime_c_api.h").read_text() == "header"

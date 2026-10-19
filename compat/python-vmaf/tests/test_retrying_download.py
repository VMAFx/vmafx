# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The retry policy of the fixture downloads (vmaf.tools.retrying_download).

A stand-in server on the loopback interface answers what a test plans: HTTP
504 before 200, as the fixture host did for some minutes on 2026-10-10
(T-CI-FIXTURE-DOWNLOADS-SHORT-RETRY-2026-10-10). The policy is exercised
through its three entry points: retrieve(), download_reactively() of
vmaf.config, and the script form scripts/test/fetch-test-yuvs.sh runs.
"""

from __future__ import annotations

import contextlib
import http.client
import os
import socket
import ssl
import subprocess
import sys
import urllib.error
from collections.abc import Iterator
from pathlib import Path

import pytest

from vmaf import config as vmaf_config
from vmaf.tools import retrying_download

# The stand-in server lives with the repository's shared test helpers.
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.http_stand_in import StandIn  # noqa: E402

BODY = b"fixture bytes\n" * 64
SCRIPT = Path(retrying_download.__file__)
FIXTURE = "yuv/fixture.yuv"


@contextlib.contextmanager
def recorded_waits(monkeypatch: pytest.MonkeyPatch) -> Iterator[list[float]]:
    """Replace the module's time.sleep; the waits it was asked for."""
    waits: list[float] = []
    monkeypatch.setattr(retrying_download.time, "sleep", waits.append)
    yield waits


def test_504_before_200_is_retried_until_the_file_arrives(tmp_path: Path) -> None:
    waits: list[float] = []
    target = tmp_path / "fixture.yuv"
    with StandIn([504, 504], BODY) as server:
        attempts = retrying_download.retrieve(server.url(FIXTURE), str(target), sleep=waits.append)
        assert len(server.requests) == 3
    assert attempts == 3
    assert target.read_bytes() == BODY
    assert waits == [5, 10]


@pytest.mark.parametrize("status", sorted(retrying_download.TRANSIENT_HTTP_STATUS))
def test_every_transient_status_is_retried(tmp_path: Path, status: int) -> None:
    waits: list[float] = []
    with StandIn([status], BODY) as server:
        retrying_download.retrieve(server.url(FIXTURE), str(tmp_path / "f"), sleep=waits.append)
        assert len(server.requests) == 2
    assert waits == [5]


@pytest.mark.parametrize("status", [400, 401, 403, 404, 410])
def test_a_permanent_status_fails_at_once(tmp_path: Path, status: int) -> None:
    waits: list[float] = []
    with StandIn([status] * 9, BODY) as server:
        with pytest.raises(urllib.error.HTTPError) as raised:
            retrying_download.retrieve(server.url(FIXTURE), str(tmp_path / "f"), sleep=waits.append)
        raised.value.close()
        assert len(server.requests) == 1
    assert raised.value.code == status
    assert waits == []


def test_an_outage_longer_than_the_policy_raises_the_last_error(tmp_path: Path) -> None:
    waits: list[float] = []
    with StandIn([504] * 99, BODY) as server:
        with pytest.raises(urllib.error.HTTPError) as raised:
            retrying_download.retrieve(server.url(FIXTURE), str(tmp_path / "f"), sleep=waits.append)
        raised.value.close()
        assert len(server.requests) == len(retrying_download.RETRY_DELAYS_S) + 1
    assert raised.value.code == 504
    assert waits == list(retrying_download.RETRY_DELAYS_S)


def test_a_refused_connection_is_retried(tmp_path: Path) -> None:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    waits: list[float] = []
    with pytest.raises(urllib.error.URLError):
        retrying_download.retrieve(
            f"http://127.0.0.1:{port}/f", str(tmp_path / "f"), (0.0, 0.0), waits.append
        )
    assert waits == [0.0, 0.0]


def test_the_waits_outlast_minutes_and_fit_the_test_timeout() -> None:
    delays = retrying_download.RETRY_DELAYS_S
    # The fixture host answered 504 for at least three minutes; the Coverage
    # Gate's pytest run gives one test 600 s.
    assert 180 <= sum(delays) < 600
    assert len(delays) >= 5
    assert list(delays) == sorted(delays)


def test_what_counts_as_transient() -> None:
    transient = retrying_download.transient
    assert transient(urllib.error.URLError(ConnectionRefusedError()))
    assert transient(urllib.error.URLError(socket.gaierror()))
    assert transient(urllib.error.ContentTooShortError("short", None))  # type: ignore[arg-type]
    assert transient(TimeoutError())
    assert transient(ConnectionResetError())
    assert transient(http.client.IncompleteRead(b""))
    assert not transient(urllib.error.URLError(ssl.SSLCertVerificationError()))
    assert not transient(OSError(28, "No space left on device"))
    assert not transient(RuntimeError("network down"))


def test_download_reactively_outlasts_a_504(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = tmp_path / "resource" / "fixture.yuv"
    with recorded_waits(monkeypatch) as waits, StandIn([504, 504, 504], BODY) as server:
        vmaf_config.download_reactively(str(target), server.url(FIXTURE))
        assert len(server.requests) == 4
    assert target.read_bytes() == BODY
    assert waits == [5, 10, 20]
    assert os.listdir(target.parent) == ["fixture.yuv"]


def test_download_reactively_still_fails_at_once_on_a_missing_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = tmp_path / "fixture.yuv"
    with recorded_waits(monkeypatch) as waits, StandIn([404], BODY) as server:
        with pytest.raises(urllib.error.HTTPError) as raised:
            vmaf_config.download_reactively(str(target), server.url(FIXTURE))
        raised.value.close()
        assert len(server.requests) == 1
    assert waits == []
    assert os.listdir(tmp_path) == []


def test_the_script_form_retries_and_replaces_the_file_whole(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = tmp_path / "fixture.yuv"
    with recorded_waits(monkeypatch) as waits, StandIn([504], BODY) as server:
        assert retrying_download.main([server.url(FIXTURE), str(target)]) == 0
    assert waits == [5]
    assert target.read_bytes() == BODY
    assert os.listdir(tmp_path) == ["fixture.yuv"]


def test_the_script_form_reports_a_failure_and_leaves_no_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    target = tmp_path / "fixture.yuv"
    with recorded_waits(monkeypatch) as waits, StandIn([404], BODY) as server:
        assert retrying_download.main([server.url(FIXTURE), str(target)]) == 1
        url = server.url(FIXTURE)
    assert waits == []
    assert os.listdir(tmp_path) == []
    assert f"retrying_download: {url}: HTTP Error 404" in capsys.readouterr().err


@pytest.mark.parametrize(
    "arguments", [[], ["https://example.invalid/f"], ["file:///etc/hosts", "x"]]
)
def test_the_script_form_refuses_other_arguments(arguments: list[str]) -> None:
    assert retrying_download.main(arguments) == 2


def test_the_script_runs_without_the_package(tmp_path: Path) -> None:
    # scripts/test/fetch-test-yuvs.sh runs the file with `python3 -I`: no vmaf
    # package, no script directory on the module path, no third-party module.
    target = tmp_path / "fixture.yuv"
    with StandIn([], BODY) as server:
        done = subprocess.run(  # noqa: S603 (this interpreter, this repository's script)
            [sys.executable, "-I", str(SCRIPT), server.url(FIXTURE), str(target)],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    assert (done.returncode, done.stdout, done.stderr) == (0, "", "")
    assert target.read_bytes() == BODY

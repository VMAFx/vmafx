# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for `vmaftune.score_backend`.

These tests do not require a real ``vmaf`` binary or any GPU runtime: the
``vmaf --list-backends`` run is stubbed through the ``runner`` seam, and the
selection cases come from ``testdata/score_backend_selection.json``, which the
Go twin (``pkg/scorebackend``) replays too (ADR-1874).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install (mirrors test_corpus).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.score import ScoreRequest, build_vmaf_command
from vmaftune.score_backend import (
    ALL_BACKENDS,
    DEFAULT_FALLBACKS,
    BackendReportError,
    BackendUnavailableError,
    backend_report,
    detect_available_backends,
    parse_backend_report,
    select_backend,
    usable_backends,
)

_CASES = json.loads(
    (_HERE.parents[2] / "testdata" / "score_backend_selection.json").read_text(encoding="utf-8")
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _runner_printing(stdout: str, returncode: int = 0, calls: list | None = None):
    def run(argv, **kwargs):
        if calls is not None:
            calls.append((argv, kwargs))
        return _FakeCompleted(returncode, stdout=stdout)

    return run


# --------------------------------------------------------------------- #
# Shared cases (testdata/score_backend_selection.json)                  #
# --------------------------------------------------------------------- #


@pytest.mark.parametrize("case", _CASES["reports"], ids=lambda c: c["case"])
def test_report_case(case):
    if case["usable"] is None:
        with pytest.raises(BackendReportError):
            parse_backend_report(case["stdout"])
        return
    assert usable_backends(parse_backend_report(case["stdout"])) == case["usable"]


@pytest.mark.parametrize(
    "case", _CASES["selections"], ids=lambda c: f"{c['prefer'] or 'empty'}-{c['available']}"
)
def test_selection_case(case):
    kwargs = {"available": case["available"]}
    if "fallbacks" in case:
        kwargs["fallbacks"] = case["fallbacks"]
    if case.get("error") == "unknown":
        with pytest.raises(ValueError):
            select_backend(case["prefer"], **kwargs)
    elif case.get("error") == "unavailable":
        with pytest.raises(BackendUnavailableError):
            select_backend(case["prefer"], **kwargs)
    else:
        assert select_backend(case["prefer"], **kwargs) == case["want"]


def test_constants_match_the_cli_report_order():
    assert ALL_BACKENDS == ("cpu", "cuda", "sycl", "hip", "metal")
    assert DEFAULT_FALLBACKS == ("cuda", "sycl", "hip", "metal", "cpu")


# --------------------------------------------------------------------- #
# Reading the report from the vmaf binary                               #
# --------------------------------------------------------------------- #


def test_detect_runs_list_backends_and_reads_usable():
    stdout = _CASES["reports"][1]["stdout"]
    calls: list = []
    got = detect_available_backends(
        vmaf_bin="/opt/vmaf/bin/vmaf", runner=_runner_printing(stdout, calls=calls)
    )
    assert got == ["cpu", "cuda"]
    assert calls[0][0] == ["/opt/vmaf/bin/vmaf", "--list-backends"]
    assert calls[0][1]["timeout"] > 0


def test_detect_ignores_vendor_tools_for_a_cpu_only_build():
    """The defect this replaces: a CPU-only vmaf on a host with GPU tools.

    The help text names every backend whatever the build, and the vendor
    tools answered for the host, so `auto` picked a backend the binary then
    refused. The report says what this binary can use.
    """
    stdout = _CASES["reports"][0]["stdout"]
    got = detect_available_backends(vmaf_bin="/opt/vmaf/bin/vmaf", runner=_runner_printing(stdout))
    assert got == ["cpu"]
    assert select_backend("auto", available=got) == "cpu"


def test_old_binary_without_the_option_yields_cpu_and_a_warning(caplog):
    runner = _runner_printing("Usage: vmaf [options]\n", returncode=1)
    with caplog.at_level("WARNING"):
        got = detect_available_backends(vmaf_bin="/opt/vmaf/bin/vmaf", runner=runner)
    assert got == ["cpu"]
    assert "--list-backends exited 1" in caplog.text


def test_missing_binary_is_named():
    with pytest.raises(BackendReportError, match="not on PATH"):
        backend_report("vmaf-binary-that-does-not-exist")


def test_runner_os_error_is_a_report_error():
    def boom(argv, **kwargs):
        raise OSError("exec format error")

    with pytest.raises(BackendReportError, match="exec format error"):
        backend_report("/opt/vmaf/bin/vmaf", runner=boom)


def test_explicit_cpu_needs_no_report():
    calls: list = []
    assert select_backend("cpu", runner=_runner_printing("", calls=calls)) == "cpu"
    assert calls == []


def test_explicit_backend_error_points_at_the_report():
    with pytest.raises(BackendUnavailableError, match="--list-backends"):
        select_backend("cuda", available=["cpu"])


def test_select_detects_when_available_is_not_given():
    stdout = _CASES["reports"][4]["stdout"]
    assert select_backend("auto", vmaf_bin="/x/vmaf", runner=_runner_printing(stdout)) == "metal"


# --------------------------------------------------------------------- #
# build_vmaf_command — verify --backend wiring                          #
# --------------------------------------------------------------------- #


def test_build_vmaf_command_omits_backend_flag_by_default():
    req = ScoreRequest(
        reference=Path("ref.yuv"),
        distorted=Path("dist.mp4"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
    )
    cmd = build_vmaf_command(req, json_output=Path("v.json"), vmaf_bin="vmaf")
    assert "--backend" not in cmd


def test_build_vmaf_command_appends_backend_when_set():
    req = ScoreRequest(
        reference=Path("ref.yuv"),
        distorted=Path("dist.mp4"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
    )
    cmd = build_vmaf_command(req, json_output=Path("v.json"), vmaf_bin="vmaf", backend="cuda")
    assert "--backend" in cmd
    assert cmd[cmd.index("--backend") + 1] == "cuda"


@pytest.mark.parametrize("backend", list(ALL_BACKENDS))
def test_build_vmaf_command_accepts_every_known_backend(backend):
    req = ScoreRequest(
        reference=Path("ref.yuv"),
        distorted=Path("dist.mp4"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
    )
    cmd = build_vmaf_command(req, json_output=Path("v.json"), vmaf_bin="vmaf", backend=backend)
    assert cmd[cmd.index("--backend") + 1] == backend


# --------------------------------------------------------------------- #
# Vulkan backend dropped — ADR-0726 (2026-05-28)                        #
# --------------------------------------------------------------------- #
#
# ADR-0726 removed the Vulkan backend from libvmaf. These tests guard
# against accidental reintroduction of the ``vulkan`` value in the
# argparse choices, the fallback chain, and the validator.


def test_score_backend_choices_exclude_vulkan_after_adr_0726():
    """ADR-0726: 'vulkan' must not appear in ALL_BACKENDS."""
    assert "vulkan" not in ALL_BACKENDS


def test_score_backend_choices_include_hip():
    """argparse must accept 'hip' as a --score-backend value."""
    assert "hip" in ALL_BACKENDS


def test_score_backend_choices_reject_unknown_value():
    """Hard-rule: spelling errors fail loud, never silently downgrade."""
    with pytest.raises(ValueError):
        select_backend(prefer="moltenvk", available=["cpu"])


def test_select_explicit_vulkan_raises_value_error_after_adr_0726():
    """Strict-mode: 'vulkan' is no longer a recognised backend name."""
    with pytest.raises(ValueError):
        select_backend(prefer="vulkan", available=["cpu"])

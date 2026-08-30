# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for `vmaftune.score_backend`.

These tests do not require a real ``vmaf`` binary or any GPU runtime;
all subprocess + capability probes are stubbed via the ``runner`` /
``available`` injection seams the module exposes for that purpose.
"""

from __future__ import annotations

import sys
from pathlib import Path
from unittest import mock

import pytest

# Make src/ importable without an editable install (mirrors test_corpus).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.score import ScoreRequest, build_vmaf_command
from vmaftune.score_backend import (
    ALL_BACKENDS,
    BackendUnavailableError,
    detect_available_backends,
    parse_supported_backends,
    select_backend,
)

_HELP_FULL = (
    " --backend $name:              exclusive backend selector — " "auto|cpu|cuda|sycl|hip|metal.\n"
)
_HELP_CUDA_ONLY = " --backend $name:              exclusive backend selector — auto|cpu|cuda.\n"
_HELP_CPU_ONLY_NO_BACKEND_LINE = (
    " --reference $path:            reference video\n"
    " --threads $unsigned:          thread count\n"
)


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


# --------------------------------------------------------------------- #
# parse_supported_backends                                              #
# --------------------------------------------------------------------- #


def test_parse_full_backend_line_yields_all_supported():
    parsed = parse_supported_backends(_HELP_FULL)
    assert parsed == frozenset({"cpu", "cuda", "sycl", "hip"})


def test_parse_cuda_only_help_text():
    parsed = parse_supported_backends(_HELP_CUDA_ONLY)
    assert parsed == frozenset({"cpu", "cuda"})


def test_parse_no_backend_line_falls_back_to_cpu_only():
    parsed = parse_supported_backends(_HELP_CPU_ONLY_NO_BACKEND_LINE)
    assert parsed == frozenset({"cpu"})


def test_parse_does_not_get_fooled_by_substring_matches():
    # The word "cuda" appears in prose but not as a --backend alternation.
    text = "We support CUDA; see ADR-0127 for cuda details. No backend line.\n"
    parsed = parse_supported_backends(text)
    assert parsed == frozenset({"cpu"})


# --------------------------------------------------------------------- #
# detect_available_backends                                             #
# --------------------------------------------------------------------- #


def _help_runner(help_text: str):
    """Build a runner stub that returns `help_text` for `vmaf --help`."""

    def runner(cmd, capture_output, text, check, timeout=None):
        if cmd[-1] == "--help":
            return _FakeCompleted(0, stdout=help_text)
        # Fall through: hardware probes default to "no GPU".
        return _FakeCompleted(1)

    return runner


def test_detect_cpu_only_when_binary_advertises_no_gpu():
    runner = _help_runner(_HELP_CPU_ONLY_NO_BACKEND_LINE)
    with mock.patch("vmaftune.score_backend.shutil.which", return_value="/usr/bin/vmaf"):
        avail = detect_available_backends(vmaf_bin="vmaf", runner=runner)
    assert avail == ["cpu"]


def test_detect_cuda_when_binary_supports_and_nvidia_smi_succeeds():
    def runner(cmd, capture_output, text, check, timeout=None):
        if cmd[0] == "vmaf":
            return _FakeCompleted(0, stdout=_HELP_FULL)
        if cmd[0] == "nvidia-smi":
            return _FakeCompleted(0, stdout="GPU 0: NVIDIA RTX 4090 (UUID: ...)\n")
        return _FakeCompleted(1)

    def fake_which(binary):
        return f"/usr/bin/{binary}" if binary in {"vmaf", "nvidia-smi"} else None

    with mock.patch("vmaftune.score_backend.shutil.which", side_effect=fake_which):
        avail = detect_available_backends(vmaf_bin="vmaf", runner=runner)
    assert "cuda" in avail
    assert "cpu" in avail
    assert "sycl" not in avail
    assert "hip" not in avail


def test_detect_orders_results_per_all_backends():
    def runner(cmd, capture_output, text, check, timeout=None):
        if cmd[0] == "vmaf":
            return _FakeCompleted(0, stdout=_HELP_FULL)
        if cmd[0] == "nvidia-smi":
            return _FakeCompleted(0, stdout="GPU 0\n")
        if cmd[0] == "sycl-ls":
            return _FakeCompleted(0, stdout="[opencl:gpu] Intel Arc\n")
        if cmd[0] == "rocminfo":
            return _FakeCompleted(0, stdout="Name:                    gfx1036\n")
        return _FakeCompleted(1)

    with mock.patch("vmaftune.score_backend.shutil.which", side_effect=lambda b: f"/x/{b}"):
        avail = detect_available_backends(vmaf_bin="vmaf", runner=runner)
    # detect must respect the tuple order exposed by ALL_BACKENDS.
    assert avail == [b for b in ALL_BACKENDS if b in set(avail)]


def test_detect_hip_when_binary_supports_and_rocminfo_succeeds():
    def runner(cmd, capture_output, text, check, timeout=None):
        if cmd[0] == "vmaf":
            return _FakeCompleted(0, stdout=_HELP_FULL)
        if cmd[0] == "rocminfo":
            return _FakeCompleted(0, stdout="Name:                    gfx1100\n")
        return _FakeCompleted(1)

    def fake_which(binary):
        return f"/usr/bin/{binary}" if binary in {"vmaf", "rocminfo"} else None

    with mock.patch("vmaftune.score_backend.shutil.which", side_effect=fake_which):
        avail = detect_available_backends(vmaf_bin="vmaf", runner=runner)
    assert "hip" in avail
    assert "cuda" not in avail


def test_detect_hip_falls_back_to_rocm_smi():
    def runner(cmd, capture_output, text, check, timeout=None):
        if cmd[0] == "vmaf":
            return _FakeCompleted(0, stdout=_HELP_FULL)
        if cmd[0] == "rocm-smi":
            return _FakeCompleted(0, stdout="GPU[0] : Card series: AMD Radeon RX 7900 XTX\n")
        return _FakeCompleted(1)

    def fake_which(binary):
        return f"/usr/bin/{binary}" if binary in {"vmaf", "rocm-smi"} else None

    with mock.patch("vmaftune.score_backend.shutil.which", side_effect=fake_which):
        avail = detect_available_backends(vmaf_bin="vmaf", runner=runner)
    assert "hip" in avail


# --------------------------------------------------------------------- #
# select_backend                                                        #
# --------------------------------------------------------------------- #


def test_select_auto_picks_cuda_when_available():
    chosen = select_backend(prefer="auto", available=["cpu", "cuda"])
    assert chosen == "cuda"


def test_select_auto_walks_fallback_chain_to_hip():
    chosen = select_backend(prefer="auto", available=["cpu", "hip"])
    assert chosen == "hip"


def test_select_auto_prefers_sycl_before_hip():
    chosen = select_backend(prefer="auto", available=["cpu", "hip", "sycl"])
    assert chosen == "sycl"


def test_select_auto_prefers_cuda_before_sycl():
    chosen = select_backend(prefer="auto", available=["cpu", "sycl", "cuda"])
    assert chosen == "cuda"


def test_select_auto_lands_on_cpu_when_no_gpu_available():
    chosen = select_backend(prefer="auto", available=["cpu"])
    assert chosen == "cpu"


def test_select_auto_returns_cpu_even_if_probes_returned_empty_list():
    # Defensive: caller can't pass an empty list literally because cpu is
    # always added by detect, but unit-test the floor anyway.
    chosen = select_backend(prefer="auto", available=[])
    assert chosen == "cpu"


def test_select_explicit_cuda_succeeds_when_available():
    chosen = select_backend(prefer="cuda", available=["cpu", "cuda"])
    assert chosen == "cuda"


def test_select_explicit_cuda_raises_when_unavailable():
    with pytest.raises(BackendUnavailableError) as exc:
        select_backend(prefer="cuda", available=["cpu"])
    assert "cuda" in str(exc.value)
    assert "available" in str(exc.value).lower()


def test_select_explicit_hip_succeeds_when_available():
    chosen = select_backend(prefer="hip", available=["cpu", "hip"])
    assert chosen == "hip"


def test_select_explicit_hip_raises_when_unavailable():
    with pytest.raises(BackendUnavailableError) as exc:
        select_backend(prefer="hip", available=["cpu"])
    assert "hip" in str(exc.value)


def test_select_explicit_vulkan_rejected_after_adr_0726():
    # ADR-0726 dropped the Vulkan backend. ``prefer="vulkan"`` is no
    # longer a recognised backend name and must surface a clear
    # ``ValueError`` rather than silently downgrading.
    with pytest.raises(ValueError):
        select_backend(prefer="vulkan", available=["cpu", "cuda"])


def test_select_rejects_unknown_backend_name():
    with pytest.raises(ValueError):
        select_backend(prefer="metal", available=["cpu"])


def test_select_custom_fallback_chain_is_honoured():
    # Operators can override the default chain with their own order.
    chosen = select_backend(
        prefer="auto",
        fallbacks=("hip", "cuda", "cpu"),
        available=["cpu", "cuda", "hip"],
    )
    assert chosen == "hip"


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

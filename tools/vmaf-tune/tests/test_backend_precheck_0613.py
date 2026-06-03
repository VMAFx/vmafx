# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for ADR-0613 P1-1 — select_backend() pre-check in
``vmaf-tune compare`` and ``vmaf-tune tune-per-shot``.

Before ADR-0613, three subcommand handlers passed the raw ``--score-backend``
value directly to ``bisect_target_vmaf()`` without first calling
``select_backend()``.  An explicit ``--score-backend cuda`` on a CPU-only
binary surfaced as a cryptic vmaf binary error mid-bisect rather than the
clean ``BackendUnavailableError`` exit-2 that ``corpus``, ``ladder``, and
``fast`` already produced.

These tests pin three contracts:

1. ``_run_compare``: ``BackendUnavailableError`` from ``select_backend()``
   causes exit RC=2 with a message containing "compare" and the backend name.
2. ``_run_tune_per_shot``: same contract for the per-shot path.
3. ``_run_compare`` with ``--no-bisect`` (CRF-sweep mode): the precheck fires
   before the sweep dispatches, so the same exit-2 behaviour applies.
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path
from unittest.mock import patch

TOOLS_SRC = Path(__file__).resolve().parents[1] / "src"
if str(TOOLS_SRC) not in sys.path:
    sys.path.insert(0, str(TOOLS_SRC))

from vmaftune import cli as cli_module  # noqa: E402
from vmaftune.score_backend import BackendUnavailableError  # noqa: E402


def _make_unavailable_backend(backend_name: str = "cuda"):
    """Return a ``select_backend`` replacement that always raises."""

    def boom(*_args, **_kwargs):
        raise BackendUnavailableError(
            f"backend {backend_name!r} requested but not available on this host "
            "(available: cpu). Check that the local vmaf binary was built with "
            "the matching backend support."
        )

    return boom


# ---------------------------------------------------------------------------
# 1. _run_compare — bisect path
# ---------------------------------------------------------------------------


def test_run_compare_exits_2_on_unavailable_backend(monkeypatch) -> None:
    """ADR-0613 P1-1: ``vmaf-tune compare --score-backend cuda`` on a host
    where cuda is unavailable must exit RC=2 before any bisect work starts."""
    monkeypatch.setattr(cli_module, "select_backend", _make_unavailable_backend("cuda"))

    # Minimal argparse Namespace — only the fields _run_compare inspects
    # before reaching the backend precheck.
    args = argparse.Namespace(
        src="/tmp/missing.yuv",
        encoders="libx264",
        format="json",
        no_bisect=False,
        score_backend="cuda",
        vmaf_bin="vmaf",
        # Fields checked after format validation but before backend check — must
        # be present to not raise AttributeError during the format-validation gate.
        vaapi_device=None,
        width=None,
        height=None,
        framerate=24.0,
        duration=0.0,
        _framerate_was_default=True,
        _duration_was_default=True,
        pix_fmt="yuv420p",
        preset=["medium"],
        crf_min=None,
        crf_max=None,
        max_iterations=8,
        vmaf_model=None,
        ffmpeg_bin="ffmpeg",
        target_vmaf=92.0,
        target_vmafs="75,80,85,90,93",
        _target_vmafs_was_default=True,
        _target_vmaf_was_default=True,
        predicate_module=None,
        output=None,
        workdir=None,
        max_concurrent_decodes=1,
        sample_clip_seconds=0.0,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        rc = cli_module._run_compare(args)

    assert rc == 2, f"Expected exit RC=2 for unavailable backend, got {rc}"
    msg = stderr_buf.getvalue()
    assert "compare" in msg.lower(), f"Expected 'compare' in stderr, got: {msg!r}"
    assert "cuda" in msg.lower(), f"Expected 'cuda' in stderr, got: {msg!r}"


def test_run_compare_exits_2_with_actionable_message(monkeypatch) -> None:
    """The error message must include both the requested backend name and a
    hint that it is unavailable (mirrors the ladder contract in ADR-0511)."""
    monkeypatch.setattr(cli_module, "select_backend", _make_unavailable_backend("hip"))

    args = argparse.Namespace(
        src="/tmp/missing.yuv",
        encoders="libx264",
        format="json",
        no_bisect=False,
        score_backend="hip",
        vmaf_bin="vmaf",
        vaapi_device=None,
        width=None,
        height=None,
        framerate=24.0,
        duration=0.0,
        _framerate_was_default=True,
        _duration_was_default=True,
        pix_fmt="yuv420p",
        preset=["medium"],
        crf_min=None,
        crf_max=None,
        max_iterations=8,
        vmaf_model=None,
        ffmpeg_bin="ffmpeg",
        target_vmaf=92.0,
        target_vmafs="75,80,85,90,93",
        _target_vmafs_was_default=True,
        _target_vmaf_was_default=True,
        predicate_module=None,
        output=None,
        workdir=None,
        max_concurrent_decodes=1,
        sample_clip_seconds=0.0,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        rc = cli_module._run_compare(args)

    assert rc == 2
    msg = stderr_buf.getvalue()
    # The BackendUnavailableError message should include "available" or
    # "not available" to give the operator context.
    assert "available" in msg.lower(), f"Expected availability hint in stderr: {msg!r}"


# ---------------------------------------------------------------------------
# 2. _run_tune_per_shot — backend precheck
# ---------------------------------------------------------------------------


def test_run_tune_per_shot_exits_2_on_unavailable_backend(monkeypatch) -> None:
    """ADR-0613 P1-1: ``vmaf-tune tune-per-shot --score-backend cuda`` on a
    CPU-only host must exit RC=2 before detecting shots."""
    monkeypatch.setattr(cli_module, "select_backend", _make_unavailable_backend("cuda"))

    args = argparse.Namespace(
        src="/tmp/missing.yuv",
        score_backend="cuda",
        vmaf_bin="vmaf",
        encoder="libx264",
        target_vmaf=92.0,
        # Remaining fields — tune-per-shot inspects these after the precheck
        # during geometry resolution; won't be reached on BackendUnavailableError.
        width=1920,
        height=1080,
        framerate=24.0,
        total_frames=0,
        pix_fmt="yuv420p",
        bitdepth=8,
        per_shot_bin="vmaf-perShot",
        scene_threshold=None,
        max_shot_duration=None,
        predicate_module=None,
        crf_min=None,
        crf_max=None,
        workdir=None,
        max_concurrent_decodes=1,
        ffmpeg_bin="ffmpeg",
        vmaf_model=None,
        preset=["medium"],
        max_iterations=8,
        output=None,
        plan_out=None,
        chart_out=None,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        rc = cli_module._run_tune_per_shot(args)

    assert rc == 2, f"Expected exit RC=2 for unavailable backend, got {rc}"
    msg = stderr_buf.getvalue()
    assert "tune-per-shot" in msg.lower(), f"Expected 'tune-per-shot' in stderr: {msg!r}"
    assert "cuda" in msg.lower(), f"Expected 'cuda' in stderr: {msg!r}"


# ---------------------------------------------------------------------------
# 3. _run_compare with --no-bisect (CRF-sweep mode dispatched by _run_compare)
# ---------------------------------------------------------------------------


def test_run_compare_no_bisect_exits_2_on_unavailable_backend(monkeypatch) -> None:
    """ADR-0613 P1-1: the CRF-sweep path (``compare --no-bisect``) is also
    gated by the select_backend() precheck in _run_compare before dispatch
    to _run_compare_crf_sweep."""
    monkeypatch.setattr(cli_module, "select_backend", _make_unavailable_backend("sycl"))

    args = argparse.Namespace(
        src="/tmp/missing.yuv",
        encoders="libx264",
        format="json",
        no_bisect=True,
        score_backend="sycl",
        vmaf_bin="vmaf",
        vaapi_device=None,
        width=None,
        height=None,
        framerate=24.0,
        duration=0.0,
        _framerate_was_default=True,
        _duration_was_default=True,
        pix_fmt="yuv420p",
        preset=["medium"],
        crf_min=None,
        crf_max=None,
        max_iterations=8,
        vmaf_model=None,
        ffmpeg_bin="ffmpeg",
        target_vmaf=92.0,
        target_vmafs="75,80,85,90,93",
        _target_vmafs_was_default=True,
        _target_vmaf_was_default=True,
        predicate_module=None,
        output=None,
        workdir=None,
        max_concurrent_decodes=1,
        sample_clip_seconds=0.0,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        rc = cli_module._run_compare(args)

    assert rc == 2, f"Expected exit RC=2 for unavailable backend, got {rc}"
    msg = stderr_buf.getvalue()
    assert "sycl" in msg.lower(), f"Expected 'sycl' in stderr: {msg!r}"


# ---------------------------------------------------------------------------
# 4. auto backend — must NOT raise, must log a backend selection line
# ---------------------------------------------------------------------------


def test_run_compare_auto_backend_logs_selection(monkeypatch) -> None:
    """When ``--score-backend auto`` is passed, ``select_backend()`` returns
    "cpu" (or another available backend) and the CLI logs a selection line.
    The function must not raise BackendUnavailableError on "auto"."""

    def always_cpu(*_args, **_kwargs):
        return "cpu"

    monkeypatch.setattr(cli_module, "select_backend", always_cpu)

    # _run_compare will proceed past the precheck and fail later on a missing
    # src file or invalid encoder — that's fine; the test only cares that
    # (a) RC != 2 from the precheck, and (b) the selection line is printed.
    args = argparse.Namespace(
        src="/tmp/missing.yuv",
        encoders="libx264",
        format="json",
        no_bisect=False,
        score_backend="auto",
        vmaf_bin="vmaf",
        vaapi_device=None,
        width=None,
        height=None,
        framerate=24.0,
        duration=0.0,
        _framerate_was_default=True,
        _duration_was_default=True,
        pix_fmt="yuv420p",
        preset=["medium"],
        crf_min=None,
        crf_max=None,
        max_iterations=8,
        vmaf_model=None,
        ffmpeg_bin="ffmpeg",
        target_vmaf=92.0,
        target_vmafs="75,80,85,90,93",
        _target_vmafs_was_default=True,
        _target_vmaf_was_default=True,
        predicate_module=None,
        output=None,
        workdir=None,
        max_concurrent_decodes=1,
        sample_clip_seconds=0.0,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        cli_module._run_compare(args)

    # RC might be 2 for other reasons (missing file, etc.) but NOT because
    # of BackendUnavailableError — the message should not mention an
    # unavailability error.
    msg = stderr_buf.getvalue()
    assert "not available" not in msg.lower(), (
        "auto backend should not trigger BackendUnavailableError, got: " + msg
    )
    # The backend-selection log line must be present.
    assert (
        "scoring backend" in msg.lower()
    ), f"Expected 'scoring backend' log line in stderr, got: {msg!r}"


# ---------------------------------------------------------------------------
# 5. Smoke: _build_per_shot_bisect_predicate respects pre-resolved backend
# ---------------------------------------------------------------------------


def test_build_per_shot_bisect_predicate_maps_auto_to_none() -> None:
    """ADR-0613: ``_build_per_shot_bisect_predicate`` must map
    ``args.score_backend in (None, "auto")`` → ``None`` (let the vmaf
    binary self-select), and any explicit name through unchanged.

    This is a unit-level boundary test; it does NOT invoke bisect_target_vmaf.
    We inspect the closure's captured ``score_backend`` via the function
    source / internal state rather than running the real bisect."""
    import inspect

    src = inspect.getsource(cli_module._build_per_shot_bisect_predicate)
    # The ADR-0613 version uses the combined check (None, "auto").
    assert 'args.score_backend in (None, "auto")' in src, (
        "ADR-0613: _build_per_shot_bisect_predicate must map both None and "
        '"auto" to None (combined in-tuple check); source mismatch.'
    )


# ---------------------------------------------------------------------------
# 6. Smoke: _run_tune_per_shot log line on auto backend
# ---------------------------------------------------------------------------


def test_run_tune_per_shot_auto_backend_logs_selection(monkeypatch) -> None:
    """``tune-per-shot --score-backend auto`` must log a backend selection
    line and must NOT raise BackendUnavailableError."""

    def always_cpu(*_args, **_kwargs):
        return "cpu"

    monkeypatch.setattr(cli_module, "select_backend", always_cpu)

    args = argparse.Namespace(
        src=Path("/tmp/missing.yuv"),
        score_backend="auto",
        vmaf_bin="vmaf",
        encoder="libx264",
        target_vmaf=92.0,
        width=1920,
        height=1080,
        framerate=24.0,
        total_frames=0,
        pix_fmt="yuv420p",
        bitdepth=8,
        per_shot_bin="vmaf-perShot",
        scene_threshold=None,
        max_shot_duration=None,
        predicate_module=None,
        crf_min=None,
        crf_max=None,
        workdir=None,
        max_concurrent_decodes=1,
        ffmpeg_bin="ffmpeg",
        vmaf_model=None,
        preset=["medium"],
        max_iterations=8,
        output=None,
        plan_out=None,
        chart_out=None,
    )

    stderr_buf = io.StringIO()
    with patch("sys.stderr", stderr_buf):
        # Will fail past the precheck (no real source file / binary), but
        # must not fail AT the precheck.
        cli_module._run_tune_per_shot(args)

    msg = stderr_buf.getvalue()
    assert "not available" not in msg.lower(), (
        "auto backend must not trigger BackendUnavailableError, got: " + msg
    )
    assert (
        "scoring backend" in msg.lower()
    ), f"Expected 'scoring backend = cpu' log line, got: {msg!r}"

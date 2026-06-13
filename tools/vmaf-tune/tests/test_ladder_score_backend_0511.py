# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for ADR-0511 / Bug C — `vmaf-tune ladder --score-backend`.

See `docs/adr/0511-mcp-backend-probe-allowlist-and-ladder-backend.md`
for the cluster write-up.

These tests pin three contracts:

1. ``vmaf-tune ladder --help`` advertises the new
   ``--score-backend {auto,cpu,cuda,sycl,hip}`` flag (DX symmetry
   with the sibling ``compare`` and ``tune-per-shot`` subcommands;
   ADR-0726 dropped the Vulkan value 2026-05-28).
2. ``--score-backend cuda`` plumbs through
   ``make_default_sampler`` → ``CorpusOptions.score_backend`` so the
   underlying corpus invocation runs ``vmaf --backend cuda``.
3. ``tune-per-shot``'s historical
   ``None if args.score_backend == "auto" else args.score_backend``
   conversion at ``_build_per_shot_bisect_predicate`` is preserved
   (ADR-0509 explicitly forbade regressing it).
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

# Make the in-tree package importable without an editable install.
TOOLS_SRC = Path(__file__).resolve().parents[1] / "src"
if str(TOOLS_SRC) not in sys.path:
    sys.path.insert(0, str(TOOLS_SRC))

from vmaftune import cli as cli_module  # noqa: E402
from vmaftune import ladder as ladder_module  # noqa: E402


def _capture_help(argv: list[str]) -> str:
    buf = io.StringIO()
    with patch("sys.stdout", buf), pytest.raises(SystemExit) as exc:
        cli_module.main(argv)
    assert exc.value.code == 0
    return buf.getvalue()


# ---------------------------------------------------------------------------
# 1. CLI surface — --score-backend must be advertised on `ladder --help`
# ---------------------------------------------------------------------------


def test_ladder_help_advertises_score_backend_flag() -> None:
    """ADR-0511 Bug C: `vmaf-tune ladder --help` must list
    ``--score-backend``. Sibling subcommands already have the flag —
    asymmetric DX was the user-discoverable bug."""
    help_text = _capture_help(["ladder", "--help"])
    assert "--score-backend" in help_text, (
        "ADR-0511 Bug C regression: ladder --help is missing " "--score-backend"
    )


def test_ladder_score_backend_accepts_all_supported_backends() -> None:
    """`--score-backend` must accept the same enum as the sibling
    subcommands: ``auto|cpu|cuda|sycl|hip`` (post-ADR-0726)."""
    parser = cli_module._build_parser()
    # argparse exposes subparsers via `choices` on the action whose
    # ``choices`` is a dict-like of {name: subparser}.
    sub_actions = [a for a in parser._actions if hasattr(a, "choices") and a.choices]
    ladder_parser = next(
        action.choices["ladder"] for action in sub_actions if "ladder" in (action.choices or {})
    )
    score_action = next(
        a for a in ladder_parser._actions if "--score-backend" in (a.option_strings or [])
    )
    expected = {"auto", "cpu", "cuda", "sycl", "hip"}
    actual = set(score_action.choices or ())
    assert expected.issubset(
        actual
    ), f"--score-backend enum is missing {expected - actual} (got {actual})"
    # ADR-0726: vulkan must not be present.
    assert (
        "vulkan" not in actual
    ), f"ADR-0726 regression: vulkan re-appeared in --score-backend choices (got {actual})"


# ---------------------------------------------------------------------------
# 2. End-to-end plumbing — --score-backend reaches CorpusOptions
# ---------------------------------------------------------------------------


def test_ladder_score_backend_threads_into_corpus_options(monkeypatch) -> None:
    """ADR-0511 Bug C: when the user passes ``--score-backend cuda``,
    the ladder default sampler must pass ``score_backend='cuda'`` to
    ``CorpusOptions`` so the underlying ``vmaf`` invocation runs with
    ``--backend cuda``."""
    from vmaftune import corpus as corpus_module

    captured_opts: list[corpus_module.CorpusOptions] = []

    def fake_iter_rows(job, opts, **_kwargs):
        captured_opts.append(opts)
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": 100.0 - (crf - 18) * 1.5,
                "bitrate_kbps": 100.0 * (40 - crf),
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)

    # Compose a `make_default_sampler` closure with score_backend='cuda'
    # directly (the same plumbing `_run_ladder` does after
    # `select_backend` resolves the user's choice).
    sampler = ladder_module.make_default_sampler(
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=1.0,
        src_width=640,
        src_height=360,
        score_backend="cuda",
    )
    point = sampler(Path("dummy.yuv"), "libx264", 640, 360, target_vmaf=92.0)
    assert isinstance(point, ladder_module.LadderPoint)
    assert captured_opts, "fake_iter_rows was never called"
    assert all(
        opts.score_backend == "cuda" for opts in captured_opts
    ), f"score_backend did not reach CorpusOptions: {[o.score_backend for o in captured_opts]}"


def test_ladder_default_sampler_passes_none_when_score_backend_unset(monkeypatch) -> None:
    """Default behaviour (no ``--score-backend``) is unchanged:
    ``score_backend`` arrives at ``CorpusOptions`` as ``None`` so
    libvmaf picks the fastest available runtime itself. Preserves the
    pre-ADR-0509 zero-flag invocation pattern."""
    from vmaftune import corpus as corpus_module

    captured_opts: list[corpus_module.CorpusOptions] = []

    def fake_iter_rows(job, opts, **_kwargs):
        captured_opts.append(opts)
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": 100.0 - (crf - 18) * 1.5,
                "bitrate_kbps": 100.0 * (40 - crf),
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)

    sampler = ladder_module.make_default_sampler(
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=1.0,
        src_width=640,
        src_height=360,
        # score_backend left at its default (None).
    )
    sampler(Path("dummy.yuv"), "libx264", 640, 360, target_vmaf=92.0)
    assert captured_opts, "fake_iter_rows was never called"
    assert all(
        opts.score_backend is None for opts in captured_opts
    ), f"backwards-compat regression: {[o.score_backend for o in captured_opts]}"


# ---------------------------------------------------------------------------
# 3. tune-per-shot's existing auto -> None contract is preserved
# ---------------------------------------------------------------------------


def test_tune_per_shot_predicate_keeps_auto_to_none_conversion() -> None:
    """ADR-0509 (preserved by ADR-0613): the tune-per-shot predicate must map
    ``args.score_backend in (None, "auto")`` → ``None`` so the vmaf binary
    self-selects the fastest available backend when the user does not request
    an explicit one.

    ADR-0613 expanded the original ``== "auto"`` check to also cover ``None``
    (which argparse sets when ``--score-backend`` is absent), making the
    behaviour strictly more correct without regressing the "auto" case.
    The test is updated to match the new, broader pattern.
    """
    src_path = TOOLS_SRC / "vmaftune" / "cli.py"
    src = src_path.read_text(encoding="utf-8")
    # ADR-0613 updated the predicate from `== "auto"` to `in (None, "auto")`.
    # Either form satisfies ADR-0509's "auto must map to None" contract; we pin
    # the ADR-0613 form so that neither a revert to the old form nor a removal
    # of the predicate goes undetected.
    needle = 'args.score_backend in (None, "auto")'
    assert needle in src, (
        "ADR-0509 / ADR-0613 regression: _build_per_shot_bisect_predicate "
        "must map score_backend in (None, 'auto') → None. "
        "Check that the combined in-tuple check is still present."
    )


def test_ladder_run_rejects_unavailable_backend(monkeypatch) -> None:
    """ADR-0511 Bug C: requesting ``--score-backend cuda`` on a host
    where the vmaf binary does not advertise CUDA must exit RC=2 with
    a clear error, BEFORE any encodes start. Drives the failure via
    ``select_backend`` raising ``BackendUnavailableError``."""
    from vmaftune.score_backend import BackendUnavailableError

    def boom(*_args, **_kwargs):
        raise BackendUnavailableError("--score-backend cuda but local vmaf does not advertise it")

    # The CLI does ``from .score_backend import ..., select_backend``
    # at module import time, so patch the imported name on the cli
    # module (not on score_backend).
    monkeypatch.setattr(cli_module, "select_backend", boom)

    args = argparse.Namespace(
        src=Path("/tmp/missing.yuv"),
        encoder="libx264",
        resolutions="640x360",
        target_vmafs="92.0",
        quality_tiers=1,
        spacing="vmaf",
        format="json",
        with_uncertainty=False,
        uncertainty_sidecar=None,
        rung_overlap_threshold=0.5,
        output=None,
        src_width=640,
        src_height=360,
        score_backend="cuda",
        vmaf_bin="vmaf",
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=1.0,
    )

    captured_stderr = io.StringIO()
    with patch("sys.stderr", captured_stderr):
        rc = cli_module._run_ladder(args)
    assert rc == 2, f"expected RC=2 for unavailable backend, got {rc}"
    msg = captured_stderr.getvalue()
    assert "ladder" in msg.lower(), msg
    assert "cuda" in msg.lower(), msg

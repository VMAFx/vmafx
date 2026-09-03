# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""vmaf binary driver for the Option C ROI-VMAF tool.

Wraps two invocations of the libvmaf CLI:

  1. Full-frame: ``vmaf --reference REF --distorted DIS ...``
  2. Saliency-masked: ``vmaf --reference REF --distorted DIS_MASKED ...``
     where ``DIS_MASKED`` is the distorted YUV with the per-pixel
     saliency mask applied (low-saliency pixels replaced with the
     reference content so they do not contribute to the metric).

The mask materialisation is done by :mod:`vmafroiscore.mask` and lives
behind the ``[runtime]`` optional dependency. This module is the
subprocess seam — kept narrow so tests can mock ``subprocess.run`` and
a fake mask materialiser.

Mirrors :mod:`vmaftune.score` deliberately: a future helper
consolidation under ``tools/_shared/`` would be mechanical.
"""

from __future__ import annotations

import dataclasses
import json
import re
import subprocess
import tempfile
import time
from pathlib import Path

from .defaultmodel import DEFAULT_MODEL


@dataclasses.dataclass(frozen=True)
class ScoreRequest:
    """Pair to score: reference YUV vs distorted YUV."""

    reference: Path
    distorted: Path
    width: int
    height: int
    pix_fmt: str
    model: str = DEFAULT_MODEL


@dataclasses.dataclass(frozen=True)
class ScoreResult:
    """Outcome of one ``vmaf`` invocation."""

    request: ScoreRequest
    vmaf_score: float
    score_time_ms: float
    vmaf_binary_version: str
    exit_status: int
    stderr_tail: str


_VMAF_VERSION_RE = re.compile(r"VMAF version[: ]+(\S+)")


def build_vmaf_command(
    req: ScoreRequest,
    json_output: Path,
    *,
    vmaf_bin: str = "vmaf",
) -> list[str]:
    """Compose the libvmaf CLI argv. Pure function; tests pin it."""
    return [
        vmaf_bin,
        "--reference",
        str(req.reference),
        "--distorted",
        str(req.distorted),
        "--width",
        str(req.width),
        "--height",
        str(req.height),
        "--pixel_format",
        _pixfmt_to_vmaf(req.pix_fmt),
        "--bitdepth",
        str(_bitdepth_for(req.pix_fmt)),
        "--model",
        f"version={req.model}",
        "--json",
        "--output",
        str(json_output),
    ]


def _pixfmt_to_vmaf(pix_fmt: str) -> str:
    """Map ffmpeg pix_fmt to libvmaf's --pixel_format vocabulary.

    Falls back to ``420`` for any unrecognised string so the CLI
    invocation can still proceed (vmaf will reject malformed inputs
    on its own).
    """
    if pix_fmt.startswith("yuv422"):
        return "422"
    if pix_fmt.startswith("yuv444"):
        return "444"
    return "420"


def _bitdepth_for(pix_fmt: str) -> int:
    if "16le" in pix_fmt or "p16" in pix_fmt:
        return 16
    if "10le" in pix_fmt or "p10" in pix_fmt:
        return 10
    if "12le" in pix_fmt or "p12" in pix_fmt:
        return 12
    return 8


def parse_vmaf_json(payload: dict) -> float:
    """Pull the pooled VMAF score from libvmaf's JSON output.

    Tries the modern ``pooled_metrics.vmaf.mean`` shape first, falls
    back to the older top-level ``VMAF score``. Raises ``ValueError``
    if neither is present.
    """
    pooled = payload.get("pooled_metrics") or {}
    vmaf = pooled.get("vmaf") or {}
    if "mean" in vmaf:
        return float(vmaf["mean"])
    if "VMAF score" in payload:
        return float(payload["VMAF score"])
    raise ValueError("vmaf JSON missing pooled_metrics.vmaf.mean")


def run_score(
    req: ScoreRequest,
    *,
    vmaf_bin: str = "vmaf",
    runner: object | None = None,
    workdir: Path | None = None,
) -> ScoreResult:
    """Drive the vmaf CLI for a single (ref, dist) pair.

    ``runner`` defaults to :func:`subprocess.run` but is injectable so
    tests can pin behaviour without spawning real processes.
    """
    runner_fn = runner or subprocess.run

    if workdir is None:
        ctx = tempfile.TemporaryDirectory()
        workdir_path = Path(ctx.name)
    else:
        ctx = None
        workdir_path = workdir
        workdir_path.mkdir(parents=True, exist_ok=True)

    json_path = workdir_path / "vmaf.json"
    cmd = build_vmaf_command(req, json_path, vmaf_bin=vmaf_bin)

    try:
        started = time.monotonic()
        completed = runner_fn(  # type: ignore[operator]
            cmd, capture_output=True, text=True, check=False
        )
        elapsed_ms = (time.monotonic() - started) * 1000.0

        stderr = getattr(completed, "stderr", "") or ""
        rc = int(getattr(completed, "returncode", 1))

        score = float("nan")
        if rc == 0 and json_path.exists():
            with json_path.open("r", encoding="utf-8") as fh:
                payload = json.load(fh)
            try:
                score = parse_vmaf_json(payload)
            except ValueError:
                rc = rc or 65

        match = _VMAF_VERSION_RE.search(stderr)
        version = match.group(1) if match else "unknown"

        tail = "\n".join(stderr.splitlines()[-8:])
        return ScoreResult(
            request=req,
            vmaf_score=score,
            score_time_ms=elapsed_ms,
            vmaf_binary_version=version,
            exit_status=rc,
            stderr_tail=tail,
        )
    finally:
        if ctx is not None:
            ctx.cleanup()

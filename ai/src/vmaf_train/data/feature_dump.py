# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Extract libvmaf feature vectors → parquet (C1 training input).

Drives the `vmaf` CLI in JSON mode, collects per-frame features listed in the
dataset manifest, and writes a parquet table suitable for pandas / polars
consumption. A feature vector is the union of whatever extractors the runner
configures (adm2, vif_scale0..3, motion2, etc.).
"""

from __future__ import annotations

import json
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import pandas as pd


@dataclass(frozen=True)
class Entry:
    key: str
    ref: Path
    dis: Path
    width: int
    height: int
    pix_fmt: str = "420"  # vmaf CLI accepts "420" | "422" | "444"
    bitdepth: int = 8
    mos: float | None = None


_FFMPEG_TO_VMAF_PIXFMT = {
    "yuv420p": "420",
    "yuv422p": "422",
    "yuv444p": "444",
    "yuvj420p": "420",
    "yuvj422p": "422",
    "yuvj444p": "444",
}


def _normalize_pixfmt(fmt: str) -> str:
    """Translate FFmpeg-style pixel format names to vmaf's 3-char variants."""
    return _FFMPEG_TO_VMAF_PIXFMT.get(fmt, fmt)


DEFAULT_FEATURES = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

DEFAULT_VMAF_BINARY = Path("core") / "build-cpu" / "tools" / "vmaf"

# vmaf's CLI takes feature-extractor names (``adm``, ``vif``, ``motion``) while
# the JSON output labels individual metrics (``adm2``, ``vif_scale0``, …).
# One extractor emits many metrics, so translate before building the argv.
_METRIC_TO_EXTRACTOR = {
    "adm2": "adm",
    "adm_scale0": "adm",
    "adm_scale1": "adm",
    "adm_scale2": "adm",
    "adm_scale3": "adm",
    "vif_scale0": "vif",
    "vif_scale1": "vif",
    "vif_scale2": "vif",
    "vif_scale3": "vif",
    "motion": "motion",
    "motion2": "motion",
}


def _extractors_for(metrics: tuple[str, ...]) -> list[str]:
    """Map requested metric names → the minimum set of extractor names."""
    seen: list[str] = []
    for m in metrics:
        ex = _METRIC_TO_EXTRACTOR.get(m, m)  # passthrough unknown names
        if ex not in seen:
            seen.append(ex)
    return seen


#: Wall-clock cap (seconds) for a single ``vmaf`` invocation under
#: :func:`_run_vmaf`. Generous because a long clip with all default features
#: on CPU can legitimately take a couple of minutes; we just need a finite
#: cap so a wedged vmaf process does not stall the whole feature-dump
#: pipeline.
_VMAF_RUN_TIMEOUT_S: float = 600.0


def _run_vmaf(
    binary: Path,
    entry: Entry,
    features: tuple[str, ...],
    *,
    timeout_s: float = _VMAF_RUN_TIMEOUT_S,
) -> dict:
    feat_args: list[str] = []
    for f in _extractors_for(features):
        feat_args += ["--feature", f]
    # The CLI's ``-o -`` stdout path is unreliable across builds, so route
    # output through a tempfile we own. The file is cleaned up on exit.
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        out_path = Path(tf.name)
    try:
        cmd = [
            str(binary),
            "-r",
            str(entry.ref),
            "-d",
            str(entry.dis),
            "-w",
            str(entry.width),
            "-h",
            str(entry.height),
            "-p",
            _normalize_pixfmt(entry.pix_fmt),
            "-b",
            str(entry.bitdepth),
            "--json",
            "-o",
            str(out_path),
            *feat_args,
        ]
        # ``timeout=`` so a hung vmaf process (corrupt YUV, deadlocked GPU
        # backend) cannot wedge the feature-dump worker indefinitely.
        # Encoding is pinned to UTF-8 so the JSON parse does not depend on
        # the calling process's locale.
        subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=timeout_s)
        return json.loads(out_path.read_text(encoding="utf-8"))
    finally:
        out_path.unlink(missing_ok=True)


def _lookup_feature(metrics: dict, name: str) -> float | None:
    """vmaf's integer kernels emit `integer_<name>` in JSON; accept either.

    Trying the unprefixed name first keeps backwards compatibility with
    any upstream JSON we might consume, and the `integer_` fallback
    matches the shipping CPU path where only the fixed-point kernels run.
    """
    if name in metrics:
        return metrics[name]
    return metrics.get(f"integer_{name}")


def dump_features(
    entries: list[Entry],
    out_parquet: Path,
    vmaf_binary: Path = DEFAULT_VMAF_BINARY,
    features: tuple[str, ...] = DEFAULT_FEATURES,
) -> Path:
    rows: list[dict] = []
    for e in entries:
        doc = _run_vmaf(vmaf_binary, e, features)
        for frame in doc.get("frames", []):
            row: dict[str, object] = {
                "key": e.key,
                "frame": frame.get("frameNum"),
                "mos": e.mos,
            }
            fmetrics = frame.get("metrics", {})
            for f in features:
                row[f] = _lookup_feature(fmetrics, f)
            rows.append(row)
    df = pd.DataFrame(rows)
    out_parquet.parent.mkdir(parents=True, exist_ok=True)
    df.to_parquet(out_parquet, index=False)
    return out_parquet

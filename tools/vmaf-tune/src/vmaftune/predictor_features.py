# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-shot feature extraction for the VMAF predictor.

Runtime feature extraction:

1.  One **probe encode** per shot via the codec adapter's
    :meth:`probe_args` — fast preset, fixed CRF/CQ. Parses the
    resulting frame sizes + bitrate from the probe encode's stderr to
    produce the complexity barometer the predictor learns from.
2.  Optional **signalstats** pass via FFmpeg's ``signalstats`` filter
    for the luma/chroma + frame-difference signals. Free during the
    probe encode (we run it as a parallel filtergraph branch).
3.  Optional **saliency** mean / variance via the existing
    :mod:`vmaftune.saliency` module's ONNX model on the shot's centre
    frame. Skipped when ``onnxruntime`` is unavailable.

The probe encode is the only mandatory step — when signalstats /
saliency aren't available the corresponding fields stay at zero, the
predictor handles that gracefully (the analytical fallback uses only
``probe_bitrate_kbps`` + the structural metadata).

Subprocess-boundary policy: every ``ffmpeg``/``ffprobe`` invocation
goes through an injectable ``runner`` argument so unit tests can mock
the subprocess without spawning a real process.
"""

from __future__ import annotations

import dataclasses
import json
import re
import shutil
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .codec_adapters import get_adapter
from .per_shot import Shot
from .predictor import ShotFeatures

# Subprocess-runner protocol — same shape as ``score.run_score`` /
# ``encode.run_encode``. Tests inject a stub.
SubprocessRunner = Callable[..., subprocess.CompletedProcess[str]]

# Regexes that scrape FFmpeg's stderr for the signals we want. FFmpeg's
# stderr layout is unstable across major versions, but these two
# patterns have held since FFmpeg 4.x and are the cheapest way to read
# the probe stats without `-f null - 2>>log` dance every codec.
_BITRATE_RE = re.compile(r"bitrate=\s*([\d.]+)kbits/s", re.IGNORECASE)
_FRAME_TYPES_RE = re.compile(
    r"Side data:\s+frame I:\s*(\d+).*?frame P:\s*(\d+).*?frame B:\s*(\d+)",
    re.DOTALL,
)


@dataclasses.dataclass(frozen=True)
class FeatureExtractorConfig:
    """Optional knobs for :func:`extract_features`.

    The defaults match the predictor PR's reference setup; tweak only
    when the host doesn't have the optional ML stack or when an
    operator wants to skip a specific feature pass.
    """

    ffmpeg_bin: str = "ffmpeg"
    ffprobe_bin: str = "ffprobe"
    use_signalstats: bool = True
    use_saliency: bool = False
    saliency_model: Path | None = None
    saliency_frame_samples: int = 8
    # Cap probe encode runtime — for very long shots we sample the
    # first N frames rather than encoding the entire shot. The
    # complexity signal saturates after a few seconds.
    probe_max_frames: int = 240


def extract_features(
    shot: Shot,
    source: Path,
    codec: str,
    *,
    config: FeatureExtractorConfig | None = None,
    runner: SubprocessRunner | None = None,
) -> ShotFeatures:
    """Extract a :class:`ShotFeatures` vector for ``shot`` of ``source``.

    Spawns the probe encode (and optionally signalstats / saliency).
    All subprocess calls go through ``runner`` for testability.
    """
    cfg = config or FeatureExtractorConfig()
    run = runner or subprocess.run

    width, height, fps = _probe_video_geometry(source, cfg, run)
    probe_stats = _run_probe_encode(shot, source, codec, cfg, run)
    if cfg.use_signalstats:
        sig_stats = _run_signalstats(shot, source, cfg, run)
    else:
        sig_stats = _SignalStats()
    if cfg.use_saliency:
        sal = _compute_saliency(shot, source, cfg, run, width=width, height=height, fps=fps)
    else:
        sal = (0.0, 0.0)

    return ShotFeatures(
        probe_bitrate_kbps=probe_stats.bitrate_kbps,
        probe_i_frame_avg_bytes=probe_stats.i_frame_avg_bytes,
        probe_p_frame_avg_bytes=probe_stats.p_frame_avg_bytes,
        probe_b_frame_avg_bytes=probe_stats.b_frame_avg_bytes,
        saliency_mean=sal[0],
        saliency_var=sal[1],
        frame_diff_mean=sig_stats.frame_diff_mean,
        y_avg=sig_stats.y_avg,
        y_var=sig_stats.y_var,
        shot_length_frames=shot.length,
        fps=fps,
        width=width,
        height=height,
    )


# ---- internals ------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class _ProbeStats:
    bitrate_kbps: float = 0.0
    i_frame_avg_bytes: float = 0.0
    p_frame_avg_bytes: float = 0.0
    b_frame_avg_bytes: float = 0.0


@dataclasses.dataclass(frozen=True)
class _SignalStats:
    frame_diff_mean: float = 0.0
    y_avg: float = 0.0
    y_var: float = 0.0


def _probe_video_geometry(
    source: Path,
    cfg: FeatureExtractorConfig,
    run: SubprocessRunner,
) -> tuple[int, int, float]:
    """Read width, height, fps from ffprobe."""
    if shutil.which(cfg.ffprobe_bin) is None and run is subprocess.run:
        # ffprobe missing on PATH — return zeros and let the predictor's
        # analytical fallback handle the missing structural metadata.
        return (0, 0, 0.0)
    completed = run(
        [
            cfg.ffprobe_bin,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,r_frame_rate",
            "-of",
            "json",
            str(source),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    rc = int(getattr(completed, "returncode", 1))
    if rc != 0:
        return (0, 0, 0.0)
    stdout = getattr(completed, "stdout", "") or "{}"
    try:
        payload: dict[str, Any] = json.loads(stdout)
    except json.JSONDecodeError:
        return (0, 0, 0.0)
    streams = payload.get("streams", [])
    if not streams:
        return (0, 0, 0.0)
    s = streams[0]
    width = int(s.get("width", 0))
    height = int(s.get("height", 0))
    fps = _parse_fps(str(s.get("r_frame_rate", "0/1")))
    return (width, height, fps)


def _parse_fps(rational: str) -> float:
    """Parse FFprobe's ``num/den`` rational frame rate."""
    if "/" not in rational:
        try:
            return float(rational)
        except ValueError:
            return 0.0
    num_str, den_str = rational.split("/", 1)
    try:
        num = float(num_str)
        den = float(den_str)
    except ValueError:
        return 0.0
    if den <= 0.0:
        return 0.0
    return num / den


def _run_probe_encode(
    shot: Shot,
    source: Path,
    codec: str,
    cfg: FeatureExtractorConfig,
    run: SubprocessRunner,
) -> _ProbeStats:
    """Run one probe encode of ``shot`` and parse FFmpeg's progress stderr.

    Output goes to ``-f null -`` so we don't waste disk on the probe.
    Frame-type counts come from ``-stats`` + ``-bsf:v showinfo``.
    """
    adapter = get_adapter(codec)
    probe_argv = list(adapter.probe_args())
    frames = min(shot.length, cfg.probe_max_frames)
    if frames <= 0:
        return _ProbeStats()

    with tempfile.TemporaryDirectory(prefix="vmaf-tune-probe-") as tmp:
        out_null = "/dev/null"
        # Use ``-vstats_file`` for per-frame size + type. Fallback to
        # bitrate-only parsing if vstats isn't honoured by the codec.
        vstats_path = Path(tmp) / "vstats.txt"
        cmd = [
            cfg.ffmpeg_bin,
            "-hide_banner",
            "-y",
            "-vstats_file",
            str(vstats_path),
            "-ss",
            f"{shot.start_frame}",
            "-i",
            str(source),
            "-frames:v",
            str(frames),
            *probe_argv,
            "-an",
            "-f",
            "null",
            out_null,
        ]
        completed = run(cmd, capture_output=True, text=True, check=False)
        rc = int(getattr(completed, "returncode", 1))
        if rc != 0:
            return _ProbeStats()
        stderr = getattr(completed, "stderr", "") or ""
        bitrate = _parse_bitrate(stderr)
        i_avg, p_avg, b_avg = _parse_frame_sizes(vstats_path)
        return _ProbeStats(
            bitrate_kbps=bitrate,
            i_frame_avg_bytes=i_avg,
            p_frame_avg_bytes=p_avg,
            b_frame_avg_bytes=b_avg,
        )


def _parse_bitrate(stderr: str) -> float:
    """Pull the ``bitrate=...kbits/s`` value from FFmpeg's stderr."""
    matches = _BITRATE_RE.findall(stderr)
    if not matches:
        return 0.0
    # Last bitrate line is the final overall summary; earlier ones are
    # progress reports during encode.
    try:
        return float(matches[-1])
    except ValueError:
        return 0.0


def _parse_frame_sizes(vstats: Path) -> tuple[float, float, float]:
    """Average I/P/B frame sizes (bytes) from FFmpeg's ``-vstats_file``."""
    if not vstats.exists():
        return (0.0, 0.0, 0.0)
    try:
        lines = vstats.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return (0.0, 0.0, 0.0)
    sums = {"I": 0.0, "P": 0.0, "B": 0.0}
    counts = {"I": 0, "P": 0, "B": 0}
    # vstats lines look like:
    #   frame=    1 q= 0.0 PSNR= 0.00 ... type= I size= 12345 ...
    for line in lines:
        m_type = re.search(r"type=\s*(\w)", line)
        m_size = re.search(r"size=\s*(\d+)", line)
        if not m_type or not m_size:
            continue
        ftype = m_type.group(1)
        if ftype not in sums:
            continue
        try:
            size = float(m_size.group(1))
        except ValueError:
            continue
        sums[ftype] += size
        counts[ftype] += 1
    return (
        sums["I"] / max(counts["I"], 1) if counts["I"] else 0.0,
        sums["P"] / max(counts["P"], 1) if counts["P"] else 0.0,
        sums["B"] / max(counts["B"], 1) if counts["B"] else 0.0,
    )


def _run_signalstats(
    shot: Shot,
    source: Path,
    cfg: FeatureExtractorConfig,
    run: SubprocessRunner,
) -> _SignalStats:
    """Run FFmpeg's ``signalstats`` + ``tblend=difference`` to read luma + frame-diff."""
    frames = min(shot.length, cfg.probe_max_frames)
    if frames <= 0:
        return _SignalStats()
    cmd = [
        cfg.ffmpeg_bin,
        "-hide_banner",
        "-ss",
        f"{shot.start_frame}",
        "-i",
        str(source),
        "-frames:v",
        str(frames),
        "-vf",
        "signalstats,metadata=mode=print:file=-",
        "-f",
        "null",
        "/dev/null",
    ]
    completed = run(cmd, capture_output=True, text=True, check=False)
    rc = int(getattr(completed, "returncode", 1))
    if rc != 0:
        return _SignalStats()
    stdout = getattr(completed, "stdout", "") or ""
    return _parse_signalstats(stdout)


def _parse_signalstats(metadata: str) -> _SignalStats:
    """Parse FFmpeg's ``signalstats`` metadata output for YAVG and YDIF."""
    yavg_values: list[float] = []
    ydif_values: list[float] = []
    yvar_values: list[float] = []
    for line in metadata.splitlines():
        if "lavfi.signalstats.YAVG=" in line:
            yavg_values.append(_parse_metadata_float(line))
        elif "lavfi.signalstats.YDIF=" in line:
            ydif_values.append(_parse_metadata_float(line))
        elif "lavfi.signalstats.YHIGH=" in line or "lavfi.signalstats.YLOW=" in line:
            # Use YHIGH-YLOW range as a proxy for variance — cheaper
            # than asking signalstats for a full second-moment pass.
            yvar_values.append(_parse_metadata_float(line))
    return _SignalStats(
        y_avg=_mean(yavg_values),
        frame_diff_mean=_mean(ydif_values),
        y_var=_mean(yvar_values),
    )


def _parse_metadata_float(line: str) -> float:
    if "=" not in line:
        return 0.0
    _, _, raw = line.rpartition("=")
    try:
        return float(raw.strip())
    except ValueError:
        return 0.0


def _mean(values: list[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def _shot_start_arg(shot: Shot, fps: float) -> str:
    if fps > 0.0:
        return f"{shot.start_frame / fps:.6f}"
    return str(shot.start_frame)


def _compute_saliency(
    shot: Shot,
    source: Path,
    cfg: FeatureExtractorConfig,
    run: SubprocessRunner,
    *,
    width: int,
    height: int,
    fps: float,
) -> tuple[float, float]:
    """Run the saliency ONNX on a raw-YUV decode of ``shot``.

    Returns ``(mean, var)`` over the full saliency map. Returns
    ``(0.0, 0.0)`` if onnxruntime / numpy / the saliency module are
    unavailable so the harness degrades gracefully on minimal hosts.
    """
    frames = min(shot.length, cfg.probe_max_frames)
    if width <= 0 or height <= 0 or frames <= 0:
        return (0.0, 0.0)
    try:
        import numpy as np

        from . import saliency
    except ImportError:
        return (0.0, 0.0)
    if not hasattr(saliency, "compute_saliency_map"):
        return (0.0, 0.0)

    with tempfile.TemporaryDirectory(prefix="vmaf-tune-saliency-") as tmp:
        raw_path = Path(tmp) / "shot.yuv"
        cmd = [
            cfg.ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-ss",
            _shot_start_arg(shot, fps),
            "-i",
            str(source),
            "-frames:v",
            str(frames),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(raw_path),
        ]
        completed = run(cmd, capture_output=True, text=True, check=False)
        rc = int(getattr(completed, "returncode", 1))
        if rc != 0 or not raw_path.exists():
            return (0.0, 0.0)
        try:
            frame_samples = max(1, min(int(cfg.saliency_frame_samples), frames))
            smap = saliency.compute_saliency_map(
                raw_path,
                width,
                height,
                model_path=cfg.saliency_model,
                frame_samples=frame_samples,
            )
        except Exception:  # pragma: no cover — saliency is best-effort
            return (0.0, 0.0)
    if smap is None:
        return (0.0, 0.0)
    arr = np.asarray(smap, dtype=np.float32)
    if arr.size == 0:
        return (0.0, 0.0)
    return (float(arr.mean()), float(arr.var()))


__all__ = [
    "FeatureExtractorConfig",
    "SubprocessRunner",
    "extract_features",
]

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Perceptual video fingerprinting for near-duplicate result reuse (ADR-3372).

Samples video frames at 1 fps via FFmpeg, computes 256-bit PDQ hashes and frame
quality, drops frames with quality < 50, and matches clips against reference
fingerprints using the ADR-3372 refusal ladder (duration, aspect ratio,
minimum entropy, and multi-segment temporal matching).
"""

from __future__ import annotations

import dataclasses
import json
import math
import subprocess
import time
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

SubprocessRunner = Callable[..., subprocess.CompletedProcess[str]]

# Perceptual video fingerprinting constants (ADR-3372)
SAMPLE_FPS: float = 1.0
MIN_FRAME_QUALITY: int = 50
MAX_HAMMING_DISTANCE: int = 31
DURATION_TOLERANCE_SECONDS: float = 0.5
DURATION_TOLERANCE_RATIO: float = 0.005  # 0.5%
MIN_VALID_FRAMES: int = 30
SEGMENT_CENTRES: tuple[float, float, float] = (0.10, 0.50, 0.85)
SEGMENT_MATCH_RATIO: float = 0.85
TIMESTAMP_TOLERANCE_SECONDS: float = 1.0
SEGMENT_RADIUS_RATIO: float = 0.05
MAX_READ_FRAMES_MARGIN: int = 60
PROBE_TIMEOUT_SECONDS: float = 30.0

# Optional dependencies: pdqhash (>=0.2.8, MIT) + numpy
try:
    import numpy as np
    import pdqhash

    _DEPS_AVAILABLE = True
    _IMPORT_ERROR: str | None = None
except ImportError as _exc:
    np = None  # type: ignore[assignment]
    pdqhash = None
    _DEPS_AVAILABLE = False
    _IMPORT_ERROR = str(_exc)


def require_fingerprint_deps() -> None:
    """Raise ImportError naming extra if pdqhash or numpy is missing (ADR-3372)."""
    if not _DEPS_AVAILABLE:
        raise ImportError(
            "perceptual video fingerprinting requires the 'fingerprint' extra: "
            "install vmaf-tune[fingerprint] (missing pdqhash>=0.2.8 or numpy)"
        )


@dataclasses.dataclass(frozen=True)
class VideoFingerprint:
    """Perceptual video fingerprint container (ADR-3372)."""

    duration_s: float
    width: int
    height: int
    dar: str
    par: str
    frames: list[tuple[float, bytes, int]]

    def to_dict(self) -> dict[str, Any]:
        """Serialize fingerprint to JSON-compatible dict with hex hashes."""
        return {
            "duration_s": float(self.duration_s),
            "width": int(self.width),
            "height": int(self.height),
            "dar": str(self.dar),
            "par": str(self.par),
            "frames": [[float(t), h.hex(), int(q)] for t, h, q in self.frames],
        }

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> VideoFingerprint:
        """Deserialize fingerprint from dict."""
        frames: list[tuple[float, bytes, int]] = [
            (float(item[0]), bytes.fromhex(str(item[1])), int(item[2]))
            for item in data.get("frames", [])
        ]
        return cls(
            duration_s=float(data["duration_s"]),
            width=int(data["width"]),
            height=int(data["height"]),
            dar=str(data["dar"]),
            par=str(data["par"]),
            frames=frames,
        )


@dataclasses.dataclass(frozen=True)
class MatchResult:
    """Outcome of clip fingerprint comparison (ADR-3372)."""

    accepted: bool
    reason: str
    segment_ratios: tuple[float, float, float]


def hamming_distance(a: bytes, b: bytes) -> int:
    """Hamming distance between two 256-bit hashes (ADR-3372)."""
    if len(a) != len(b):
        raise ValueError(f"hash length mismatch: {len(a)} vs {len(b)}")
    return (int.from_bytes(a, "big") ^ int.from_bytes(b, "big")).bit_count()


def _frame_matches(
    fa: tuple[float, bytes, int],
    candidate_frames: Sequence[tuple[float, bytes, int]],
    timestamp_tolerance: float = TIMESTAMP_TOLERANCE_SECONDS,
    max_hamming: int = MAX_HAMMING_DISTANCE,
) -> bool:
    """Check if query frame fa matches any candidate within timestamp tolerance."""
    t_q, hash_q, _ = fa
    for t_c, hash_c, _ in candidate_frames:
        if (
            abs(t_c - t_q) <= timestamp_tolerance
            and hamming_distance(hash_q, hash_c) <= max_hamming
        ):
            return True
    return False


def _compute_segment_ratio(
    query_frames: Sequence[tuple[float, bytes, int]],
    ref_frames: Sequence[tuple[float, bytes, int]],
    center_ratio: float,
    duration_s: float,
    radius_ratio: float = SEGMENT_RADIUS_RATIO,
) -> float:
    """Compute matching frame ratio for a single segment (ADR-3372)."""
    center_t = center_ratio * duration_s
    half_span = max(1.0, duration_s * radius_ratio)
    seg_frames = [f for f in query_frames if abs(f[0] - center_t) <= half_span]
    if not seg_frames:
        return 0.0
    matched = sum(1 for f in seg_frames if _frame_matches(f, ref_frames))
    return matched / len(seg_frames)


def match(a: VideoFingerprint, b: VideoFingerprint) -> MatchResult:
    """Compare two fingerprints and evaluate the ADR-3372 refusal ladder."""
    # Step a: duration within max(0.5 s, 0.5%) and same DAR and PAR
    max_dur = max(a.duration_s, b.duration_s)
    dur_tol = max(DURATION_TOLERANCE_SECONDS, max_dur * DURATION_TOLERANCE_RATIO)
    if abs(a.duration_s - b.duration_s) > dur_tol:
        return MatchResult(accepted=False, reason="duration", segment_ratios=(0.0, 0.0, 0.0))
    if a.dar != b.dar or a.par != b.par:
        return MatchResult(accepted=False, reason="aspect", segment_ratios=(0.0, 0.0, 0.0))

    # Step b: each side >= 30 valid frames
    if len(a.frames) < MIN_VALID_FRAMES or len(b.frames) < MIN_VALID_FRAMES:
        return MatchResult(accepted=False, reason="too-few-frames", segment_ratios=(0.0, 0.0, 0.0))

    # Step c: segments centred at 10%, 50%, 85% of duration
    r0 = _compute_segment_ratio(a.frames, b.frames, SEGMENT_CENTRES[0], a.duration_s)
    r1 = _compute_segment_ratio(a.frames, b.frames, SEGMENT_CENTRES[1], a.duration_s)
    r2 = _compute_segment_ratio(a.frames, b.frames, SEGMENT_CENTRES[2], a.duration_s)
    ratios = (r0, r1, r2)

    if any(r < SEGMENT_MATCH_RATIO for r in ratios):
        return MatchResult(accepted=False, reason="segment", segment_ratios=ratios)

    return MatchResult(accepted=True, reason="match", segment_ratios=ratios)


def _parse_aspect_fraction(raw: str | None) -> tuple[int, int]:
    """Parse aspect ratio string 'N:D' or 'N/D' to (num, den)."""
    if not raw or raw in ("0:1", "N/A"):
        return 1, 1
    sep = ":" if ":" in raw else ("/" if "/" in raw else "")
    if not sep:
        return 1, 1
    try:
        num, den = map(int, raw.split(sep, 1))
        return (num, den) if den > 0 else (1, 1)
    except ValueError:
        return 1, 1


def _normalize_aspect_ratios(
    width: int,
    height: int,
    sar_raw: str | None,
    dar_raw: str | None,
) -> tuple[str, str]:
    """Return canonical (dar, par) ratio strings (e.g. ('16:9', '1:1'))."""
    p_num, p_den = _parse_aspect_fraction(sar_raw)
    g_par = math.gcd(p_num, p_den)
    par = f"{p_num // g_par}:{p_den // g_par}"

    if dar_raw and dar_raw not in ("0:1", "N/A"):
        d_num, d_den = _parse_aspect_fraction(dar_raw)
    else:
        d_num, d_den = width * p_num, height * p_den
    d_den = max(1, d_den)
    g_dar = math.gcd(d_num, d_den)
    dar = f"{d_num // g_dar}:{d_den // g_dar}"
    return dar, par


def probe_fingerprint_meta(
    source: Path,
    ffprobe_bin: str = "ffprobe",
    runner: SubprocessRunner | None = None,
    timeout: float = PROBE_TIMEOUT_SECONDS,
) -> tuple[int, int, float, str, str]:
    """Probe (width, height, duration_s, dar, par) using ffprobe JSON pattern."""
    run = runner or subprocess.run
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,duration,sample_aspect_ratio,display_aspect_ratio:format=duration",
        "-of",
        "json",
        str(source),
    ]
    try:
        completed = run(cmd, capture_output=True, text=True, check=False, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"fingerprint: ffprobe timed out for {source}") from exc
    if int(getattr(completed, "returncode", 1)) != 0:
        raise RuntimeError(f"fingerprint: ffprobe failed for {source}")
    try:
        payload = json.loads(getattr(completed, "stdout", "") or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"fingerprint: ffprobe output invalid for {source}") from exc

    streams = payload.get("streams") or []
    if not streams:
        raise RuntimeError(f"fingerprint: no video stream found in {source}")
    s = streams[0]
    w = int(s.get("width") or 0)
    h = int(s.get("height") or 0)
    if w <= 0 or h <= 0:
        raise RuntimeError(f"fingerprint: invalid geometry {w}x{h} for {source}")

    dur_str = s.get("duration") or (payload.get("format") or {}).get("duration") or "0.0"
    try:
        duration_s = float(dur_str)
    except ValueError:
        duration_s = 0.0

    sar = s.get("sample_aspect_ratio")
    dar_val = s.get("display_aspect_ratio")
    dar, par = _normalize_aspect_ratios(w, h, sar, dar_val)
    return w, h, duration_s, dar, par


def _read_pdq_frames(
    proc: subprocess.Popen[bytes],
    w: int,
    h: int,
    max_frames: int,
    deadline: float,
) -> list[tuple[float, bytes, int]]:
    """Stream RGB24 frames from ffmpeg and extract PDQ hashes above quality threshold.

    Raises ``RuntimeError`` when the monotonic ``deadline`` passes before the
    stream ends (HISS-02: the read loop is bounded in frames and in time).
    """
    frame_size = w * h * 3
    valid_frames: list[tuple[float, bytes, int]] = []
    try:
        for idx in range(max_frames):
            if time.monotonic() > deadline:
                raise RuntimeError("fingerprint: frame sampling timed out")
            raw = proc.stdout.read(frame_size) if proc.stdout else b""
            if len(raw) < frame_size:
                break
            frame_arr = np.frombuffer(raw, dtype=np.uint8).reshape((h, w, 3))
            hash_bits, quality = pdqhash.compute(frame_arr)
            if quality >= MIN_FRAME_QUALITY:
                hash_bytes = np.packbits(hash_bits).tobytes()
                valid_frames.append((float(idx) / SAMPLE_FPS, hash_bytes, int(quality)))
    finally:
        if proc.stdout:
            proc.stdout.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5.0)
        else:
            proc.wait(timeout=5.0)
    return valid_frames


def compute_fingerprint(
    source: Path,
    *,
    ffmpeg_bin: str = "ffmpeg",
    ffprobe_bin: str = "ffprobe",
    timeout: float = 120.0,
    runner: SubprocessRunner | None = None,
) -> VideoFingerprint:
    """Sample video at 1 fps via FFmpeg and compute PDQ frame hashes (ADR-3372)."""
    require_fingerprint_deps()
    deadline = time.monotonic() + timeout
    w, h, duration_s, dar, par = probe_fingerprint_meta(
        source, ffprobe_bin, runner, timeout=min(timeout, PROBE_TIMEOUT_SECONDS)
    )

    cmd = [
        ffmpeg_bin,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(source),
        "-vf",
        f"fps={int(SAMPLE_FPS)}",
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    max_frames = max(100, int(duration_s * SAMPLE_FPS) + MAX_READ_FRAMES_MARGIN)
    valid_frames = _read_pdq_frames(proc, w, h, max_frames, deadline)

    resolved_dur = duration_s if duration_s > 0.0 else (float(len(valid_frames)) / SAMPLE_FPS)
    return VideoFingerprint(
        duration_s=resolved_dur,
        width=w,
        height=h,
        dar=dar,
        par=par,
        frames=valid_frames,
    )

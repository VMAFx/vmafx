# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Content-aware video classification for encoder and VMAF routing (ADR-0618, ADR-3374).

Extracts spatial information (SI) and temporal information (TI) via the ITU-T P.910
filter in FFmpeg, probes container geometry, dynamic range (HDR/SDR), and optionally
invokes a local vision-language model (VLM) for genre tagging.

Per ADR-3374, genre classification fails by name (raising VisionBackendUnavailable)
when a vision backend is unconfigured or unsupported; it never silently coerces to
"unknown". Video frames are strictly local-only (loopback / RFC 1918).
"""

from __future__ import annotations

import base64
import dataclasses
import hashlib
import ipaddress
import json
import logging
import math
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any, Protocol, runtime_checkable

from .hdr import HdrInfo, detect_hdr
from .jsonio import write_json_strict

_LOG = logging.getLogger(__name__)

# ITU-T Recommendation P.910 (07/2008), Annex B: Temporal Information (TI)
# Thresholds for motion classification:
TI_STATIC_THRESHOLD: float = 1.0  # TI < 1.0 -> static (still scene / slides)
TI_LOW_THRESHOLD: float = 12.0  # 1.0 <= TI < 12.0 -> low motion (talking head / slow pan)
TI_HIGH_THRESHOLD: float = 30.0  # 12.0 <= TI < 30.0 -> high motion (normal action)
# TI >= 30.0 -> very_high (sports / fast motion / camera shake)

# ITU-T Recommendation P.910, Annex B: Spatial Information (SI)
# Thresholds for spatial detail classification:
SI_LOW_THRESHOLD: float = 50.0  # SI < 50.0 -> low (flat / smooth graphics)
SI_MEDIUM_THRESHOLD: float = 100.0  # 50.0 <= SI < 100.0 -> medium
# SI >= 100.0 -> high (fine textures / sharp edges)

GENRE_LABELS: tuple[str, ...] = ("movie", "tv", "anime", "adult", "sports", "unknown")
CLASSIFIER_VERSION: int = 1

DEFAULT_PROBE_TIMEOUT: float = 30.0
DEFAULT_SITI_TIMEOUT: float = 120.0
DEFAULT_VLM_TIMEOUT: float = 30.0


class VisionBackendUnavailable(RuntimeError):
    """Raised when genre tagging requires a vision backend that is missing or unsupported."""


@dataclasses.dataclass(frozen=True)
class ContentTags:
    """Content classification tags for encoder and VMAF routing (ADR-0618, ADR-3374)."""

    si_mean: float
    si_p95: float
    ti_mean: float
    ti_p95: float
    hdr: HdrInfo | None
    resolution: tuple[int, int]
    fps: float
    duration: float
    motion_class: str
    detail_class: str
    genre: str | None
    genre_source: str

    def to_dict(self) -> dict[str, Any]:
        """Serialize ContentTags to dictionary."""
        return {
            "si_mean": float(self.si_mean),
            "si_p95": float(self.si_p95),
            "ti_mean": float(self.ti_mean),
            "ti_p95": float(self.ti_p95),
            "hdr": dataclasses.asdict(self.hdr) if self.hdr is not None else None,
            "resolution": [int(self.resolution[0]), int(self.resolution[1])],
            "fps": float(self.fps),
            "duration": float(self.duration),
            "motion_class": str(self.motion_class),
            "detail_class": str(self.detail_class),
            "genre": self.genre,
            "genre_source": str(self.genre_source),
        }

    @classmethod
    def from_dict(cls, data: Mapping[str, Any]) -> ContentTags:
        """Deserialize ContentTags from dictionary."""
        raw_hdr = data.get("hdr")
        hdr = HdrInfo(**raw_hdr) if isinstance(raw_hdr, dict) else None
        res_raw = data.get("resolution", [0, 0])
        res = (int(res_raw[0]), int(res_raw[1]))
        return cls(
            si_mean=float(data["si_mean"]),
            si_p95=float(data["si_p95"]),
            ti_mean=float(data["ti_mean"]),
            ti_p95=float(data["ti_p95"]),
            hdr=hdr,
            resolution=res,
            fps=float(data.get("fps", 0.0)),
            duration=float(data.get("duration", 0.0)),
            motion_class=str(data["motion_class"]),
            detail_class=str(data["detail_class"]),
            genre=data.get("genre"),
            genre_source=str(data.get("genre_source", "not-run")),
        )


def _percentile(values: Sequence[float], p: float) -> float:
    """Compute p-th percentile with linear interpolation."""
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return float(s[int(k)])
    return float(s[int(f)] * (c - k) + s[int(c)] * (k - f))


def classify_motion(ti: float) -> str:
    """Classify motion from TI according to ITU-T P.910 thresholds."""
    if ti < TI_STATIC_THRESHOLD:
        return "static"
    if ti < TI_LOW_THRESHOLD:
        return "low"
    if ti < TI_HIGH_THRESHOLD:
        return "high"
    return "very_high"


def classify_detail(si: float) -> str:
    """Classify spatial detail from SI according to ITU-T P.910 thresholds."""
    if si < SI_LOW_THRESHOLD:
        return "low"
    if si < SI_MEDIUM_THRESHOLD:
        return "medium"
    return "high"


def _parse_siti_output(stdout: str, stderr: str) -> tuple[list[float], list[float]]:
    """Parse lavfi.siti.si and lavfi.siti.ti entries from ffmpeg metadata output."""
    si_vals: list[float] = []
    ti_vals: list[float] = []
    lines = (stdout + "\n" + stderr).splitlines()
    max_lines = min(len(lines), 1_000_000)
    for i in range(max_lines):
        line = lines[i].strip()
        if line.startswith("lavfi.siti.si="):
            try:
                si_vals.append(float(line.split("=", 1)[1]))
            except ValueError:
                continue
        elif line.startswith("lavfi.siti.ti="):
            try:
                ti_vals.append(float(line.split("=", 1)[1]))
            except ValueError:
                continue
    return si_vals, ti_vals


def extract_siti(
    source: Path,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: Callable[..., Any] | None = None,
    timeout: float = DEFAULT_SITI_TIMEOUT,
) -> tuple[float, float, float, float]:
    """Extract (si_mean, si_p95, ti_mean, ti_p95) using FFmpeg siti filter."""
    run = runner or subprocess.run
    cmd = [
        ffmpeg_bin,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(source),
        "-vf",
        "siti,metadata=print:file=-",
        "-f",
        "null",
        "-",
    ]
    try:
        completed = run(cmd, capture_output=True, text=True, check=False, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError) as exc:
        raise RuntimeError(f"classify: ffmpeg siti extraction failed for {source}: {exc}") from exc

    if int(getattr(completed, "returncode", 1)) != 0:
        raise RuntimeError(f"classify: ffmpeg siti extraction returned non-zero for {source}")
    si_vals, ti_vals = _parse_siti_output(
        getattr(completed, "stdout", "") or "", getattr(completed, "stderr", "") or ""
    )
    if not si_vals or not ti_vals:
        raise RuntimeError(f"classify: ffmpeg siti filter produced no SI/TI values for {source}")

    si_mean = float(sum(si_vals) / len(si_vals))
    si_p95 = _percentile(si_vals, 95.0)
    ti_mean = float(sum(ti_vals) / len(ti_vals))
    ti_p95 = _percentile(ti_vals, 95.0)
    return (si_mean, si_p95, ti_mean, ti_p95)


def _parse_fps(rational: str) -> float:
    """Parse rational fps string num/den."""
    if "/" not in rational:
        try:
            return float(rational)
        except ValueError:
            return 0.0
    num_s, den_s = rational.split("/", 1)
    try:
        num, den = float(num_s), float(den_s)
        return num / den if den > 0 else 0.0
    except ValueError:
        return 0.0


def probe_source_metadata(
    source: Path,
    *,
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., Any] | None = None,
    timeout: float = DEFAULT_PROBE_TIMEOUT,
) -> tuple[tuple[int, int], float, float]:
    """Probe (resolution, fps, duration) via ffprobe."""
    run = runner or subprocess.run
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,r_frame_rate,duration:format=duration",
        "-of",
        "json",
        str(source),
    ]
    try:
        completed = run(cmd, capture_output=True, text=True, check=False, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError) as exc:
        raise RuntimeError(f"classify: ffprobe failed for {source}: {exc}") from exc
    if int(getattr(completed, "returncode", 1)) != 0:
        raise RuntimeError(f"classify: ffprobe returned non-zero for {source}")
    try:
        payload = json.loads(getattr(completed, "stdout", "") or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"classify: ffprobe returned invalid JSON for {source}") from exc

    streams = payload.get("streams") or []
    if not streams:
        raise RuntimeError(f"classify: no video stream found in {source}")
    s = streams[0]
    w = int(s.get("width") or 0)
    h = int(s.get("height") or 0)
    fps = _parse_fps(str(s.get("r_frame_rate", "0/1")))
    dur_str = s.get("duration") or (payload.get("format") or {}).get("duration") or "0.0"
    try:
        duration = float(dur_str)
    except ValueError:
        duration = 0.0
    return (w, h), fps, duration


@runtime_checkable
class VlmBackend(Protocol):
    """Protocol for VLM genre classifier."""

    endpoint_model: str

    def classify_frames(self, frames: Sequence[bytes]) -> str:
        """Classify video frames into one of GENRE_LABELS."""
        ...


class OpenAICompatVlm:
    """OpenAI-compatible VLM backend communicating with local endpoints."""

    def __init__(
        self,
        base_url: str,
        model: str,
        *,
        allow_remote: bool = False,
        timeout: float = DEFAULT_VLM_TIMEOUT,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.allow_remote = allow_remote
        self.timeout = timeout
        self.endpoint_model = f"{self.base_url}-{self.model}"
        self._probed = False
        self._validate_host()

    def _validate_host(self) -> None:
        """Enforce that frames never leave the local machine (loopback/RFC1918 only)."""
        if self.allow_remote:
            return
        parsed = urllib.parse.urlparse(self.base_url)
        host = parsed.hostname
        if not host:
            raise ValueError(f"Invalid VLM base_url {self.base_url!r}: missing host")
        if host.lower() in ("localhost", "127.0.0.1", "::1"):
            return
        try:
            ip = ipaddress.ip_address(host)
            if ip.is_loopback or ip.is_private:
                return
        except ValueError:
            pass
        raise ValueError(
            f"Remote VLM host {host!r} refused: frames must not leave local machine "
            "(loopback/RFC1918 only; set allow_remote=True to override)"
        )

    def probe(self) -> None:
        """Probe endpoint capability once with a minimal dummy image."""
        if self._probed:
            return
        dummy_png = (
            b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
            b"\x08\x06\x00\x00\x00\x1f\x15c4\x00\x00\x00\nIDATx\x9cc\x00\x01\x00\x00\x05"
            b"\x00\x01\r\n-\xb4\x00\x00\x00\x00IEND\xaeB`\x82"
        )
        b64 = base64.b64encode(dummy_png).decode("ascii")
        payload = {
            "model": self.model,
            "max_tokens": 1,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": "probe"},
                        {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b64}"}},
                    ],
                }
            ],
        }
        self._post(payload)
        self._probed = True

    def _post(self, payload: dict[str, Any]) -> dict[str, Any]:
        """Send JSON payload to /v1/chat/completions."""
        url = f"{self.base_url}/v1/chat/completions"
        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            err_body = exc.read().decode("utf-8", errors="replace")
            exc.close()
            combined = (str(exc) + " " + err_body).lower()
            if 400 <= exc.code < 500 or "not supported" in combined or "image" in combined:
                raise VisionBackendUnavailable(
                    f"Vision backend {self.base_url} ({self.model}) lacks vision support: "
                    f"HTTP {exc.code} {err_body} (config key: vlm_url)"
                ) from exc
            raise RuntimeError(f"VLM request failed: HTTP {exc.code} {err_body}") from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise VisionBackendUnavailable(
                f"Vision backend {self.base_url} ({self.model}) unavailable: {exc} "
                "(config key: vlm_url)"
            ) from exc

    def classify_frames(self, frames: Sequence[bytes]) -> str:
        """Send sampled frames to /v1/chat/completions and return genre label."""
        self.probe()
        content: list[dict[str, Any]] = [
            {
                "type": "text",
                "text": (
                    "Classify the video genre into exactly one of: "
                    f"{', '.join(GENRE_LABELS)}. "
                    "Respond with only the label in lowercase."
                ),
            }
        ]
        max_frames = min(len(frames), 10)
        for idx in range(max_frames):
            b64 = base64.b64encode(frames[idx]).decode("ascii")
            content.append(
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{b64}"}}
            )
        payload = {"model": self.model, "messages": [{"role": "user", "content": content}]}
        resp = self._post(payload)
        choices = resp.get("choices") or []
        if not choices:
            raise RuntimeError(f"VLM returned no choices: {resp}")
        raw = (
            str(choices[0].get("message", {}).get("content", "")).strip().lower().strip(".\"';:`\n")
        )
        if raw not in GENRE_LABELS:
            raise ValueError(
                f"VLM returned invalid genre label {raw!r}; expected one of {GENRE_LABELS}"
            )
        return raw


def sample_video_frames(
    source: Path,
    duration: float,
    count: int = 3,
    *,
    ffmpeg_bin: str = "ffmpeg",
    runner: Callable[..., Any] | None = None,
    timeout: float = DEFAULT_PROBE_TIMEOUT,
) -> list[bytes]:
    """Sample ``count`` JPEG frames spaced across duration."""
    run = runner or subprocess.run
    dur = max(duration, 1.0)
    timestamps = [dur * (i + 1) / (count + 1) for i in range(count)]
    frames: list[bytes] = []
    for ts in timestamps:
        cmd = [
            ffmpeg_bin,
            "-nostdin",
            "-v",
            "error",
            "-ss",
            f"{ts:.3f}",
            "-i",
            str(source),
            "-frames:v",
            "1",
            "-f",
            "image2pipe",
            "-c:v",
            "mjpeg",
            "-",
        ]
        try:
            completed = run(cmd, capture_output=True, text=False, check=False, timeout=timeout)
            out = getattr(completed, "stdout", b"")
            if out:
                frames.append(out)
        except (subprocess.TimeoutExpired, OSError) as exc:
            _LOG.warning("sample_video_frames: failed at ts=%s: %s", ts, exc)
            continue
    if not frames:
        raise RuntimeError(f"classify: no frames could be sampled from {source}")
    return frames


def compute_classifier_fingerprint(
    path: Path,
    *,
    runner: Callable[..., Any] | None = None,
) -> str:
    """Compute cache fingerprint for clip (ADR-3372 perceptual or SHA-256 fallback).

    When ``fingerprint.compute_fingerprint`` is available and the clip satisfies the
    ADR-3372 minimum frame requirement (>= 30 frames), perceptual video fingerprinting
    is reused. Otherwise, computes SHA-256 content digest.
    """
    try:
        from . import fingerprint

        if getattr(fingerprint, "_DEPS_AVAILABLE", False) and path.stat().st_size > 0:
            fp = fingerprint.compute_fingerprint(path, runner=runner)
            fp_json = json.dumps(fp.to_dict(), sort_keys=True)
            return f"pdq:{hashlib.sha256(fp_json.encode('utf-8')).hexdigest()[:32]}"
    except Exception as exc:  # any fingerprint failure selects the logged sha256 key
        _LOG.warning(
            "classify: perceptual fingerprint unavailable for %s (%s); using sha256", path, exc
        )

    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return f"sha256:{h.hexdigest()}"


def _read_cached_sidecar(sidecar: Path, expected_fp: str) -> ContentTags | None:
    """Read cached ContentTags from sidecar JSON if valid and matching fingerprint."""
    if not sidecar.exists():
        return None
    try:
        content = sidecar.read_text(encoding="utf-8")
        payload = json.loads(content)
        if not isinstance(payload, dict):
            raise ValueError("Sidecar JSON root must be an object")
        if payload.get("classifier_version") != CLASSIFIER_VERSION:
            return None
        if payload.get("fingerprint") != expected_fp:
            return None
        return ContentTags.from_dict(payload["tags"])
    except Exception as exc:
        _LOG.warning("classify: corrupt sidecar JSON in %s (%s); recomputing", sidecar, exc)
        return None


def _write_cached_sidecar(sidecar: Path, fp: str, tags: ContentTags) -> None:
    """Write ContentTags atomically to sidecar JSON."""
    payload = {
        "classifier_version": CLASSIFIER_VERSION,
        "fingerprint": fp,
        "tags": tags.to_dict(),
    }
    write_json_strict(sidecar, payload)


def _cache_satisfies(cached: ContentTags | None, *, genre: bool) -> bool:
    """True when a cached result answers the request (a genre request needs a run genre)."""
    if cached is None:
        return False
    if not genre:
        return True
    return cached.genre is not None and cached.genre_source != "not-run"


def _classify_genre(
    source: Path,
    duration: float,
    vlm: VlmBackend | None,
    *,
    ffmpeg_bin: str,
    runner: Callable[..., Any] | None,
) -> tuple[str | None, str]:
    """Return (label, source) from the VLM, or (None, "not-run") when genre was not asked."""
    if vlm is None:
        return None, "not-run"
    frames = sample_video_frames(source, duration, count=3, ffmpeg_bin=ffmpeg_bin, runner=runner)
    return vlm.classify_frames(frames), f"vlm:{vlm.endpoint_model}"


def classify(
    path: Path | str,
    *,
    genre: bool = False,
    vlm: VlmBackend | None = None,
    cache: bool = True,
    sidecar_path: Path | None = None,
    ffmpeg_bin: str = "ffmpeg",
    ffprobe_bin: str = "ffprobe",
    runner: Callable[..., Any] | None = None,
) -> ContentTags:
    """Classify video content complexity, dynamic range, and optional genre (ADR-0618, ADR-3374)."""
    source = Path(path)
    if not source.exists():
        raise FileNotFoundError(f"Source file not found: {source}")

    if genre and vlm is None:
        raise VisionBackendUnavailable(
            "Vision backend unavailable: genre classification requested but no VLM provided "
            "(missing vision backend; configure via --vlm-url or vlm config key)"
        )

    sidecar = sidecar_path or source.with_name(f"{source.name}.classify.json")
    fp = compute_classifier_fingerprint(source, runner=runner) if cache else ""

    cached_tags = _read_cached_sidecar(sidecar, fp) if cache else None
    if cached_tags is not None and _cache_satisfies(cached_tags, genre=genre):
        return cached_tags

    si_mean, si_p95, ti_mean, ti_p95 = extract_siti(source, ffmpeg_bin=ffmpeg_bin, runner=runner)
    hdr_info = detect_hdr(source, ffprobe_bin=ffprobe_bin, runner=runner)
    res, fps, duration = probe_source_metadata(source, ffprobe_bin=ffprobe_bin, runner=runner)
    motion = classify_motion(ti_p95)
    detail = classify_detail(si_mean)

    genre_label, genre_source = _classify_genre(
        source, duration, vlm if genre else None, ffmpeg_bin=ffmpeg_bin, runner=runner
    )

    tags = ContentTags(
        si_mean=si_mean,
        si_p95=si_p95,
        ti_mean=ti_mean,
        ti_p95=ti_p95,
        hdr=hdr_info,
        resolution=res,
        fps=fps,
        duration=duration,
        motion_class=motion,
        detail_class=detail,
        genre=genre_label,
        genre_source=genre_source,
    )

    if cache:
        _write_cached_sidecar(sidecar, fp, tags)

    return tags

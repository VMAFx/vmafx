# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for content-aware classifier (ADR-0618, ADR-3374).

Covers positive, negative, and boundary conditions (HISS-15):
1. Planted negative: genre=True with vlm=None must raise VisionBackendUnavailable,
   naming the missing backend and config key; never silently coerces to "unknown".
2. Positive: synthetic flat gray clip -> SI≈0, motion_class "static".
3. Positive: synthetic moving-noise clip -> high TI, motion_class "high" or "very_high".
4. Negative: stub VLM returning invalid genre ("documentary") -> error (no coercion).
5. Positive: stub VLM returning "adult" -> accepted as lawful genre category.
6. Security: OpenAICompatVlm with remote host (e.g. https://api.example.com) refused
   before urlopen can be called.
7. Capability: stub HTTP server returning 400 "image input not supported" raises
   VisionBackendUnavailable.
8. Cache resilience: corrupt sidecar JSON recomputes with logged warning naming the file.
9. Boundaries: exact threshold boundary values for TI (motion) and SI (detail).
10. CLI: vmaf-tune classify subcommand exits non-zero on missing vision backend and emits
    strict JSON when --json is passed.
"""

from __future__ import annotations

import http.server
import json
import logging
import shutil
import subprocess
import threading
import urllib.request
from collections.abc import Sequence
from pathlib import Path

import pytest

from vmaftune.classify import (
    GENRE_LABELS,
    TI_HIGH_THRESHOLD,
    TI_LOW_THRESHOLD,
    TI_STATIC_THRESHOLD,
    ContentTags,
    OpenAICompatVlm,
    VisionBackendUnavailable,
    classify,
    classify_detail,
    classify_motion,
)
from vmaftune.cli import main

_HAS_FFMPEG = shutil.which("ffmpeg") is not None and shutil.which("ffprobe") is not None
_requires_ffmpeg = pytest.mark.skipif(not _HAS_FFMPEG, reason="ffmpeg or ffprobe not on PATH")


def _generate_flat_gray_clip(out: Path, duration: float = 0.5) -> None:
    """Generate a solid flat gray synthetic video clip."""
    cmd = [
        "ffmpeg",
        "-y",
        "-nostdin",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"color=c=gray:s=320x240:d={duration}:r=10",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-pix_fmt",
        "yuv420p",
        str(out),
    ]
    subprocess.check_call(cmd, stderr=subprocess.DEVNULL, timeout=30)


def _generate_moving_noise_clip(out: Path, duration: float = 0.5) -> None:
    """Generate a high-motion moving noise synthetic video clip."""
    cmd = [
        "ffmpeg",
        "-y",
        "-nostdin",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"nullsrc=s=320x240:d={duration}:r=10,geq=random(1)*255:128:128",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-pix_fmt",
        "yuv420p",
        str(out),
    ]
    subprocess.check_call(cmd, stderr=subprocess.DEVNULL, timeout=30)


class StubVlm:
    """Test stub implementing VlmBackend protocol."""

    def __init__(self, label: str, endpoint_model: str = "stub-v1") -> None:
        self.label = label
        self.endpoint_model = endpoint_model

    def classify_frames(self, frames: Sequence[bytes]) -> str:
        if self.label not in GENRE_LABELS:
            raise ValueError(f"Stub label {self.label!r} not in GENRE_LABELS")
        return self.label


# ---------------------------------------------------------------------------
# Planted Negative: missing VLM must fail by name, never coerce to "unknown"
# ---------------------------------------------------------------------------


def test_planted_negative_missing_vlm_fails_by_name(tmp_path: Path):
    """genre=True with vlm=None must raise VisionBackendUnavailable naming backend & config."""
    clip = tmp_path / "dummy.mp4"
    clip.write_bytes(b"dummy-video-data")

    with pytest.raises(VisionBackendUnavailable) as exc_info:
        classify(clip, genre=True, vlm=None, cache=False)

    msg = str(exc_info.value)
    assert "Vision backend unavailable" in msg
    assert "--vlm-url" in msg or "vlm" in msg


# ---------------------------------------------------------------------------
# Positive: Synthetic video analysis (SI and TI)
# ---------------------------------------------------------------------------


@_requires_ffmpeg
def test_flat_gray_clip_low_si_static_motion(tmp_path: Path):
    """Flat gray video must yield SI≈0, low detail, and static motion class."""
    clip = tmp_path / "flat_gray.mp4"
    _generate_flat_gray_clip(clip, duration=0.5)

    tags = classify(clip, genre=False, cache=False)

    assert isinstance(tags, ContentTags)
    assert tags.si_mean < 1.0
    assert tags.si_p95 < 1.0
    assert tags.ti_mean < 1.0
    assert tags.motion_class == "static"
    assert tags.detail_class == "low"
    assert tags.genre is None
    assert tags.genre_source == "not-run"
    assert tags.resolution == (320, 240)


@_requires_ffmpeg
def test_moving_noise_clip_high_ti(tmp_path: Path):
    """High-contrast moving noise must yield high TI and high/very_high motion."""
    clip = tmp_path / "moving_noise.mp4"
    _generate_moving_noise_clip(clip, duration=0.5)

    tags = classify(clip, genre=False, cache=False)

    assert tags.ti_p95 > TI_LOW_THRESHOLD
    assert tags.motion_class in ("high", "very_high")


# ---------------------------------------------------------------------------
# VLM Hook: label validation, adult category, refusal of remote endpoints
# ---------------------------------------------------------------------------


@_requires_ffmpeg
def test_stub_vlm_invalid_genre_rejected(tmp_path: Path):
    """A VLM returning a label outside GENRE_LABELS must raise error, not coerce."""
    clip = tmp_path / "clip.mp4"
    _generate_flat_gray_clip(clip, duration=0.5)

    stub = StubVlm("documentary")
    with pytest.raises(ValueError, match=r"documentary"):
        classify(clip, genre=True, vlm=stub, cache=False)


@_requires_ffmpeg
def test_stub_vlm_adult_genre_accepted(tmp_path: Path):
    """A VLM returning lawful 'adult' category must be accepted into ContentTags."""
    clip = tmp_path / "clip.mp4"
    _generate_flat_gray_clip(clip, duration=0.5)

    stub = StubVlm("adult", endpoint_model="stub-endpoint-v1")
    tags = classify(clip, genre=True, vlm=stub, cache=False)

    assert tags.genre == "adult"
    assert tags.genre_source == "vlm:stub-endpoint-v1"


def test_openai_compat_vlm_remote_host_refused(monkeypatch: pytest.MonkeyPatch):
    """OpenAICompatVlm with remote host (e.g. api.example.com) refused before urlopen."""

    def fake_urlopen(*args, **kwargs):
        pytest.fail("urlopen must not be called when remote host is refused")

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)

    with pytest.raises(ValueError, match=r"Remote VLM host 'api.example.com' refused"):
        OpenAICompatVlm("https://api.example.com", "vlm-model")


def test_openai_compat_vlm_local_hosts_allowed():
    """Loopback and RFC1918 private IP addresses are allowed by default."""
    vlm_local = OpenAICompatVlm("http://127.0.0.1:8000", "vlm-model")
    assert vlm_local.base_url == "http://127.0.0.1:8000"

    vlm_host = OpenAICompatVlm("http://localhost:8000", "vlm-model")
    assert vlm_host.base_url == "http://localhost:8000"

    vlm_rfc1918 = OpenAICompatVlm("http://192.168.1.50:8000", "vlm-model")
    assert vlm_rfc1918.base_url == "http://192.168.1.50:8000"

    vlm_10 = OpenAICompatVlm("http://10.0.0.5:8000", "vlm-model")
    assert vlm_10.base_url == "http://10.0.0.5:8000"


# ---------------------------------------------------------------------------
# Capability Probe: HTTP 400 "image input not supported"
# ---------------------------------------------------------------------------


class _ImageNotSupportedHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"error": "image input not supported"}')

    def log_message(self, format, *args):
        pass  # suppress console logs during tests


def test_stub_http_server_400_image_not_supported():
    """Stub HTTP server on 127.0.0.1 returning 400 image not supported -> VisionBackendUnavailable."""
    server = http.server.HTTPServer(("127.0.0.1", 0), _ImageNotSupportedHandler)
    port = server.server_port
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        vlm = OpenAICompatVlm(f"http://127.0.0.1:{port}", "text-only-model")
        with pytest.raises(VisionBackendUnavailable) as exc_info:
            vlm.classify_frames([b"fake-image-bytes"])

        msg = str(exc_info.value)
        assert "lacks vision support" in msg or "unavailable" in msg
        assert "vlm_url" in msg
    finally:
        server.shutdown()
        server.server_close()


# ---------------------------------------------------------------------------
# Cache: Hit, persistence, and recompute on corrupt sidecar
# ---------------------------------------------------------------------------


@_requires_ffmpeg
def test_cache_hit_and_recompute_on_corrupt_sidecar(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
):
    """Valid sidecar reuses result; corrupt sidecar recomputes with warning naming file."""
    clip = tmp_path / "sample.mp4"
    _generate_flat_gray_clip(clip, duration=0.5)
    sidecar = tmp_path / "sample.mp4.classify.json"

    # Run 1: compute and write sidecar
    tags1 = classify(clip, sidecar_path=sidecar, cache=True)
    assert sidecar.exists()

    # Run 2: cache hit
    tags2 = classify(clip, sidecar_path=sidecar, cache=True)
    assert tags1 == tags2

    # Corrupt the sidecar JSON
    sidecar.write_text("{corrupt: json content[invalid", encoding="utf-8")

    # Run 3: detect corrupt sidecar, warn naming file, recompute
    caplog.clear()
    with caplog.at_level(logging.WARNING):
        tags3 = classify(clip, sidecar_path=sidecar, cache=True)

    assert tags3.motion_class == "static"
    # Verify logged warning names the sidecar file path
    assert "corrupt sidecar JSON" in caplog.text
    assert str(sidecar) in caplog.text

    # Verify sidecar has been repaired with valid JSON
    reloaded = json.loads(sidecar.read_text(encoding="utf-8"))
    assert reloaded["classifier_version"] == 1
    assert "tags" in reloaded


# ---------------------------------------------------------------------------
# Boundary Conditions: TI & SI thresholds
# ---------------------------------------------------------------------------


def test_ti_threshold_boundary_values():
    """Verify motion classification at exact ITU-T P.910 TI threshold boundaries."""
    assert classify_motion(0.0) == "static"
    assert classify_motion(TI_STATIC_THRESHOLD - 0.001) == "static"
    assert classify_motion(TI_STATIC_THRESHOLD) == "low"
    assert classify_motion(TI_LOW_THRESHOLD - 0.001) == "low"
    assert classify_motion(TI_LOW_THRESHOLD) == "high"
    assert classify_motion(TI_HIGH_THRESHOLD - 0.001) == "high"
    assert classify_motion(TI_HIGH_THRESHOLD) == "very_high"
    assert classify_motion(100.0) == "very_high"


def test_si_threshold_boundary_values():
    """Verify detail classification at exact ITU-T P.910 SI threshold boundaries."""
    assert classify_detail(0.0) == "low"
    assert classify_detail(49.999) == "low"
    assert classify_detail(50.0) == "medium"
    assert classify_detail(99.999) == "medium"
    assert classify_detail(100.0) == "high"
    assert classify_detail(250.0) == "high"


# ---------------------------------------------------------------------------
# CLI Integration Tests
# ---------------------------------------------------------------------------


def test_cli_missing_vision_backend_exits_nonzero(tmp_path: Path, capsys: pytest.CaptureFixture):
    """vmaf-tune classify <src> --genre without vision backend exits non-zero."""
    clip = tmp_path / "dummy.mp4"
    clip.write_bytes(b"dummy-clip-data")

    rc = main(["classify", str(clip), "--genre"])
    assert rc != 0
    err = capsys.readouterr().err
    assert "Vision backend unavailable" in err


@_requires_ffmpeg
def test_cli_json_output_strict(tmp_path: Path, capsys: pytest.CaptureFixture):
    """vmaf-tune classify <src> --json emits strict JSON matching ContentTags schema."""
    clip = tmp_path / "clip.mp4"
    _generate_flat_gray_clip(clip, duration=0.5)

    rc = main(["classify", str(clip), "--json"])
    assert rc == 0

    out = capsys.readouterr().out
    data = json.loads(out)
    assert "si_mean" in data
    assert "ti_mean" in data
    assert "motion_class" in data
    assert "detail_class" in data
    assert data["genre"] is None
    assert data["genre_source"] == "not-run"

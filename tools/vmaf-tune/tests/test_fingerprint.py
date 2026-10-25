# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for perceptual video fingerprinting and near-dup reuse (ADR-3372).

Covers positive, negative, and boundary conditions (HISS-15):
1. Planted negative: two clips sharing their first 1 MB with different content
   afterward -> sha256(1 MB) collides, match() refuses with "segment".
2. Planted positive: re-encode of the same source at different CRF and resolution
   (same DAR) -> accepted with >= 85% segment match.
3. Duration mismatch (off by 2s) -> refused with "duration".
4. Aspect ratio mismatch (different DAR) -> refused with "aspect".
5. Low frame count (< 30 valid frames, e.g. 25s clip) -> refused with "too-few-frames".
6. Boundary conditions tested exactly at tolerance limits.
7. Cache integration: exact miss + near-dup hit returns record; flag off -> no lookup.
8. Dependency gating: missing dependencies raise clear error naming vmaf-tune[fingerprint].
"""

from __future__ import annotations

import hashlib
import subprocess
from pathlib import Path

import pytest

from vmaftune import fingerprint
from vmaftune.cache import CachedResult, TuneCache, params_key
from vmaftune.fingerprint import (
    MIN_VALID_FRAMES,
    SEGMENT_MATCH_RATIO,
    VideoFingerprint,
    compute_fingerprint,
    hamming_distance,
    match,
    require_fingerprint_deps,
)


def _make_synthetic_frames(
    count: int,
    base_byte: int = 0xAA,
    quality: int = 100,
    fps: float = 1.0,
) -> list[tuple[float, bytes, int]]:
    """Generate synthetic frame hash sequence."""
    base_hash = bytes([base_byte] * 32)
    return [(float(i) / fps, base_hash, quality) for i in range(count)]


def _lavfi_encode(out: Path, source: str, seconds: int, codec_args: list[str]) -> None:
    """Encode a synthetic lavfi source with libx264 into ``out``."""
    argv = ["ffmpeg", "-y", "-f", "lavfi", "-i", source, "-t", str(seconds)]
    argv += ["-c:v", "libx264", *codec_args, "-pix_fmt", "yuv420p", str(out)]
    subprocess.check_call(argv, stderr=subprocess.DEVNULL, timeout=300)


def _concat(out: Path, head: Path, tail: Path) -> bytes:
    """Write ``head`` followed by ``tail`` to ``out`` and return the bytes."""
    data = head.read_bytes() + tail.read_bytes()
    out.write_bytes(data)
    return data


_CBR_ARGS = ["-b:v", "1500k", "-minrate", "1500k", "-maxrate", "1500k", "-bufsize", "1000k"]
_FAST_ARGS = ["-preset", "ultrafast"]


def _shared_head_pair(tmp_path: Path) -> tuple[Path, Path, bytes, bytes]:
    """Two clips sharing an 8 s (> 1 MiB) head, then 32 s of different content."""
    head = tmp_path / "head.ts"
    tail1 = tmp_path / "tail1.ts"
    tail2 = tmp_path / "tail2.ts"
    _lavfi_encode(head, "testsrc2=s=320x180:r=24", 8, _CBR_ARGS)
    _lavfi_encode(tail1, "testsrc2=s=320x180:r=24", 32, _FAST_ARGS)
    _lavfi_encode(tail2, "smptebars=s=320x180:r=24", 32, _FAST_ARGS)
    clip1 = tmp_path / "clip1.ts"
    clip2 = tmp_path / "clip2.ts"
    return clip1, clip2, _concat(clip1, head, tail1), _concat(clip2, head, tail2)


# ---------------------------------------------------------------------------
# Planted Negative: First 1 MB byte-identical, content diverges afterward
# ---------------------------------------------------------------------------


def test_planted_negative_first_megabyte_collision_refused(tmp_path: Path):
    """Two clips with byte-identical first 1 MB must collide on ADR-0618 but refuse match()."""
    clip1, clip2, b1, b2 = _shared_head_pair(tmp_path)

    one_mb = 1024 * 1024
    assert len(b1) > one_mb and len(b2) > one_mb
    assert b1[:one_mb] == b2[:one_mb]
    # ADR-0618 first-1MB key collides; full file hashes differ
    assert hashlib.sha256(b1[:one_mb]).digest() == hashlib.sha256(b2[:one_mb]).digest()
    assert hashlib.sha256(b1).digest() != hashlib.sha256(b2).digest()

    res = match(compute_fingerprint(clip1), compute_fingerprint(clip2))

    assert not res.accepted
    assert res.reason == "segment"
    # Segments at 50% (20s) and 85% (34s) fall into divergent tails
    assert res.segment_ratios[1] < SEGMENT_MATCH_RATIO
    assert res.segment_ratios[2] < SEGMENT_MATCH_RATIO


# ---------------------------------------------------------------------------
# Planted Positive: Re-encode at different CRF and resolution
# ---------------------------------------------------------------------------


def test_planted_positive_reencode_different_crf_and_resolution(tmp_path: Path):
    """Same visual source re-encoded with different CRF and scaled must be accepted."""
    clip1 = tmp_path / "high_q.mp4"
    clip2 = tmp_path / "low_q.mp4"
    source = "testsrc2=s=640x360:r=24"
    _lavfi_encode(clip1, source, 40, [*_FAST_ARGS, "-crf", "18"])
    # Same source scaled to 320x180 (same 16:9 DAR) at CRF 35
    _lavfi_encode(clip2, source, 40, ["-vf", "scale=320:180", *_FAST_ARGS, "-crf", "35"])
    assert clip1.read_bytes() != clip2.read_bytes()

    fp1 = compute_fingerprint(clip1)
    fp2 = compute_fingerprint(clip2)
    assert fp1.dar == fp2.dar == "16:9"
    assert fp1.par == fp2.par == "1:1"
    assert fp1.width != fp2.width

    res = match(fp1, fp2)
    assert res.accepted
    assert res.reason == "match"
    assert all(r >= SEGMENT_MATCH_RATIO for r in res.segment_ratios)


# ---------------------------------------------------------------------------
# Refusal Ladder Checks
# ---------------------------------------------------------------------------


def test_duration_mismatch_refused(tmp_path: Path):
    """Clips differing in duration by 2s (> max(0.5s, 0.5%)) must be refused."""
    c1 = tmp_path / "c1.mp4"
    c2 = tmp_path / "c2.mp4"

    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x180:r=24",
            "-t",
            "40",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            str(c1),
        ],
        stderr=subprocess.DEVNULL,
    )
    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x180:r=24",
            "-t",
            "42",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            str(c2),
        ],
        stderr=subprocess.DEVNULL,
    )

    fp1 = compute_fingerprint(c1)
    fp2 = compute_fingerprint(c2)

    res = match(fp1, fp2)
    assert not res.accepted
    assert res.reason == "duration"


def test_aspect_ratio_mismatch_refused(tmp_path: Path):
    """Clips with different display aspect ratio (16:9 vs 4:3) must be refused."""
    c1 = tmp_path / "c1.mp4"
    c2 = tmp_path / "c2.mp4"

    # 320x180 is 16:9
    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x180:r=24",
            "-t",
            "40",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            str(c1),
        ],
        stderr=subprocess.DEVNULL,
    )
    # 320x240 is 4:3
    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x240:r=24",
            "-t",
            "40",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            str(c2),
        ],
        stderr=subprocess.DEVNULL,
    )

    fp1 = compute_fingerprint(c1)
    fp2 = compute_fingerprint(c2)

    assert fp1.dar != fp2.dar
    res = match(fp1, fp2)
    assert not res.accepted
    assert res.reason == "aspect"


def test_too_few_frames_refused(tmp_path: Path):
    """Clips with < 30 valid frames (e.g. 25s clip @ 1 fps) must be refused."""
    c1 = tmp_path / "short.mp4"

    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=s=320x180:r=24",
            "-t",
            "25",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            str(c1),
        ],
        stderr=subprocess.DEVNULL,
    )

    fp = compute_fingerprint(c1)
    assert len(fp.frames) < MIN_VALID_FRAMES

    res = match(fp, fp)
    assert not res.accepted
    assert res.reason == "too-few-frames"


# ---------------------------------------------------------------------------
# Boundary Conditions (Exact Tolerances)
# ---------------------------------------------------------------------------


def test_boundary_duration_tolerance():
    """Duration tolerance boundary: max(0.5s, 0.5% * max_dur)."""
    frames_a = _make_synthetic_frames(40)
    frames_b = _make_synthetic_frames(40)

    # 40.0s: tol = max(0.5, 40.5 * 0.005) = 0.5s
    fp_base = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_a)
    fp_pass = VideoFingerprint(40.5, 320, 180, "16:9", "1:1", frames_b)
    fp_fail = VideoFingerprint(40.51, 320, 180, "16:9", "1:1", frames_b)

    assert match(fp_base, fp_pass).accepted
    res_fail = match(fp_base, fp_fail)
    assert not res_fail.accepted
    assert res_fail.reason == "duration"

    # 200.0s: 0.5% is 1.0s (> 0.5s)
    fp_long_base = VideoFingerprint(200.0, 320, 180, "16:9", "1:1", _make_synthetic_frames(205))
    fp_long_pass = VideoFingerprint(201.0, 320, 180, "16:9", "1:1", _make_synthetic_frames(205))
    fp_long_fail = VideoFingerprint(201.01, 320, 180, "16:9", "1:1", _make_synthetic_frames(205))

    assert match(fp_long_base, fp_long_pass).accepted
    assert match(fp_long_base, fp_long_fail).reason == "duration"


def test_boundary_valid_frames_count():
    """Exact boundary at MIN_VALID_FRAMES (30)."""
    fp_29 = VideoFingerprint(30.0, 320, 180, "16:9", "1:1", _make_synthetic_frames(29))
    fp_30 = VideoFingerprint(30.0, 320, 180, "16:9", "1:1", _make_synthetic_frames(30))

    assert match(fp_29, fp_30).reason == "too-few-frames"
    assert match(fp_30, fp_30).accepted


def test_boundary_hamming_distance():
    """Frame match threshold: Hamming <= 31 matches, >= 32 does not."""
    h_base = bytes([0x00] * 32)
    # 31 bits flipped
    h_31 = bytearray([0xFF] * 3 + [0xFE] + [0x00] * 28)
    # 32 bits flipped
    h_32 = bytearray([0xFF] * 4 + [0x00] * 28)

    assert hamming_distance(h_base, bytes(h_31)) == 31
    assert hamming_distance(h_base, bytes(h_32)) == 32

    # A pair with identical timestamps, one at dist 31, one at dist 32
    frames_base = [(float(i), h_base, 100) for i in range(40)]
    frames_31 = [(float(i), bytes(h_31), 100) for i in range(40)]
    frames_32 = [(float(i), bytes(h_32), 100) for i in range(40)]

    fp_base = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_base)
    fp_31 = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_31)
    fp_32 = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_32)

    assert match(fp_base, fp_31).accepted
    assert not match(fp_base, fp_32).accepted
    assert match(fp_base, fp_32).reason == "segment"


def test_boundary_segment_match_ratio():
    """Segment match boundary: exactly 85% matches, 84.9% fails."""

    # Distinct frame hash per second so frames at different timestamps don't collide
    def _sec_hash(sec: int) -> bytes:
        return hashlib.sha256(f"frame-{sec}".encode()).digest()

    h_mismatch = bytes([0xFF] * 32)

    # Frame timestamps from 0 to 40
    frames_ref = [(float(i), _sec_hash(i), 100) for i in range(41)]

    # In segment 0 (centered at 4.0s, range [2.0, 6.0], frames 2,3,4,5,6 = 5 frames)
    # All 5 match -> 100%
    frames_pass = [(float(i), _sec_hash(i), 100) for i in range(41)]

    fp_ref = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_ref)
    fp_pass = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_pass)
    assert match(fp_ref, fp_pass).accepted

    # Corrupt 1 frame in segment 0 (t=4.0): 4 of 5 match = 80% < 85%
    frames_fail = [(t, h if t != 4.0 else h_mismatch, q) for t, h, q in frames_pass]
    fp_fail = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", frames_fail)
    res_fail = match(fp_ref, fp_fail)
    assert not res_fail.accepted
    assert res_fail.reason == "segment"


# ---------------------------------------------------------------------------
# Cache Layer Near-Dup Lookup & Flag Gating
# ---------------------------------------------------------------------------


def _cached_result(artifact: Path) -> CachedResult:
    """A stored result for the cache near-dup tests."""
    return CachedResult(
        encode_size_bytes=12345,
        encode_time_ms=500.0,
        encoder_version="x264 0.164",
        ffmpeg_version="7.0",
        vmaf_score=93.5,
        vmaf_model="vmaf_v0.6.1",
        score_time_ms=200.0,
        vmaf_binary_version="libvmaf 3.0.0",
        artifact_path=artifact,
        row={"crf": 23, "vmaf_score": 93.5},
    )


def test_cache_near_dup_exact_miss_and_hit(tmp_path: Path):
    """Cache returns near-duplicate on exact miss when entry is present."""
    cache = TuneCache(path=tmp_path / "cache")
    artifact = tmp_path / "test_artifact.bin"
    artifact.write_bytes(b"dummy artifact bytes")

    fp_orig = VideoFingerprint(40.0, 320, 180, "16:9", "1:1", _make_synthetic_frames(40))
    # Re-encoded duplicate: slightly different duration (+0.2s), same aspect
    fp_near = VideoFingerprint(40.2, 640, 360, "16:9", "1:1", _make_synthetic_frames(40))

    pkey = params_key(
        encoder="libx264",
        preset="medium",
        crf=23,
        adapter_version="1",
        ffmpeg_version="7.0",
    )

    orig_result = _cached_result(artifact)

    # Store in cache with fingerprint sidecar
    key_orig = "exact_key_abc123"
    cache.put(
        key_orig,
        orig_result,
        artifact,
        fingerprint=fp_orig,
        params_key=pkey,
        src_sha256="orig_sha256_hash",
    )

    # 1. Exact miss for new source
    assert cache.get("new_source_key_xyz999") is None

    # 2. Near-duplicate lookup succeeds
    hit = cache.get_near_duplicate(fp_near, pkey)
    assert hit is not None
    cached_res, record = hit
    assert cached_res.vmaf_score == 93.5
    assert record.matched_src_sha256 == "orig_sha256_hash"
    assert record.segment_ratios == (1.0, 1.0, 1.0)
    assert cached_res.near_dup == record

    # 3. Lookup with different params_key misses
    other_pkey = params_key(
        encoder="libx264",
        preset="slow",
        crf=23,
        adapter_version="1",
        ffmpeg_version="7.0",
    )
    assert cache.get_near_duplicate(fp_near, other_pkey) is None


def test_dependency_error_names_extra(monkeypatch: pytest.MonkeyPatch):
    """When fingerprint dependencies are missing, raise clear error naming extra."""
    monkeypatch.setattr(fingerprint, "_DEPS_AVAILABLE", False)
    with pytest.raises(ImportError, match=r"vmaf-tune\[fingerprint\]"):
        require_fingerprint_deps()

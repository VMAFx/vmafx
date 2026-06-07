# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Fix B smoke tests — compare --no-bisect --crf-sweep mode.

ADR-0542: when --no-bisect is set the compare command encodes each
(codec, CRF) pair from --crf-sweep and reports results without running
a target-VMAF bisect. No real ffmpeg / vmaf binaries are required —
_encode_and_score is monkeypatched to return deterministic results.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import cli  # noqa: E402
from vmaftune.bisect import BisectResult  # noqa: E402

# ---------------------------------------------------------------------------
# Fake _encode_and_score for deterministic results
# ---------------------------------------------------------------------------

# Maps (codec, crf) -> (bitrate_kbps, vmaf_score) for 3 codecs x 4 CRFs.
_FAKE_TABLE: dict[tuple[str, int], tuple[float, float]] = {
    ("libx264", 18): (8000.0, 97.5),
    ("libx264", 23): (4500.0, 94.0),
    ("libx264", 28): (2500.0, 89.0),
    ("libx264", 33): (1400.0, 83.0),
    ("libx265", 18): (5000.0, 97.6),
    ("libx265", 23): (2800.0, 94.1),
    ("libx265", 28): (1600.0, 89.2),
    ("libx265", 33): (900.0, 83.1),
    ("libsvtav1", 18): (4200.0, 97.4),
    ("libsvtav1", 23): (2300.0, 94.2),
    ("libsvtav1", 28): (1300.0, 89.3),
    ("libsvtav1", 33): (750.0, 83.2),
}


def _make_fake_encode_and_score(monkeypatch) -> None:
    """Patch bisect._encode_and_score to return deterministic BisectResult rows."""

    def _fake(*, src, codec, adapter, preset, crf, width, height, **kwargs) -> BisectResult:
        key = (codec, int(crf))
        if key in _FAKE_TABLE:
            bitrate, vmaf = _FAKE_TABLE[key]
            return BisectResult(
                codec=codec,
                best_crf=int(crf),
                measured_vmaf=vmaf,
                bitrate_kbps=bitrate,
                encode_time_ms=500.0,
                n_iterations=1,
                encoder_version=f"{codec}-test",
                ok=True,
                error="",
            )
        return BisectResult(
            codec=codec,
            best_crf=int(crf),
            measured_vmaf=float("nan"),
            bitrate_kbps=float("nan"),
            encode_time_ms=0.0,
            n_iterations=0,
            ok=False,
            error=f"no fake for ({codec}, {crf})",
        )

    # Patch via the module object so the `from .bisect import _encode_and_score`
    # inside the _encode_one closure picks up the fake at call time.
    import vmaftune.bisect as _b  # noqa: PLC0415

    monkeypatch.setattr(_b, "_encode_and_score", _fake)


def _make_compare_args(
    src: Path,
    encoders: str = "libx264,libx265,libsvtav1",
    crf_sweep: str = "18,23,28,33",
    output: Path | None = None,
    **overrides,
) -> SimpleNamespace:
    base = {
        "src": src,
        "no_bisect": True,
        "crf_sweep": crf_sweep,
        "encoders": encoders,
        "target_vmaf": 92.0,
        "target_vmafs": "94,96,97,98",
        "format": "json",
        "output": output,
        "width": 320,
        "height": 240,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "duration": 10.0,
        "sample_clip_seconds": 0.0,
        "preset": None,
        "crf_min": None,
        "crf_max": None,
        "max_iterations": 8,
        "vmaf_model": "vmaf_v0.6.1",
        "score_backend": None,
        "ffmpeg_bin": "ffmpeg",
        "vmaf_bin": "vmaf",
        "predicate_module": None,
        "no_parallel": True,  # serial for deterministic ordering in tests
        "max_workers": None,
        "_framerate_was_default": False,
        "_duration_was_default": False,
        "_target_vmafs_was_default": True,
        "_target_vmaf_was_default": True,
    }
    base.update(overrides)
    return SimpleNamespace(**base)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_no_bisect_produces_codec_x_crf_rows(monkeypatch, tmp_path):
    """3 codecs x 4 CRFs = 12 rows in the JSON output."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(src, output=out)

    _make_fake_encode_and_score(monkeypatch)

    # Bypass encoder availability probe so no ffmpeg binary needed.
    from vmaftune import compare as _cmp  # noqa: PLC0415

    monkeypatch.setattr(_cmp, "probe_encoder_available", lambda enc, **kw: (True, ""))

    rc = cli._run_compare_crf_sweep(args, ["libx264", "libx265", "libsvtav1"])
    assert rc == 0, "expected exit 0"

    payload = json.loads(out.read_text())
    assert "rows" in payload
    assert len(payload["rows"]) == 12, f"expected 12 rows, got {len(payload['rows'])}"


def test_no_bisect_all_rows_ok(monkeypatch, tmp_path):
    """Every (codec, CRF) cell in the fake table must have ok=True."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(src, output=out)

    _make_fake_encode_and_score(monkeypatch)
    from vmaftune import compare as _cmp  # noqa: PLC0415

    monkeypatch.setattr(_cmp, "probe_encoder_available", lambda enc, **kw: (True, ""))

    cli._run_compare_crf_sweep(args, ["libx264", "libx265", "libsvtav1"])
    payload = json.loads(out.read_text())
    failed = [r for r in payload["rows"] if not r["ok"]]
    assert not failed, f"unexpected failed rows: {failed}"


def test_no_bisect_crf_values_in_rows(monkeypatch, tmp_path):
    """Each row carries the CRF it was encoded at."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(src, output=out, crf_sweep="18,28")

    _make_fake_encode_and_score(monkeypatch)
    from vmaftune import compare as _cmp  # noqa: PLC0415

    monkeypatch.setattr(_cmp, "probe_encoder_available", lambda enc, **kw: (True, ""))

    cli._run_compare_crf_sweep(args, ["libx264"])
    payload = json.loads(out.read_text())
    crfs = sorted({r["crf"] for r in payload["rows"]})
    assert crfs == [18, 28]


def test_no_bisect_schema_version_is_3(monkeypatch, tmp_path):
    """CRF-sweep output carries schema_version=3 and mode='crf_sweep'."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(src, output=out)

    _make_fake_encode_and_score(monkeypatch)
    from vmaftune import compare as _cmp  # noqa: PLC0415

    monkeypatch.setattr(_cmp, "probe_encoder_available", lambda enc, **kw: (True, ""))

    cli._run_compare_crf_sweep(args, ["libx264"])
    payload = json.loads(out.read_text())
    assert payload.get("schema_version") == 3
    assert payload.get("mode") == "crf_sweep"


def test_no_bisect_requires_crf_sweep_arg(tmp_path, capsys):
    """--no-bisect without --crf-sweep exits 2 with a helpful message."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    args = _make_compare_args(src, crf_sweep=None)

    rc = cli._run_compare_crf_sweep(args, ["libx264"])
    assert rc == 2
    captured = capsys.readouterr()
    assert "--crf-sweep" in captured.err


def test_no_bisect_cli_flag_dispatches_to_sweep(monkeypatch, tmp_path):
    """Passing --no-bisect via the full CLI routes to _run_compare_crf_sweep."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"

    _make_fake_encode_and_score(monkeypatch)
    from vmaftune import compare as _cmp  # noqa: PLC0415

    monkeypatch.setattr(_cmp, "probe_encoder_available", lambda enc, **kw: (True, ""))

    rc = cli.main(
        [
            "compare",
            "--src",
            str(src),
            "--encoders",
            "libx264",
            "--no-bisect",
            "--crf-sweep",
            "23,28",
            "--width",
            "320",
            "--height",
            "240",
            "--framerate",
            "24",
            "--duration",
            "5",
            "--format",
            "json",
            "--output",
            str(out),
            "--no-parallel",
        ]
    )
    assert rc == 0
    payload = json.loads(out.read_text())
    # 1 codec x 2 CRFs = 2 rows
    assert len(payload["rows"]) == 2


def test_no_bisect_unavailable_encoder_gets_error_rows(monkeypatch, tmp_path):
    """An unavailable encoder produces ok=False rows for each CRF."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(src, output=out, crf_sweep="23,28", encoders="libx264,h264_nvenc")

    _make_fake_encode_and_score(monkeypatch)
    from vmaftune import compare as _cmp  # noqa: PLC0415

    def _probe(enc, **kw):
        if enc == "h264_nvenc":
            return (
                False,
                "hardware encoder not available: h264_nvenc not compiled into ffmpeg",
            )
        return (True, "")

    monkeypatch.setattr(_cmp, "probe_encoder_available", _probe)

    cli._run_compare_crf_sweep(args, ["libx264", "h264_nvenc"])
    payload = json.loads(out.read_text())
    nvenc_rows = [r for r in payload["rows"] if r["codec"] == "h264_nvenc"]
    assert len(nvenc_rows) == 2  # one per CRF
    assert all(not r["ok"] for r in nvenc_rows)
    assert all("hardware encoder not available" in r["error"] for r in nvenc_rows)


def test_no_bisect_runtime_variant_uses_bound_ffmpeg(monkeypatch, tmp_path):
    """CRF-sweep mode keeps display tokens while encoding through the base adapter."""
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    out = tmp_path / "sweep.json"
    args = _make_compare_args(
        src,
        output=out,
        crf_sweep="23",
        encoders="libsvtav1,libsvtav1@svt-av1-hdr",
        ffmpeg_bin="/opt/ffmpeg-main",
        encoder_ffmpeg_bin=("libsvtav1@svt-av1-hdr=/opt/ffmpeg-hdr",),
    )

    calls: list[tuple[str, str]] = []

    def _fake(*, codec, crf, ffmpeg_bin, **kwargs) -> BisectResult:
        calls.append((codec, ffmpeg_bin))
        return BisectResult(
            codec=codec,
            best_crf=int(crf),
            measured_vmaf=94.0,
            bitrate_kbps=1500.0 if ffmpeg_bin.endswith("ffmpeg-hdr") else 1700.0,
            encode_time_ms=500.0,
            n_iterations=1,
            encoder_version=f"{codec}:{Path(ffmpeg_bin).name}",
            ok=True,
            error="",
        )

    import vmaftune.bisect as _b  # noqa: PLC0415

    monkeypatch.setattr(_b, "_encode_and_score", _fake)

    probes: list[tuple[str, str]] = []
    from vmaftune import compare as _cmp  # noqa: PLC0415

    def _probe(enc, **kw):
        probes.append((enc, kw["ffmpeg_bin"]))
        return (True, "")

    monkeypatch.setattr(_cmp, "probe_encoder_available", _probe)

    rc = cli._run_compare_crf_sweep(args, ["libsvtav1", "libsvtav1@svt-av1-hdr"])
    assert rc == 0
    assert calls == [
        ("libsvtav1", "/opt/ffmpeg-main"),
        ("libsvtav1", "/opt/ffmpeg-hdr"),
    ]
    assert probes == [
        ("libsvtav1", "/opt/ffmpeg-main"),
        ("libsvtav1", "/opt/ffmpeg-hdr"),
    ]

    payload = json.loads(out.read_text())
    by_codec = {row["codec"]: row for row in payload["rows"]}
    assert set(by_codec) == {"libsvtav1", "libsvtav1@svt-av1-hdr"}
    assert by_codec["libsvtav1@svt-av1-hdr"]["adapter"] == "libsvtav1"
    assert by_codec["libsvtav1@svt-av1-hdr"]["runtime_variant"] == "svt-av1-hdr"
    assert by_codec["libsvtav1@svt-av1-hdr"]["ffmpeg_bin"] == "/opt/ffmpeg-hdr"


def test_no_bisect_without_explicit_preset_uses_medium(monkeypatch, tmp_path):
    """Regression: compare --no-bisect --crf-sweep without --preset must not silently fail.

    Before ADR-1077 the --preset argparse default was None, which caused
    _encode_and_score to reject every encode (``None not in presets`` is
    always True) and return ok=False rows.  The fix sets default='medium'.
    This test drives the full CLI parse path to verify the default value
    is 'medium' when the flag is omitted.
    """
    from vmaftune import cli as _cli  # noqa: PLC0415

    # Build a parser instance and parse a minimal compare invocation that
    # omits --preset, then assert the default is 'medium', not None.
    src = tmp_path / "ref.yuv"
    src.write_bytes(b"\x00" * 100)
    args = _cli._build_parser().parse_args(
        [
            "compare",
            "--src",
            str(src),
            "--encoders",
            "libx264",
            "--no-bisect",
            "--crf-sweep",
            "23",
            "--width",
            "320",
            "--height",
            "240",
        ]
    )
    assert args.preset == "medium", (
        "compare --preset must default to 'medium' so that --no-bisect "
        "--crf-sweep does not silently produce all-failed rows (ADR-1077)"
    )

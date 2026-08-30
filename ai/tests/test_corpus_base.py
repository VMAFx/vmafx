# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for :mod:`ai.src.corpus.base` (ADR-0371).

Exercises the shared base class on a tiny synthetic corpus without
requiring ffprobe, curl, or any real dataset on disk.  All subprocess
calls are intercepted via the ``runner`` test seam.
"""

from __future__ import annotations

import json
import subprocess
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock

import pytest

from corpus.base import (
    CorpusIngestBase,
    RunStats,
    _parse_framerate,
    download_clip,
    load_progress,
    mark_done,
    mark_failed,
    normalise_clip_name,
    pick,
    probe_geometry,
    read_sha_index,
    save_progress,
    sha256_file,
    should_attempt,
    utc_now_iso,
    write_ingest_manifest,
)

# ---------------------------------------------------------------------------
# Synthetic corpus fixture
# ---------------------------------------------------------------------------


def _make_ffprobe_response(
    width: int = 1280,
    height: int = 720,
    fps: str = "30/1",
    duration: str = "5.0",
    pix_fmt: str = "yuv420p",
    codec: str = "h264",
    rc: int = 0,
) -> MagicMock:
    payload = {
        "streams": [
            {
                "width": width,
                "height": height,
                "avg_frame_rate": fps,
                "duration": duration,
                "pix_fmt": pix_fmt,
                "codec_name": codec,
            }
        ],
        "format": {"duration": duration},
    }
    proc = MagicMock(spec=subprocess.CompletedProcess)
    proc.returncode = rc
    proc.stdout = json.dumps(payload)
    proc.stderr = ""
    return proc


class _SyntheticIngest(CorpusIngestBase):
    """Minimal subclass that yields caller-supplied rows."""

    corpus_label = "test-corpus"

    def __init__(self, rows: list[tuple[Path, dict[str, Any]]], **kwargs: Any) -> None:
        self._rows = rows
        super().__init__(**kwargs)

    def iter_source_rows(self, clips_dir: Path) -> Iterator[tuple[Path, dict[str, Any]]]:
        yield from self._rows


# ---------------------------------------------------------------------------
# Unit tests — pure helpers
# ---------------------------------------------------------------------------


def test_utc_now_iso_format() -> None:
    ts = utc_now_iso()
    assert "T" in ts
    assert ts.endswith("+00:00")


def test_sha256_file(tmp_path: Path) -> None:
    f = tmp_path / "a.bin"
    f.write_bytes(b"hello")
    digest = sha256_file(f)
    assert len(digest) == 64
    assert digest == sha256_file(f)  # deterministic


def test_pick_case_insensitive() -> None:
    row: dict[str, str] = {"MOS": "3.5", "sd": "0.2"}
    assert pick(row, ("mos", "MOS")) == "3.5"
    assert pick(row, ("SD", "sd")) == "0.2"
    assert pick(row, ("MISSING",)) is None


def test_normalise_clip_name() -> None:
    assert normalise_clip_name("0001") == "0001.mp4"
    assert normalise_clip_name("0001.mp4") == "0001.mp4"
    assert normalise_clip_name("clip", suffix=".yuv") == "clip.yuv"


def test_parse_framerate_rational() -> None:
    assert _parse_framerate("30/1") == pytest.approx(30.0)
    assert _parse_framerate("24000/1001") == pytest.approx(23.976, rel=1e-3)
    assert _parse_framerate("0/0") == 0.0
    assert _parse_framerate("") == 0.0
    assert _parse_framerate("bad") == 0.0


# ---------------------------------------------------------------------------
# probe_geometry seam tests
# ---------------------------------------------------------------------------


def test_probe_geometry_success(tmp_path: Path) -> None:
    clip = tmp_path / "clip.mp4"
    clip.write_bytes(b"\x00")
    runner = MagicMock(return_value=_make_ffprobe_response())
    result = probe_geometry(clip, runner=runner)
    assert result is not None
    assert result["width"] == 1280
    assert result["height"] == 720
    assert result["framerate"] == pytest.approx(30.0)
    assert result["pix_fmt"] == "yuv420p"
    assert result["encoder_upstream"] == "h264"


def test_probe_geometry_bad_rc(tmp_path: Path) -> None:
    clip = tmp_path / "clip.mp4"
    clip.write_bytes(b"\x00")
    runner = MagicMock(return_value=_make_ffprobe_response(rc=1))
    assert probe_geometry(clip, runner=runner) is None


def test_probe_geometry_no_streams(tmp_path: Path) -> None:
    clip = tmp_path / "clip.mp4"
    clip.write_bytes(b"\x00")
    proc = MagicMock(spec=subprocess.CompletedProcess)
    proc.returncode = 0
    proc.stdout = json.dumps({"streams": [], "format": {}})
    proc.stderr = ""
    runner = MagicMock(return_value=proc)
    assert probe_geometry(clip, runner=runner) is None


# ---------------------------------------------------------------------------
# Progress state helpers
# ---------------------------------------------------------------------------


def test_load_save_progress_roundtrip(tmp_path: Path) -> None:
    p = tmp_path / "progress.json"
    state: dict[str, dict[str, Any]] = {}
    mark_done(state, "a.mp4")
    mark_failed(state, "b.mp4", "curl-rc=22")
    save_progress(p, state)
    loaded = load_progress(p)
    assert loaded["a.mp4"]["state"] == "done"
    assert loaded["b.mp4"]["state"] == "failed"
    assert loaded["b.mp4"]["reason"] == "curl-rc=22"


def test_load_progress_missing_file(tmp_path: Path) -> None:
    assert load_progress(tmp_path / "nonexistent.json") == {}


def test_should_attempt_logic(tmp_path: Path) -> None:
    clip = tmp_path / "clip.mp4"
    state: dict[str, dict[str, Any]] = {}
    # pending
    assert should_attempt(state, "clip.mp4", clip) is True
    # done + file missing -> re-fetch
    mark_done(state, "clip.mp4")
    assert should_attempt(state, "clip.mp4", clip) is True
    # done + file present -> skip
    clip.write_bytes(b"\x00")
    assert should_attempt(state, "clip.mp4", clip) is False
    # failed -> never retry
    mark_failed(state, "clip.mp4", "404")
    assert should_attempt(state, "clip.mp4", clip) is False


# ---------------------------------------------------------------------------
# read_sha_index
# ---------------------------------------------------------------------------


def test_read_sha_index(tmp_path: Path) -> None:
    jsonl = tmp_path / "out.jsonl"
    rows = [
        {"src_sha256": "aaa", "mos": 3.0},
        {"src_sha256": "bbb", "mos": 4.0},
        {"not_sha": "ccc"},  # should be ignored
        "malformed line\n",  # type: ignore[list-item]
    ]
    with jsonl.open("w") as f:
        for row in rows:
            if isinstance(row, str):
                f.write(row)
            else:
                f.write(json.dumps(row) + "\n")
    index = read_sha_index(jsonl)
    assert "aaa" in index
    assert "bbb" in index
    assert len(index) == 2


# ---------------------------------------------------------------------------
# CorpusIngestBase orchestrator
# ---------------------------------------------------------------------------


def _make_fake_clip(tmp_path: Path, name: str) -> Path:
    p = tmp_path / "clips" / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"fake video data for " + name.encode())
    return p


def test_corpus_ingest_base_writes_rows(tmp_path: Path) -> None:
    """Full orchestrator round-trip on a 3-clip synthetic corpus."""
    clips = [_make_fake_clip(tmp_path, f"clip_{i}.mp4") for i in range(3)]
    manifest_rows = [
        {"filename": c.name, "url": "", "mos": float(i + 1), "mos_std_dev": 0.1, "n_ratings": 10}
        for i, c in enumerate(clips)
    ]
    rows: list[tuple[Path, dict[str, Any]]] = list(zip(clips, manifest_rows, strict=True))

    output = tmp_path / "out.jsonl"
    runner = MagicMock(return_value=_make_ffprobe_response())

    ingest = _SyntheticIngest(
        rows=rows,
        corpus_dir=tmp_path,
        output=output,
        corpus_version="test-v1",
        runner=runner,
        now_fn=lambda: "2026-01-01T00:00:00+00:00",
        max_rows=None,
    )
    stats = ingest.run()

    assert stats.written == 3
    assert stats.skipped_broken == 0
    assert stats.dedups == 0

    written = [json.loads(line) for line in output.read_text().splitlines()]
    assert len(written) == 3
    assert all(r["corpus"] == "test-corpus" for r in written)
    assert all(r["corpus_version"] == "test-v1" for r in written)
    assert written[0]["mos"] == pytest.approx(1.0)


def test_corpus_ingest_base_dedup(tmp_path: Path) -> None:
    """Re-run on the same clips yields zero new rows (SHA dedup)."""
    clip = _make_fake_clip(tmp_path, "clip.mp4")
    row: dict[str, Any] = {
        "filename": clip.name,
        "url": "",
        "mos": 3.0,
        "mos_std_dev": 0.0,
        "n_ratings": 5,
    }
    output = tmp_path / "out.jsonl"
    runner = MagicMock(return_value=_make_ffprobe_response())

    def _make_ingest() -> _SyntheticIngest:
        return _SyntheticIngest(
            rows=[(clip, row)],
            corpus_dir=tmp_path,
            output=output,
            corpus_version="test-v1",
            runner=runner,
            now_fn=lambda: "2026-01-01T00:00:00+00:00",
            max_rows=None,
        )

    s1 = _make_ingest().run()
    assert s1.written == 1
    s2 = _make_ingest().run()
    assert s2.written == 0
    assert s2.dedups == 1


def test_corpus_ingest_base_max_rows(tmp_path: Path) -> None:
    clips = [_make_fake_clip(tmp_path, f"c{i}.mp4") for i in range(5)]
    rows = [
        (c, {"filename": c.name, "url": "", "mos": 3.0, "mos_std_dev": 0.0, "n_ratings": 0})
        for c in clips
    ]
    output = tmp_path / "out.jsonl"
    runner = MagicMock(return_value=_make_ffprobe_response())

    ingest = _SyntheticIngest(
        rows=rows,
        corpus_dir=tmp_path,
        output=output,
        corpus_version="v1",
        runner=runner,
        now_fn=lambda: "2026-01-01T00:00:00+00:00",
        max_rows=3,
    )
    stats = ingest.run()
    assert stats.written == 3


def test_corpus_ingest_base_zero_geometry_skipped(tmp_path: Path) -> None:
    clip = _make_fake_clip(tmp_path, "clip.mp4")
    row: dict[str, Any] = {
        "filename": clip.name,
        "url": "",
        "mos": 3.0,
        "mos_std_dev": 0.0,
        "n_ratings": 0,
    }
    output = tmp_path / "out.jsonl"
    runner = MagicMock(return_value=_make_ffprobe_response(width=0, height=0))

    ingest = _SyntheticIngest(
        rows=[(clip, row)],
        corpus_dir=tmp_path,
        output=output,
        corpus_version="v1",
        runner=runner,
        now_fn=lambda: "2026-01-01T00:00:00+00:00",
        max_rows=None,
    )
    stats = ingest.run()
    assert stats.written == 0
    assert stats.skipped_broken == 1


def test_write_ingest_manifest_records_run_provenance(tmp_path: Path) -> None:
    stats = RunStats()
    stats.written = 1
    manifest = tmp_path / "out.manifest.json"

    write_ingest_manifest(
        manifest,
        schema="test-corpus-jsonl-manifest-v1",
        entrypoint=Path(__file__),
        repo_root=Path(__file__).resolve().parents[2],
        argv=["--output", str(tmp_path / "out.jsonl")],
        args={"output": tmp_path / "out.jsonl"},
        corpus_label="test-corpus",
        stats=stats,
        inputs={"corpus_dir": tmp_path},
        outputs={"manifest": manifest},
        config={"max_rows": 1},
    )

    payload = json.loads(manifest.read_text(encoding="utf-8"))
    assert payload["schema"] == "test-corpus-jsonl-manifest-v1"
    assert payload["corpus"] == "test-corpus"
    assert payload["stats"]["written"] == 1
    assert payload["config"]["max_rows"] == 1
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


# ---------------------------------------------------------------------------
# download_clip seam test
# ---------------------------------------------------------------------------


def test_download_clip_success(tmp_path: Path) -> None:
    dest = tmp_path / "clip.mp4"
    part = dest.with_suffix(".mp4.part")

    def fake_runner(cmd, **_kwargs):
        part.write_bytes(b"video")
        proc = MagicMock()
        proc.returncode = 0
        proc.stderr = ""
        return proc

    ok, reason = download_clip(url="http://example.com/clip.mp4", dest=dest, runner=fake_runner)
    assert ok is True
    assert reason == ""
    assert dest.is_file()


def test_download_clip_no_url(tmp_path: Path) -> None:
    ok, reason = download_clip(url="", dest=tmp_path / "noop.mp4")
    assert ok is False
    assert "no-url" in reason

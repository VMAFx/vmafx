# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.konvid_1k_to_corpus_jsonl` (ADR-0325 Phase 1).

These tests exercise the adapter's pure-Python contract end-to-end on
synthetic fixtures. They do **not** require ffprobe to be installed,
nor do they need the KonViD-1k corpus on disk — every external call is
routed through the script's injectable ``runner`` seam.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
from pathlib import Path
from typing import Any

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "konvid_1k_to_corpus_jsonl.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("konvid_1k_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


KONVID = _load_module()


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _make_clip(path: Path, *, content: bytes | None = None) -> None:
    """Create a placeholder MP4 file (content is irrelevant under mocked ffprobe).

    Each placeholder gets a unique payload by default — the script's
    SHA-256 dedup keys on file content, so identical-byte placeholders
    would collapse multiple clips into one row.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    if content is None:
        content = f"konvid-1k-fixture:{path.name}".encode("utf-8")
    path.write_bytes(content)


def _make_csv(
    path: Path,
    rows: list[dict[str, Any]],
    *,
    header_aliases: dict[str, str] | None = None,
) -> None:
    """Write a synthetic KonViD-style attribute CSV.

    By default uses the canonical 2017-release column names
    ``file_name,MOS,SD,n,flickr_id``. Pass ``header_aliases`` to remap a
    standard key to an alternative spelling (e.g.
    ``{"MOS": "mos", "SD": "mos_std"}``) to exercise the alias matcher.
    """
    aliases = header_aliases or {}
    standard = ["file_name", "MOS", "SD", "n", "flickr_id"]
    headers = [aliases.get(k, k) for k in standard]

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        fh.write(",".join(headers) + "\n")
        for row in rows:
            fh.write(
                "{file_name},{MOS},{SD},{n},{flickr_id}\n".format(
                    file_name=row["file_name"],
                    MOS=row["MOS"],
                    SD=row.get("SD", ""),
                    n=row.get("n", ""),
                    flickr_id=row.get("flickr_id", ""),
                )
            )


def _scaffold_corpus(
    tmp_path: Path,
    *,
    clip_names: list[str],
    csv_rows: list[dict[str, Any]] | None = None,
    csv_filename: str = "KoNViD_1k_attributes.csv",
) -> Path:
    """Build a minimal ``.workingdir2/konvid-1k/``-shaped tree under tmp_path."""
    konvid_dir = tmp_path / "konvid-1k"
    videos_dir = konvid_dir / "KoNViD_1k_videos"
    metadata_dir = konvid_dir / "KoNViD_1k_metadata"
    videos_dir.mkdir(parents=True)
    metadata_dir.mkdir(parents=True)

    for name in clip_names:
        _make_clip(videos_dir / name)

    if csv_rows is None:
        csv_rows = [
            {"file_name": name, "MOS": 3.5 + 0.01 * i, "SD": 0.42, "n": 50, "flickr_id": str(i)}
            for i, name in enumerate(clip_names)
        ]
    _make_csv(metadata_dir / csv_filename, csv_rows)
    return konvid_dir


def _ffprobe_ok_payload(
    *, width: int = 960, height: int = 540, codec: str = "h264", pix_fmt: str = "yuv420p"
) -> str:
    return json.dumps(
        {
            "streams": [
                {
                    "width": width,
                    "height": height,
                    "r_frame_rate": "30/1",
                    "avg_frame_rate": "30/1",
                    "duration": "8.000",
                    "pix_fmt": pix_fmt,
                    "codec_name": codec,
                }
            ],
            "format": {"duration": "8.000"},
        }
    )


def _make_runner(
    behaviour: dict[str, tuple[int, str, str]] | None = None,
    *,
    default: tuple[int, str, str] = (
        0,
        _ffprobe_ok_payload(),
        "",
    ),
):
    """Return a fake ``subprocess.run`` keyed by clip basename in the cmd.

    ``behaviour[basename] = (returncode, stdout, stderr)``. Anything not
    keyed falls back to ``default`` (a successful 540p h264 probe).
    """
    behaviour = behaviour or {}

    def _runner(cmd, **_kw):
        # The clip path is the last positional arg in our ffprobe call.
        target = Path(cmd[-1]).name
        rc, stdout, stderr = behaviour.get(target, default)
        return subprocess.CompletedProcess(args=cmd, returncode=rc, stdout=stdout, stderr=stderr)

    return _runner


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


# ---------------------------------------------------------------------------
# 1. test_geometry_parse_from_ffprobe_json
# ---------------------------------------------------------------------------


def test_geometry_parse_from_ffprobe_json(tmp_path: Path) -> None:
    """One mocked ffprobe (vp9, 540p) → row's geometry fields match."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["clipA.mp4"])
    runner = _make_runner(
        default=(
            0,
            _ffprobe_ok_payload(width=960, height=540, codec="vp9", pix_fmt="yuv420p"),
            "",
        )
    )

    output = tmp_path / "out.jsonl"
    written, skipped, dedups = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner,
    )

    assert (written, skipped, dedups) == (1, 0, 0)
    rows = _read_jsonl(output)
    assert len(rows) == 1
    row = rows[0]
    assert row["src"] == "clipA.mp4"
    assert row["width"] == 960
    assert row["height"] == 540
    assert row["framerate"] == pytest.approx(30.0)
    assert row["pix_fmt"] == "yuv420p"
    assert row["encoder_upstream"] == "vp9"
    assert row["duration_s"] == pytest.approx(8.0)


# ---------------------------------------------------------------------------
# 2. test_skips_broken_clip_continues_run
# ---------------------------------------------------------------------------


def test_skips_broken_clip_continues_run(tmp_path: Path) -> None:
    """One ffprobe failure must not abort the run — other clips still emit."""
    clips = ["good1.mp4", "broken.mp4", "good2.mp4"]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clips)
    runner = _make_runner(
        behaviour={"broken.mp4": (1, "", "moov atom not found")},
    )
    output = tmp_path / "out.jsonl"

    written, skipped, dedups = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner,
    )

    assert (written, skipped, dedups) == (2, 1, 0)
    rows = _read_jsonl(output)
    src_set = {r["src"] for r in rows}
    assert src_set == {"good1.mp4", "good2.mp4"}


# ---------------------------------------------------------------------------
# 3. test_refuses_konvid_150k_csv
# ---------------------------------------------------------------------------


def test_refuses_konvid_150k_csv(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """A CSV with > 1500 rows must abort with a hint pointing at Phase 2."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["only_one.mp4"])
    csv_path = konvid_dir / "KoNViD_1k_metadata" / "KoNViD_1k_attributes.csv"
    rows = [
        {"file_name": f"v{i}.mp4", "MOS": 3.0, "SD": 0.5, "n": 50, "flickr_id": str(i)}
        for i in range(1600)
    ]
    _make_csv(csv_path, rows)

    rc = KONVID.main(
        [
            "--konvid-dir",
            str(konvid_dir),
            "--output",
            str(tmp_path / "should_not_be_written.jsonl"),
        ]
    )
    assert rc != 0
    captured = capsys.readouterr()
    blob = captured.out + captured.err
    assert "150k" in blob.lower() or "konvid_150k" in blob.lower()


# ---------------------------------------------------------------------------
# 4. test_mos_columns_present
# ---------------------------------------------------------------------------


def test_mos_columns_present(tmp_path: Path) -> None:
    """Round-trip a synthetic CSV and assert all three MOS columns survive."""
    konvid_dir = _scaffold_corpus(
        tmp_path,
        clip_names=["m1.mp4", "m2.mp4"],
        csv_rows=[
            {"file_name": "m1.mp4", "MOS": 4.21, "SD": 0.37, "n": 64, "flickr_id": "111"},
            {"file_name": "m2.mp4", "MOS": 2.18, "SD": 0.91, "n": 51, "flickr_id": "222"},
        ],
    )
    output = tmp_path / "out.jsonl"
    KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
    )
    rows = _read_jsonl(output)
    by_src = {r["src"]: r for r in rows}
    assert by_src["m1.mp4"]["mos"] == pytest.approx(4.21)
    assert by_src["m1.mp4"]["mos_std_dev"] == pytest.approx(0.37)
    assert by_src["m1.mp4"]["n_ratings"] == 64
    assert by_src["m2.mp4"]["mos"] == pytest.approx(2.18)
    assert by_src["m2.mp4"]["mos_std_dev"] == pytest.approx(0.91)
    assert by_src["m2.mp4"]["n_ratings"] == 51


def test_mos_columns_present_with_alias_headers(tmp_path: Path) -> None:
    """Alias spellings (mos / mos_std / num_ratings / video_name) also work."""
    konvid_dir = tmp_path / "konvid-1k"
    videos_dir = konvid_dir / "KoNViD_1k_videos"
    metadata_dir = konvid_dir / "KoNViD_1k_metadata"
    videos_dir.mkdir(parents=True)
    metadata_dir.mkdir(parents=True)
    _make_clip(videos_dir / "alias.mp4")
    _make_csv(
        metadata_dir / "KoNViD_1k_attributes.csv",
        [{"file_name": "alias.mp4", "MOS": 3.7, "SD": 0.2, "n": 42, "flickr_id": "999"}],
        header_aliases={
            "file_name": "video_name",
            "MOS": "mos",
            "SD": "mos_std",
            "n": "num_ratings",
        },
    )
    output = tmp_path / "out.jsonl"
    written, _skipped, _dedups = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
    )
    assert written == 1
    row = _read_jsonl(output)[0]
    assert row["mos"] == pytest.approx(3.7)
    assert row["mos_std_dev"] == pytest.approx(0.2)
    assert row["n_ratings"] == 42


# ---------------------------------------------------------------------------
# 5. test_existing_jsonl_resumed
# ---------------------------------------------------------------------------


def test_existing_jsonl_resumed(tmp_path: Path) -> None:
    """Re-running with the same corpus and the same output dedups by src_sha256."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["a.mp4", "b.mp4"])
    output = tmp_path / "out.jsonl"

    # First run: both clips land.
    w1, s1, d1 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
    )
    assert (w1, s1, d1) == (2, 0, 0)
    first_pass = _read_jsonl(output)
    assert len(first_pass) == 2

    # Second run: nothing new, both should dedup.
    w2, _s2, d2 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
    )
    assert w2 == 0
    assert d2 == 2
    second_pass = _read_jsonl(output)
    # Length unchanged — original rows still present (not blown away).
    assert len(second_pass) == 2
    assert {r["src_sha256"] for r in second_pass} == {r["src_sha256"] for r in first_pass}

    # Third run with a *new* clip added: appends one row.
    _make_clip(konvid_dir / "KoNViD_1k_videos" / "c.mp4")
    csv_path = konvid_dir / "KoNViD_1k_metadata" / "KoNViD_1k_attributes.csv"
    with csv_path.open("a", encoding="utf-8") as fh:
        fh.write("c.mp4,4.0,0.3,55,777\n")
    w3, _s3, d3 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
    )
    assert (w3, d3) == (1, 2)
    assert {r["src"] for r in _read_jsonl(output)} == {"a.mp4", "b.mp4", "c.mp4"}


# ---------------------------------------------------------------------------
# 6. test_corpus_metadata_columns_constant
# ---------------------------------------------------------------------------


def test_corpus_metadata_columns_constant(tmp_path: Path) -> None:
    """Pin ``corpus`` = 'konvid-1k' and assert ``corpus_version`` is non-empty."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["c.mp4"])
    output = tmp_path / "out.jsonl"
    KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_make_runner(),
        corpus_version="konvid-1k-2017",
    )
    row = _read_jsonl(output)[0]
    assert row["corpus"] == "konvid-1k"
    assert isinstance(row["corpus_version"], str)
    assert row["corpus_version"]  # non-empty
    # Schema sanity: all required fields present.
    expected_keys = {
        "src",
        "src_sha256",
        "src_size_bytes",
        "width",
        "height",
        "framerate",
        "duration_s",
        "pix_fmt",
        "encoder_upstream",
        "mos",
        "mos_std_dev",
        "n_ratings",
        "corpus",
        "corpus_version",
        "ingested_at_utc",
    }
    assert set(row) == expected_keys
    # Schema isolation: no collision with vmaf-tune Phase A row contract.
    assert "vmaf_score" not in row


# ---------------------------------------------------------------------------
# Bonus: missing-corpus-dir error path
# ---------------------------------------------------------------------------


def test_missing_konvid_dir_returns_clear_error(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    """Pointing the script at a non-existent dir surfaces a download hint."""
    rc = KONVID.main(
        [
            "--konvid-dir",
            str(tmp_path / "nope"),
            "--output",
            str(tmp_path / "out.jsonl"),
        ]
    )
    assert rc != 0
    err = capsys.readouterr().err
    assert "konvid-1k-database.html" in err

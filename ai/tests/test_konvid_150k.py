# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.konvid_150k_to_corpus_jsonl` (ADR-0325 Phase 2).

These tests exercise the adapter's pure-Python contract end-to-end on
synthetic fixtures. They do **not** require ffprobe or curl on the host,
nor do they need the KonViD-150k corpus on disk — every external call is
routed through the script's injectable ``runner`` seam.

Phase-2-specific paths covered:

* Resumable downloads — partial progress JSON + restart picks up where
  the prior run left off.
* Attrition tolerance — mocked 8% download failure rate completes the
  run with a warning summary, not an exception.
* Refuse-1k cutoff — passing a 1.2k-row CSV exits non-zero with a hint
  pointing at the Phase 1 script.

Plus the standard cross-phase ones (geometry parse, MOS columns,
corpus / corpus_version constants, license-text wording, append+dedup
on re-run).
"""

from __future__ import annotations

import importlib.util
import json
import logging
import subprocess
from pathlib import Path
from typing import Any

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "konvid_150k_to_corpus_jsonl.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("konvid_150k_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


KONVID = _load_module()


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _make_clip(path: Path, *, content: bytes | None = None) -> None:
    """Create a placeholder MP4 file (content is irrelevant under mocked ffprobe)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if content is None:
        content = f"konvid-150k-fixture:{path.name}".encode("utf-8")
    path.write_bytes(content)


def _make_manifest_csv(
    path: Path,
    rows: list[dict[str, Any]],
    *,
    header_aliases: dict[str, str] | None = None,
) -> None:
    """Write a synthetic KonViD-150k-style manifest CSV.

    Default headers: ``file_name,url,MOS,SD,n,flickr_id``. Pass
    ``header_aliases`` to remap a standard key to an alternative
    spelling (e.g. ``{"MOS": "mos", "SD": "mos_std"}``).
    """
    aliases = header_aliases or {}
    standard = ["file_name", "url", "MOS", "SD", "n", "flickr_id"]
    headers = [aliases.get(k, k) for k in standard]

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        fh.write(",".join(headers) + "\n")
        for row in rows:
            fh.write(
                "{file_name},{url},{MOS},{SD},{n},{flickr_id}\n".format(
                    file_name=row["file_name"],
                    url=row.get("url", ""),
                    MOS=row["MOS"],
                    SD=row.get("SD", ""),
                    n=row.get("n", ""),
                    flickr_id=row.get("flickr_id", ""),
                )
            )


def _make_split_score_csv(path: Path, rows: list[tuple[str, float]], *, header: str) -> None:
    """Write a synthetic K150K-A/B score CSV."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        fh.write(header + "\n")
        for name, score in rows:
            if header == "video_name,video_score":
                fh.write(f"{name},{score}\n")
            else:
                fh.write(f"{name},{score},{score}\n")


def _ffprobe_ok_payload(
    *,
    width: int = 960,
    height: int = 540,
    codec: str = "h264",
    pix_fmt: str = "yuv420p",
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


class _FakeRunner:
    """Composable subprocess.run stand-in for ffprobe + curl.

    Routes by argv[0]: ``ffprobe`` returns the canned ffprobe JSON,
    ``curl`` writes a placeholder file at ``--output`` to simulate a
    successful download. Behaviour can be overridden per-target-name
    (the basename of the output file or the clip path).
    """

    def __init__(
        self,
        *,
        download_failures: set[str] | None = None,
        ffprobe_failures: set[str] | None = None,
        ffprobe_payload: str | None = None,
    ) -> None:
        self.download_failures = download_failures or set()
        self.ffprobe_failures = ffprobe_failures or set()
        self.ffprobe_payload = ffprobe_payload or _ffprobe_ok_payload()
        self.download_attempts: list[str] = []
        self.ffprobe_attempts: list[str] = []

    def __call__(self, cmd, **_kw):
        argv0 = Path(cmd[0]).name
        if argv0.endswith("ffprobe"):
            target = Path(cmd[-1]).name
            self.ffprobe_attempts.append(target)
            if target in self.ffprobe_failures:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=1, stdout="", stderr="moov atom not found"
                )
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=self.ffprobe_payload, stderr=""
            )
        if argv0.endswith("curl"):
            # ``--output <path>`` is the dest; URL is the last positional.
            output_path: str | None = None
            for i, tok in enumerate(cmd):
                if tok == "--output" and i + 1 < len(cmd):
                    output_path = cmd[i + 1]
                    break
            assert output_path is not None
            target_name = Path(output_path).name.removesuffix(".part")
            self.download_attempts.append(target_name)
            if target_name in self.download_failures:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=22, stdout="", stderr="HTTP 404 Not Found"
                )
            # Write a deterministic non-empty payload to the .part file.
            Path(output_path).parent.mkdir(parents=True, exist_ok=True)
            Path(output_path).write_bytes(f"downloaded:{target_name}".encode("utf-8"))
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")
        raise AssertionError(f"unexpected runner invocation: {cmd!r}")


def _scaffold_corpus(
    tmp_path: Path,
    *,
    clip_names: list[str],
    csv_rows: list[dict[str, Any]] | None = None,
    pre_existing_clips: list[str] | None = None,
    n_csv_rows: int | None = None,
) -> Path:
    """Build a minimal ``.workingdir2/konvid-150k/``-shaped tree under tmp_path.

    By default no clips are pre-staged on disk — the run must download
    them via the mocked curl. Pass ``pre_existing_clips`` to drop a
    subset on disk pre-run.
    """
    konvid_dir = tmp_path / "konvid-150k"
    konvid_dir.mkdir(parents=True)
    clips_dir = konvid_dir / "clips"
    clips_dir.mkdir()

    if csv_rows is None:
        if n_csv_rows is None:
            n_csv_rows = len(clip_names)
        csv_rows = [
            {
                "file_name": clip_names[i] if i < len(clip_names) else f"v{i}.mp4",
                "url": (
                    f"https://example.invalid/{clip_names[i]}"
                    if i < len(clip_names)
                    else f"https://example.invalid/v{i}.mp4"
                ),
                "MOS": 3.5 + 0.001 * i,
                "SD": 0.42,
                "n": 50,
                "flickr_id": str(i),
            }
            for i in range(n_csv_rows)
        ]
    _make_manifest_csv(konvid_dir / "manifest.csv", csv_rows)

    for name in pre_existing_clips or []:
        _make_clip(clips_dir / name)
    return konvid_dir


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


# ---------------------------------------------------------------------------
# 1. Resumable download — partial progress + restart picks up where left off
# ---------------------------------------------------------------------------


def test_resumable_download_picks_up_where_interrupted(tmp_path: Path) -> None:
    """Write a partial progress file, re-run, assert only missing clips retried."""
    clips = ["a.mp4", "b.mp4", "c.mp4", "d.mp4"]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clips)
    progress_path = konvid_dir / ".download-progress.json"

    # Pretend a previous run already completed `a.mp4` and `b.mp4`
    # (clips on disk + state == done).
    _make_clip(konvid_dir / "clips" / "a.mp4")
    _make_clip(konvid_dir / "clips" / "b.mp4")
    progress_path.write_text(
        json.dumps(
            {
                "a.mp4": {"state": "done"},
                "b.mp4": {"state": "done"},
            }
        ),
        encoding="utf-8",
    )

    runner = _FakeRunner()
    output = tmp_path / "out.jsonl"
    stats = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
    )

    # Only c.mp4 and d.mp4 should have been downloaded.
    assert sorted(runner.download_attempts) == ["c.mp4", "d.mp4"]
    assert stats.written == 4
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0

    # Persisted state should now mark all four as done.
    state_after = json.loads(progress_path.read_text(encoding="utf-8"))
    assert {k: v["state"] for k, v in state_after.items()} == {
        "a.mp4": "done",
        "b.mp4": "done",
        "c.mp4": "done",
        "d.mp4": "done",
    }


def test_resumable_download_persists_failures(tmp_path: Path) -> None:
    """A failed download writes to progress; re-run honours the non-retry contract."""
    clips = ["ok.mp4", "gone.mp4"]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clips)
    progress_path = konvid_dir / ".download-progress.json"

    runner1 = _FakeRunner(download_failures={"gone.mp4"})
    output = tmp_path / "out.jsonl"
    stats1 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner1,
        min_csv_rows=2,
    )
    assert stats1.written == 1
    assert stats1.skipped_download == 1
    state_after = json.loads(progress_path.read_text(encoding="utf-8"))
    assert state_after["gone.mp4"]["state"] == "failed"
    assert "404" in state_after["gone.mp4"]["reason"]

    # Re-run: the failed clip must NOT be re-attempted.
    runner2 = _FakeRunner()  # would succeed if attempted
    stats2 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner2,
        min_csv_rows=2,
    )
    assert "gone.mp4" not in runner2.download_attempts
    # Already-written rows dedup'd on second pass.
    assert stats2.written == 0
    assert stats2.skipped_download == 1


# ---------------------------------------------------------------------------
# 2. Attrition tolerance — 8% download failures produces a warning, not crash
# ---------------------------------------------------------------------------


def test_attrition_threshold_warning(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    """8% mocked download failures: run completes, WARNING summary fires."""
    n = 100
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    # Fail 8 out of 100 (8% — within typical 5-8% but above the
    # default 10% advisory threshold once we drop the warn cap).
    failed = {clip_names[i] for i in range(0, 80, 10)}  # 8 evenly-spaced
    assert len(failed) == 8
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        stats = KONVID.run(
            konvid_dir=konvid_dir,
            output=output,
            runner=runner,
            # Drop the threshold below the actual rate to force the warning.
            attrition_warn_threshold=0.05,
            min_csv_rows=2,
        )

    assert stats.written == n - len(failed)
    assert stats.skipped_download == len(failed)
    assert stats.skipped_broken == 0
    assert stats.attrition_pct == pytest.approx(0.08)

    # Summary line on stderr (subprocess.run doesn't capture print, but
    # logger fires too).
    log_text = caplog.text
    assert "attrition" in log_text.lower()
    assert "exceeds" in log_text.lower()


def test_attrition_below_threshold_no_warning(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """3% download failures stay below the 10% default; no attrition warning."""
    n = 100
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 30, 10)}  # 3 failures
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        KONVID.run(
            konvid_dir=konvid_dir,
            output=output,
            runner=runner,
            # Default is 0.10; 0.03 stays under.
            min_csv_rows=2,
        )

    # No "exceeds" attrition WARNING expected.
    assert "exceeds advisory threshold" not in caplog.text


# ---------------------------------------------------------------------------
# 3. Refuse-1k cutoff — < 5000 rows aborts with Phase-1 hint
# ---------------------------------------------------------------------------


def test_refuses_konvid_1k_csv(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """A CSV with < 5000 rows must abort with a hint pointing at Phase 1."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["only_one.mp4"], n_csv_rows=1200)

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
    assert "1k" in blob.lower() or "konvid_1k" in blob.lower() or "phase 1" in blob.lower()


# ---------------------------------------------------------------------------
# 4. Geometry parse from ffprobe JSON
# ---------------------------------------------------------------------------


def test_geometry_parse_from_ffprobe_json(tmp_path: Path) -> None:
    """One mocked ffprobe (vp9, 540p) → row's geometry fields match."""
    konvid_dir = _scaffold_corpus(
        tmp_path,
        clip_names=["clipA.mp4"],
        pre_existing_clips=["clipA.mp4"],
    )
    runner = _FakeRunner(
        ffprobe_payload=_ffprobe_ok_payload(width=960, height=540, codec="vp9", pix_fmt="yuv420p"),
    )
    output = tmp_path / "out.jsonl"
    stats = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
    )

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
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0


# ---------------------------------------------------------------------------
# 5. MOS columns survive round-trip (canonical + alias spellings)
# ---------------------------------------------------------------------------


def test_mos_columns_present(tmp_path: Path) -> None:
    """Round-trip a synthetic CSV and assert all three MOS columns survive."""
    konvid_dir = _scaffold_corpus(
        tmp_path,
        clip_names=["m1.mp4", "m2.mp4"],
        csv_rows=[
            {
                "file_name": "m1.mp4",
                "url": "https://example.invalid/m1.mp4",
                "MOS": 4.21,
                "SD": 0.37,
                "n": 64,
                "flickr_id": "111",
            },
            {
                "file_name": "m2.mp4",
                "url": "https://example.invalid/m2.mp4",
                "MOS": 2.18,
                "SD": 0.91,
                "n": 51,
                "flickr_id": "222",
            },
        ],
    )
    output = tmp_path / "out.jsonl"
    KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
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
    konvid_dir = tmp_path / "konvid-150k"
    konvid_dir.mkdir()
    (konvid_dir / "clips").mkdir()
    csv_rows = [
        {
            "file_name": "alias.mp4",
            "url": "https://example.invalid/alias.mp4",
            "MOS": 3.7,
            "SD": 0.2,
            "n": 42,
            "flickr_id": "999",
        }
    ]
    _make_manifest_csv(
        konvid_dir / "manifest.csv",
        csv_rows,
        header_aliases={
            "file_name": "video_name",
            "url": "download_url",
            "MOS": "mos",
            "SD": "mos_std",
            "n": "num_ratings",
        },
    )
    output = tmp_path / "out.jsonl"
    stats = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=1,
    )
    assert stats.written == 1
    row = _read_jsonl(output)[0]
    assert row["mos"] == pytest.approx(3.7)
    assert row["mos_std_dev"] == pytest.approx(0.2)
    assert row["n_ratings"] == 42


def test_split_score_layout_is_auto_discovered(tmp_path: Path) -> None:
    """Default path accepts canonical k150ka/k150kb score CSV + extracted dirs."""
    konvid_dir = tmp_path / "konvid-150k"
    clips_a = konvid_dir / "k150ka_extracted"
    clips_b = konvid_dir / "k150kb_extracted"
    rows_a = [("a1.mp4", 3.4), ("a2.mp4", 2.8)]
    rows_b = [("b1.mp4", 3.61)]
    _make_split_score_csv(konvid_dir / "k150ka_scores.csv", rows_a, header="video_name,video_score")
    _make_split_score_csv(
        konvid_dir / "k150kb_scores.csv", rows_b, header="video_name,mos,video_score"
    )
    for name, _score in rows_a:
        _make_clip(clips_a / name)
    for name, _score in rows_b:
        _make_clip(clips_b / name)

    output = tmp_path / "out.jsonl"
    stats = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=3,
    )

    assert stats.written == 3
    rows = _read_jsonl(output)
    by_src = {r["src"]: r for r in rows}
    assert by_src["a1.mp4"]["mos"] == pytest.approx(3.4)
    assert by_src["a2.mp4"]["mos"] == pytest.approx(2.8)
    assert by_src["b1.mp4"]["mos"] == pytest.approx(3.61)
    assert all(r["mos_std_dev"] == 0.0 for r in rows)
    assert all(r["n_ratings"] == 0 for r in rows)


def test_explicit_missing_manifest_does_not_fallback_to_split_scores(tmp_path: Path) -> None:
    """An explicit --manifest-csv remains strict and does not mask path mistakes."""
    konvid_dir = tmp_path / "konvid-150k"
    _make_split_score_csv(
        konvid_dir / "k150ka_scores.csv", [("a1.mp4", 3.4)], header="video_name,video_score"
    )
    with pytest.raises(FileNotFoundError, match=r"missing\.csv"):
        KONVID.run(
            konvid_dir=konvid_dir,
            output=tmp_path / "out.jsonl",
            manifest_csv=konvid_dir / "missing.csv",
            runner=_FakeRunner(),
            min_csv_rows=1,
        )


# ---------------------------------------------------------------------------
# 6. Broken clips are skipped; run still completes
# ---------------------------------------------------------------------------


def test_skips_broken_clip_continues_run(tmp_path: Path) -> None:
    """One ffprobe failure must not abort the run — other clips still emit."""
    clip_names = ["good1.mp4", "broken.mp4", "good2.mp4"]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    runner = _FakeRunner(ffprobe_failures={"broken.mp4"})

    output = tmp_path / "out.jsonl"
    stats = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
    )

    assert stats.written == 2
    assert stats.skipped_broken == 1
    assert stats.skipped_download == 0
    rows = _read_jsonl(output)
    assert {r["src"] for r in rows} == {"good1.mp4", "good2.mp4"}


# ---------------------------------------------------------------------------
# 7. Append + dedup on re-run (idempotency contract)
# ---------------------------------------------------------------------------


def test_existing_jsonl_resumed(tmp_path: Path) -> None:
    """Re-running with the same corpus and the same output dedups by src_sha256."""
    clip_names = ["a.mp4", "b.mp4"]
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    output = tmp_path / "out.jsonl"

    # First run.
    s1 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
    )
    assert s1.written == 2
    first_pass = _read_jsonl(output)
    assert len(first_pass) == 2

    # Second run.
    s2 = KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
    )
    assert s2.written == 0
    assert s2.dedups == 2
    second_pass = _read_jsonl(output)
    assert len(second_pass) == 2


# ---------------------------------------------------------------------------
# 8. corpus / corpus_version metadata constants + license-text wording
# ---------------------------------------------------------------------------


def test_corpus_metadata_columns_constant(tmp_path: Path) -> None:
    """Pin ``corpus`` = 'konvid-150k' and ``corpus_version`` non-empty."""
    konvid_dir = _scaffold_corpus(tmp_path, clip_names=["c.mp4"])
    output = tmp_path / "out.jsonl"
    KONVID.run(
        konvid_dir=konvid_dir,
        output=output,
        runner=_FakeRunner(),
        corpus_version="konvid-150k-2019",
        min_csv_rows=1,
    )
    row = _read_jsonl(output)[0]
    assert row["corpus"] == "konvid-150k"
    assert isinstance(row["corpus_version"], str)
    assert row["corpus_version"]
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


def test_license_text_wording_in_module_docstring() -> None:
    """The script's docstring must explicitly state the no-redistribute posture."""
    doc = KONVID.__doc__ or ""
    assert "research-only" in doc.lower()
    # Must say we don't ship clips, MOS, or features in tree.
    assert "does **not** ship" in doc or "does not ship" in doc
    # Must reference ADR-0325.
    assert "ADR-0325" in doc


# ---------------------------------------------------------------------------
# 9. Missing-corpus-dir error path
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
    assert "konvid-150k-vqa-database.html" in err


# ---------------------------------------------------------------------------
# 10. Download progress file is atomically rewritten (no partial-write loss)
# ---------------------------------------------------------------------------


def test_progress_file_is_atomic(tmp_path: Path) -> None:
    """save_progress must rename atomically — no half-truncated JSON survives."""
    progress_path = tmp_path / ".download-progress.json"
    KONVID.save_progress(progress_path, {"a.mp4": {"state": "done"}})
    payload = json.loads(progress_path.read_text(encoding="utf-8"))
    assert payload == {"a.mp4": {"state": "done"}}

    # Overwrite — must not leave a temp file behind.
    KONVID.save_progress(progress_path, {"b.mp4": {"state": "failed", "reason": "x"}})
    payload2 = json.loads(progress_path.read_text(encoding="utf-8"))
    assert "b.mp4" in payload2
    assert "a.mp4" not in payload2
    leftovers = list(tmp_path.glob(".download-progress.*.tmp"))
    assert leftovers == []

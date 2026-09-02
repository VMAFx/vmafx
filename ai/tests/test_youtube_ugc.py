# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.youtube_ugc_to_corpus_jsonl` (ADR-0368).

These tests exercise the YouTube UGC adapter's pure-Python
contract end-to-end on synthetic fixtures. They do **not**
require ffprobe or curl on the host, nor do they need the ~2 TB
YouTube UGC corpus on disk — every external call is routed
through the script's injectable ``runner`` seam.

YouTube-UGC-specific paths covered:

* Resumable downloads — partial progress JSON + restart picks up
  where the prior run left off.
* Attrition tolerance — mocked failure rate above
  ``--attrition-warn-threshold`` produces a WARNING summary, not
  an exception.
* Refuse-tiny cutoff — passing a < 200-row CSV exits non-zero
  with a hint.
* ``--max-rows`` cap — laptop-class default ingests only the
  first N rows; ``--full`` opts into whole-corpus ingestion.
* Bare-stem ``vid`` column → ``.mp4`` suffix is appended.
* Synthesised bucket URL when manifest CSV lacks a ``url`` column.

Plus the standard cross-corpus ones (geometry parse, MOS columns,
corpus / corpus_version constants, license-text wording, append+
dedup on re-run).
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
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "youtube_ugc_to_corpus_jsonl.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("youtube_ugc_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


UGC = _load_module()


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _make_clip(path: Path, *, content: bytes | None = None) -> None:
    """Create a placeholder MP4 file (content is irrelevant under mocked ffprobe)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if content is None:
        content = f"ugc-fixture:{path.name}".encode("utf-8")
    path.write_bytes(content)


def _make_manifest_csv(
    path: Path,
    rows: list[dict[str, Any]],
    *,
    header_aliases: dict[str, str] | None = None,
    omit_url_column: bool = False,
) -> None:
    """Write a synthetic YouTube-UGC-style manifest CSV.

    Default headers: ``vid,url,mos,sd,n``. Pass
    ``header_aliases`` to remap a standard key to an alternative
    spelling (e.g. ``{"mos": "MOS", "sd": "mos_std"}``). Pass
    ``omit_url_column=True`` to write a CSV without the ``url``
    column entirely (exercises the synth-bucket-URL path).
    """
    aliases = header_aliases or {}
    standard = ["vid", "url", "mos", "sd", "n"]
    if omit_url_column:
        standard = ["vid", "mos", "sd", "n"]
    headers = [aliases.get(k, k) for k in standard]

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        fh.write(",".join(headers) + "\n")
        for row in rows:
            if omit_url_column:
                fh.write(
                    "{vid},{mos},{sd},{n}\n".format(
                        vid=row["vid"],
                        mos=row["mos"],
                        sd=row.get("sd", ""),
                        n=row.get("n", ""),
                    )
                )
            else:
                fh.write(
                    "{vid},{url},{mos},{sd},{n}\n".format(
                        vid=row["vid"],
                        url=row.get("url", ""),
                        mos=row["mos"],
                        sd=row.get("sd", ""),
                        n=row.get("n", ""),
                    )
                )


def _ffprobe_ok_payload(
    *,
    width: int = 1920,
    height: int = 1080,
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
                    "duration": "20.000",
                    "pix_fmt": pix_fmt,
                    "codec_name": codec,
                }
            ],
            "format": {"duration": "20.000"},
        }
    )


class _FakeRunner:
    """Composable subprocess.run stand-in for ffprobe + curl.

    Routes by argv[0]: ``ffprobe`` returns the canned ffprobe
    JSON; ``curl`` writes a placeholder file at ``--output`` to
    simulate a successful download. Behaviour can be overridden
    per-target-name (the basename of the output file or the clip
    path).
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
        self.download_urls: list[str] = []
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
            output_path: str | None = None
            for i, tok in enumerate(cmd):
                if tok == "--output" and i + 1 < len(cmd):
                    output_path = cmd[i + 1]
                    break
            assert output_path is not None
            # Capture URL too — last positional arg in the curl invocation.
            self.download_urls.append(cmd[-1])
            target_name = Path(output_path).name.removesuffix(".part")
            self.download_attempts.append(target_name)
            if target_name in self.download_failures:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=22, stdout="", stderr="HTTP 404 Not Found"
                )
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
    omit_url_column: bool = False,
) -> Path:
    """Build a minimal ``.workingdir2/youtube-ugc/``-shaped tree under tmp_path.

    By default no clips are pre-staged on disk — the run must
    download them via the mocked curl. Pass ``pre_existing_clips``
    to drop a subset on disk pre-run.
    """
    ugc_dir = tmp_path / "youtube-ugc"
    ugc_dir.mkdir(parents=True)
    clips_dir = ugc_dir / "clips"
    clips_dir.mkdir()

    if csv_rows is None:
        if n_csv_rows is None:
            n_csv_rows = len(clip_names)
        csv_rows = [
            {
                "vid": clip_names[i] if i < len(clip_names) else f"v{i}.mp4",
                "url": (
                    f"https://example.invalid/{clip_names[i]}"
                    if i < len(clip_names)
                    else f"https://example.invalid/v{i}.mp4"
                ),
                "mos": 3.5 + 0.001 * i,
                "sd": 0.42,
                "n": 50,
            }
            for i in range(n_csv_rows)
        ]
    _make_manifest_csv(ugc_dir / "manifest.csv", csv_rows, omit_url_column=omit_url_column)

    for name in pre_existing_clips or []:
        _make_clip(clips_dir / name)
    return ugc_dir


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
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clips)
    progress_path = ugc_dir / ".download-progress.json"

    _make_clip(ugc_dir / "clips" / "a.mp4")
    _make_clip(ugc_dir / "clips" / "b.mp4")
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
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
        max_rows=None,
    )

    assert sorted(runner.download_attempts) == ["c.mp4", "d.mp4"]
    assert stats.written == 4
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0

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
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clips)
    progress_path = ugc_dir / ".download-progress.json"

    runner1 = _FakeRunner(download_failures={"gone.mp4"})
    output = tmp_path / "out.jsonl"
    stats1 = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner1,
        min_csv_rows=2,
        max_rows=None,
    )
    assert stats1.written == 1
    assert stats1.skipped_download == 1
    state_after = json.loads(progress_path.read_text(encoding="utf-8"))
    assert state_after["gone.mp4"]["state"] == "failed"
    assert "404" in state_after["gone.mp4"]["reason"]

    runner2 = _FakeRunner()  # would succeed if attempted
    stats2 = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner2,
        min_csv_rows=2,
        max_rows=None,
    )
    assert "gone.mp4" not in runner2.download_attempts
    assert stats2.written == 0
    assert stats2.skipped_download == 1


# ---------------------------------------------------------------------------
# 2. Attrition tolerance — failures produce a warning, not crash
# ---------------------------------------------------------------------------


def test_attrition_threshold_warning(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    """8% mocked download failures: run completes, WARNING summary fires."""
    n = 100
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 80, 10)}  # 8 evenly-spaced
    assert len(failed) == 8
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        stats = UGC.run(
            ugc_dir=ugc_dir,
            output=output,
            runner=runner,
            attrition_warn_threshold=0.05,
            min_csv_rows=2,
            max_rows=None,
        )

    assert stats.written == n - len(failed)
    assert stats.skipped_download == len(failed)
    assert stats.skipped_broken == 0
    assert stats.attrition_pct == pytest.approx(0.08)

    log_text = caplog.text
    assert "attrition" in log_text.lower()
    assert "exceeds" in log_text.lower()


def test_attrition_below_threshold_no_warning(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """3% download failures stay below the 10% default; no attrition warning."""
    n = 100
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 30, 10)}  # 3 failures
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        UGC.run(
            ugc_dir=ugc_dir,
            output=output,
            runner=runner,
            min_csv_rows=2,
            max_rows=None,
        )

    assert "exceeds advisory threshold" not in caplog.text


# ---------------------------------------------------------------------------
# 3. Refuse-tiny cutoff — < 200 rows aborts
# ---------------------------------------------------------------------------


def test_refuses_tiny_csv(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """A CSV with < 200 rows must abort with a clear error."""
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=["only_one.mp4"], n_csv_rows=50)

    rc = UGC.main(
        [
            "--ugc-dir",
            str(ugc_dir),
            "--output",
            str(tmp_path / "should_not_be_written.jsonl"),
        ]
    )
    assert rc != 0
    captured = capsys.readouterr()
    blob = captured.out + captured.err
    assert "ugc" in blob.lower() or "sanity floor" in blob.lower()


# ---------------------------------------------------------------------------
# 4. Geometry parse from ffprobe JSON
# ---------------------------------------------------------------------------


def test_geometry_parse_from_ffprobe_json(tmp_path: Path) -> None:
    """One mocked ffprobe (vp9, 1080p) → row's geometry fields match."""
    ugc_dir = _scaffold_corpus(
        tmp_path,
        clip_names=["clipA.mp4"],
        pre_existing_clips=["clipA.mp4"],
    )
    runner = _FakeRunner(
        ffprobe_payload=_ffprobe_ok_payload(
            width=1920, height=1080, codec="vp9", pix_fmt="yuv420p"
        ),
    )
    output = tmp_path / "out.jsonl"
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )

    rows = _read_jsonl(output)
    assert len(rows) == 1
    row = rows[0]
    assert row["src"] == "clipA.mp4"
    assert row["width"] == 1920
    assert row["height"] == 1080
    assert row["framerate"] == pytest.approx(30.0)
    assert row["pix_fmt"] == "yuv420p"
    assert row["encoder_upstream"] == "vp9"
    assert row["duration_s"] == pytest.approx(20.0)
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0


# ---------------------------------------------------------------------------
# 5. MOS columns survive round-trip (canonical + alias spellings)
# ---------------------------------------------------------------------------


def test_mos_columns_present(tmp_path: Path) -> None:
    """Round-trip a synthetic CSV and assert all three MOS columns survive."""
    ugc_dir = _scaffold_corpus(
        tmp_path,
        clip_names=["m1.mp4", "m2.mp4"],
        csv_rows=[
            {
                "vid": "m1.mp4",
                "url": "https://example.invalid/m1.mp4",
                "mos": 4.21,
                "sd": 0.37,
                "n": 64,
            },
            {
                "vid": "m2.mp4",
                "url": "https://example.invalid/m2.mp4",
                "mos": 2.18,
                "sd": 0.91,
                "n": 51,
            },
        ],
    )
    output = tmp_path / "out.jsonl"
    UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
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
    """Alias spellings (DMOS / mos_std / num_ratings / video_name) also work."""
    ugc_dir = tmp_path / "youtube-ugc"
    ugc_dir.mkdir()
    (ugc_dir / "clips").mkdir()
    csv_rows = [
        {
            "vid": "alias.mp4",
            "url": "https://example.invalid/alias.mp4",
            "mos": 3.7,
            "sd": 0.2,
            "n": 42,
        }
    ]
    _make_manifest_csv(
        ugc_dir / "manifest.csv",
        csv_rows,
        header_aliases={
            "vid": "video_name",
            "url": "download_url",
            "mos": "DMOS",
            "sd": "mos_std",
            "n": "num_ratings",
        },
    )
    output = tmp_path / "out.jsonl"
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=1,
        max_rows=None,
    )
    assert stats.written == 1
    row = _read_jsonl(output)[0]
    assert row["mos"] == pytest.approx(3.7)
    assert row["mos_std_dev"] == pytest.approx(0.2)
    assert row["n_ratings"] == 42


# ---------------------------------------------------------------------------
# 6. Bare-stem name column → .mp4 suffix appended
# ---------------------------------------------------------------------------


def test_bare_stem_name_column_gets_mp4_suffix(tmp_path: Path) -> None:
    """YouTube UGC manifest CSVs commonly carry stems
    (``"Gaming_720P-25aa_orig"``) without an extension.

    The adapter must transparently append the default ``.mp4``
    suffix and look the clip up under
    ``clips/Gaming_720P-25aa_orig.mp4``.
    """
    ugc_dir = tmp_path / "youtube-ugc"
    ugc_dir.mkdir()
    (ugc_dir / "clips").mkdir()
    _make_manifest_csv(
        ugc_dir / "manifest.csv",
        [
            {
                "vid": "Gaming_720P-25aa_orig",  # bare stem
                "url": "https://example.invalid/Gaming_720P-25aa_orig",
                "mos": 3.0,
                "sd": 0.5,
                "n": 30,
            }
        ],
    )

    output = tmp_path / "out.jsonl"
    runner = _FakeRunner()
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )

    assert stats.written == 1
    rows = _read_jsonl(output)
    assert rows[0]["src"] == "Gaming_720P-25aa_orig.mp4"
    assert "Gaming_720P-25aa_orig.mp4" in runner.download_attempts


# ---------------------------------------------------------------------------
# 7. Broken clips are skipped; run still completes
# ---------------------------------------------------------------------------


def test_skips_broken_clip_continues_run(tmp_path: Path) -> None:
    """One ffprobe failure must not abort the run — other clips still emit."""
    clip_names = ["good1.mp4", "broken.mp4", "good2.mp4"]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    runner = _FakeRunner(ffprobe_failures={"broken.mp4"})

    output = tmp_path / "out.jsonl"
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
        max_rows=None,
    )

    assert stats.written == 2
    assert stats.skipped_broken == 1
    assert stats.skipped_download == 0
    rows = _read_jsonl(output)
    assert {r["src"] for r in rows} == {"good1.mp4", "good2.mp4"}


# ---------------------------------------------------------------------------
# 8. Append + dedup on re-run (idempotency contract)
# ---------------------------------------------------------------------------


def test_existing_jsonl_resumed(tmp_path: Path) -> None:
    """Re-running with the same corpus and the same output dedups by src_sha256."""
    clip_names = ["a.mp4", "b.mp4"]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)
    output = tmp_path / "out.jsonl"

    s1 = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s1.written == 2
    first_pass = _read_jsonl(output)
    assert len(first_pass) == 2

    s2 = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s2.written == 0
    assert s2.dedups == 2
    second_pass = _read_jsonl(output)
    assert len(second_pass) == 2


# ---------------------------------------------------------------------------
# 9. corpus / corpus_version metadata constants + license-text wording
# ---------------------------------------------------------------------------


def test_corpus_metadata_columns_constant(tmp_path: Path) -> None:
    """Pin ``corpus`` = 'youtube-ugc' and ``corpus_version`` non-empty."""
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=["c.mp4"])
    output = tmp_path / "out.jsonl"
    UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        corpus_version="ugc-2019-orig",
        min_csv_rows=1,
        max_rows=None,
    )
    row = _read_jsonl(output)[0]
    assert row["corpus"] == "youtube-ugc"
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
    assert "vmaf_score" not in row


def test_license_text_wording_in_module_docstring() -> None:
    """The script's docstring must explicitly state the no-redistribute posture."""
    doc = UGC.__doc__ or ""
    # CC-BY (Creative Commons Attribution) licence + redistribution posture must be called out.
    assert "Creative" in doc or "CC-BY" in doc.lower() or "cc-by" in doc.lower()
    # Allow the line-wrap variant where black/formatter splits "does\n**not** ship"
    # across a line break (the verbatim phrase is "does\n**not** ship any clip").
    doc_normalised = " ".join(doc.split())
    assert "does **not** ship" in doc_normalised or "does not ship" in doc_normalised
    # Must reference ADR-0368 and the canonical bucket.
    assert "ADR-0368" in doc
    assert "ugc-dataset" in doc


# ---------------------------------------------------------------------------
# 10. Missing-corpus-dir error path
# ---------------------------------------------------------------------------


def test_missing_ugc_dir_returns_clear_error(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """Pointing the script at a non-existent dir surfaces a download hint."""
    rc = UGC.main(
        [
            "--ugc-dir",
            str(tmp_path / "nope"),
            "--output",
            str(tmp_path / "out.jsonl"),
        ]
    )
    assert rc != 0
    err = capsys.readouterr().err
    assert "ugc-dataset" in err.lower() or "original_videos" in err.lower()


# ---------------------------------------------------------------------------
# 11. Download progress file is atomically rewritten (no partial-write loss)
# ---------------------------------------------------------------------------


def test_progress_file_is_atomic(tmp_path: Path) -> None:
    """save_progress must rename atomically — no half-truncated JSON survives."""
    progress_path = tmp_path / ".download-progress.json"
    UGC.save_progress(progress_path, {"a.mp4": {"state": "done"}})
    payload = json.loads(progress_path.read_text(encoding="utf-8"))
    assert payload == {"a.mp4": {"state": "done"}}

    UGC.save_progress(progress_path, {"b.mp4": {"state": "failed", "reason": "x"}})
    payload2 = json.loads(progress_path.read_text(encoding="utf-8"))
    assert "b.mp4" in payload2
    assert "a.mp4" not in payload2
    leftovers = list(tmp_path.glob(".download-progress.*.tmp"))
    assert leftovers == []


# ---------------------------------------------------------------------------
# 12. --max-rows cap (laptop-class default) + --full opts in to the whole CSV
# ---------------------------------------------------------------------------


def test_max_rows_caps_ingestion(tmp_path: Path) -> None:
    """max_rows=10 against a 250-row CSV ingests only the first 10."""
    n = 250
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)

    output = tmp_path / "out.jsonl"
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=10,
    )

    assert stats.written == 10
    rows = _read_jsonl(output)
    assert {r["src"] for r in rows} == {f"clip{i:03d}.mp4" for i in range(10)}


def test_full_flag_disables_max_rows_cap(tmp_path: Path) -> None:
    """``max_rows=None`` (the ``--full`` path) ingests the whole CSV."""
    n = 50
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    ugc_dir = _scaffold_corpus(tmp_path, clip_names=clip_names)

    # max_rows=20 cap path
    output_capped = tmp_path / "capped.jsonl"
    stats_capped = UGC.run(
        ugc_dir=ugc_dir,
        output=output_capped,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=20,
    )
    assert stats_capped.written == 20

    # --full path: max_rows=None, no cap.
    output_full = tmp_path / "full.jsonl"
    stats_full = UGC.run(
        ugc_dir=ugc_dir,
        output=output_full,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert stats_full.written == n


# ---------------------------------------------------------------------------
# 13. Synthesised bucket URL when manifest lacks a `url` column
# ---------------------------------------------------------------------------


def test_synth_bucket_url_when_url_column_missing(tmp_path: Path) -> None:
    """The canonical YouTube UGC original_videos.csv has no URL column;
    the adapter must synthesise URLs from the bucket prefix + filename.
    """
    n_csv_rows = 5
    clip_names = [f"clip{i:03d}.mp4" for i in range(n_csv_rows)]
    csv_rows = [
        {
            "vid": clip_names[i],
            "mos": 3.0 + 0.1 * i,
            "sd": 0.3,
            "n": 40,
        }
        for i in range(n_csv_rows)
    ]
    ugc_dir = _scaffold_corpus(
        tmp_path,
        clip_names=clip_names,
        csv_rows=csv_rows,
        omit_url_column=True,
    )

    runner = _FakeRunner()
    output = tmp_path / "out.jsonl"
    stats = UGC.run(
        ugc_dir=ugc_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )
    assert stats.written == n_csv_rows
    # All synthesised URLs must point at the canonical bucket.
    assert all(
        url.startswith("https://storage.googleapis.com/ugc-dataset/original_videos/")
        for url in runner.download_urls
    )
    # And every clip name must appear in exactly one URL.
    url_basenames = {url.rsplit("/", 1)[-1] for url in runner.download_urls}
    assert url_basenames == set(clip_names)

# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.live_vqc_to_corpus_jsonl` (ADR-0370).

These tests exercise the LIVE-VQC adapter's pure-Python contract
end-to-end on synthetic fixtures. They do **not** require ffprobe
or curl on the host, nor do they need the actual LIVE-VQC corpus on
disk — every external call is routed through the script's injectable
``runner`` seam.

LIVE-VQC-specific paths covered:

* Canonical two-column headerless shape auto-detection.
* Standard adapter CSV (LSVQ / KonViD-150k header style).
* Resumable downloads — partial progress JSON + restart picks up
  where the prior run left off.
* Attrition tolerance — mocked failure rate above
  ``--attrition-warn-threshold`` produces a WARNING summary.
* Refuse-tiny cutoff — passing a < 50-row CSV exits non-zero.
* ``--max-rows`` cap and ``--full`` opt-in to whole-corpus ingestion.
* Bare-stem ``name`` column → ``.mp4`` suffix is appended.

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
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "live_vqc_to_corpus_jsonl.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("live_vqc_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LIVE_VQC = _load_module()


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _make_clip(path: Path, *, content: bytes | None = None) -> None:
    """Create a placeholder MP4 file (content is irrelevant under mocked ffprobe)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if content is None:
        content = f"live-vqc-fixture:{path.name}".encode("utf-8")
    path.write_bytes(content)


def _make_standard_csv(
    path: Path,
    rows: list[dict[str, Any]],
    *,
    header_aliases: dict[str, str] | None = None,
) -> None:
    """Write a synthetic LIVE-VQC-style standard CSV.

    Default headers: ``name,url,mos,sd,n``. Pass ``header_aliases``
    to remap a standard key to an alternative spelling.
    """
    aliases = header_aliases or {}
    standard = ["name", "url", "mos", "sd", "n"]
    headers = [aliases.get(k, k) for k in standard]

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        fh.write(",".join(headers) + "\n")
        for row in rows:
            fh.write(
                "{name},{url},{mos},{sd},{n}\n".format(
                    name=row["name"],
                    url=row.get("url", ""),
                    mos=row["mos"],
                    sd=row.get("sd", ""),
                    n=row.get("n", ""),
                )
            )


def _make_two_column_csv(path: Path, rows: list[tuple[str, float]]) -> None:
    """Write the canonical headerless two-column ``<filename>, <mos>`` CSV."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for filename, mos in rows:
            fh.write(f"{filename},{mos}\n")


def _ffprobe_ok_payload(
    *,
    width: int = 1280,
    height: int = 720,
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
                    "duration": "6.000",
                    "pix_fmt": pix_fmt,
                    "codec_name": codec,
                }
            ],
            "format": {"duration": "6.000"},
        }
    )


class _FakeRunner:
    """Composable subprocess.run stand-in for ffprobe + curl.

    Routes by argv[0]: ``ffprobe`` returns the canned ffprobe JSON;
    ``curl`` writes a placeholder file at ``--output`` to simulate a
    successful download. Behaviour can be overridden per-target-name.
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
            Path(output_path).parent.mkdir(parents=True, exist_ok=True)
            Path(output_path).write_bytes(f"downloaded:{target_name}".encode("utf-8"))
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")
        raise AssertionError(f"unexpected runner invocation: {cmd!r}")


def _scaffold_corpus_standard(
    tmp_path: Path,
    *,
    clip_names: list[str],
    csv_rows: list[dict[str, Any]] | None = None,
    pre_existing_clips: list[str] | None = None,
    n_csv_rows: int | None = None,
) -> Path:
    """Build a minimal ``.workingdir2/live-vqc/``-shaped tree (standard CSV)."""
    live_vqc_dir = tmp_path / "live-vqc"
    live_vqc_dir.mkdir(parents=True)
    clips_dir = live_vqc_dir / "clips"
    clips_dir.mkdir()

    if csv_rows is None:
        if n_csv_rows is None:
            n_csv_rows = len(clip_names)
        csv_rows = [
            {
                "name": clip_names[i] if i < len(clip_names) else f"v{i:03d}.mp4",
                "url": (
                    f"https://example.invalid/{clip_names[i]}"
                    if i < len(clip_names)
                    else f"https://example.invalid/v{i:03d}.mp4"
                ),
                "mos": 50.0 + 0.1 * i,
                "sd": 8.5,
                "n": 30,
            }
            for i in range(n_csv_rows)
        ]
    _make_standard_csv(live_vqc_dir / "manifest.csv", csv_rows)

    for name in pre_existing_clips or []:
        _make_clip(clips_dir / name)
    return live_vqc_dir


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


# ---------------------------------------------------------------------------
# 1. Canonical two-column headerless CSV auto-detection
# ---------------------------------------------------------------------------


def test_canonical_two_column_csv_parsed(tmp_path: Path) -> None:
    """Headerless ``<filename>, <mos>`` shape is auto-detected and parsed."""
    live_vqc_dir = tmp_path / "live-vqc"
    live_vqc_dir.mkdir()
    (live_vqc_dir / "clips").mkdir()

    rows_data = [(f"{i:03d}.mp4", 45.0 + i * 2.5) for i in range(60)]
    _make_two_column_csv(live_vqc_dir / "manifest.csv", rows_data)

    # Pre-stage all clips so no download is attempted.
    for name, _ in rows_data:
        _make_clip(live_vqc_dir / "clips" / name)

    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=50,
        max_rows=None,
    )

    assert stats.written == 60
    rows = _read_jsonl(output)
    assert len(rows) == 60
    # MOS values should be verbatim from the two-column CSV.
    mos_values = {r["src"]: r["mos"] for r in rows}
    assert mos_values["000.mp4"] == pytest.approx(45.0)
    assert mos_values["001.mp4"] == pytest.approx(47.5)
    # mos_std_dev and n_ratings default to 0 / 0 for two-column shape.
    assert rows[0]["mos_std_dev"] == pytest.approx(0.0)
    assert rows[0]["n_ratings"] == 0


def test_canonical_two_column_bare_stem(tmp_path: Path) -> None:
    """Bare stems in the two-column CSV (no extension) get .mp4 appended."""
    live_vqc_dir = tmp_path / "live-vqc"
    live_vqc_dir.mkdir()
    (live_vqc_dir / "clips").mkdir()

    rows_data = [("001", 60.0), ("002", 70.0)]
    # Extend to minimum row count (50) by appending more rows.
    rows_data += [(f"{i + 3:03d}", 50.0) for i in range(50)]
    _make_two_column_csv(live_vqc_dir / "manifest.csv", rows_data)

    for stem, _ in rows_data:
        _make_clip(live_vqc_dir / "clips" / f"{stem}.mp4")

    output = tmp_path / "out.jsonl"
    LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=50,
        max_rows=None,
    )

    rows = _read_jsonl(output)
    src_names = {r["src"] for r in rows}
    assert "001.mp4" in src_names
    assert "002.mp4" in src_names


# ---------------------------------------------------------------------------
# 2. Standard adapter CSV (LSVQ header style)
# ---------------------------------------------------------------------------


def test_standard_csv_round_trips_all_mos_columns(tmp_path: Path) -> None:
    """Round-trip a standard CSV and assert all three MOS columns survive."""
    live_vqc_dir = _scaffold_corpus_standard(
        tmp_path,
        clip_names=["a.mp4", "b.mp4"],
        csv_rows=[
            {
                "name": "a.mp4",
                "url": "https://example.invalid/a.mp4",
                "mos": 63.7,
                "sd": 9.2,
                "n": 45,
            },
            {
                "name": "b.mp4",
                "url": "https://example.invalid/b.mp4",
                "mos": 28.4,
                "sd": 14.1,
                "n": 38,
            },
        ],
    )
    output = tmp_path / "out.jsonl"
    LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    rows = _read_jsonl(output)
    by_src = {r["src"]: r for r in rows}
    assert by_src["a.mp4"]["mos"] == pytest.approx(63.7)
    assert by_src["a.mp4"]["mos_std_dev"] == pytest.approx(9.2)
    assert by_src["a.mp4"]["n_ratings"] == 45
    assert by_src["b.mp4"]["mos"] == pytest.approx(28.4)
    assert by_src["b.mp4"]["mos_std_dev"] == pytest.approx(14.1)
    assert by_src["b.mp4"]["n_ratings"] == 38


def test_standard_csv_alias_headers_accepted(tmp_path: Path) -> None:
    """Alias header spellings (MOS / mos_std / num_ratings / video_name) work."""
    live_vqc_dir = tmp_path / "live-vqc"
    live_vqc_dir.mkdir()
    (live_vqc_dir / "clips").mkdir()
    _make_standard_csv(
        live_vqc_dir / "manifest.csv",
        [
            {
                "name": "alias.mp4",
                "url": "https://example.invalid/alias.mp4",
                "mos": 55.5,
                "sd": 7.0,
                "n": 25,
            }
        ],
        header_aliases={
            "name": "video_name",
            "url": "download_url",
            "mos": "MOS",
            "sd": "mos_std",
            "n": "num_ratings",
        },
    )
    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=1,
        max_rows=None,
    )
    assert stats.written == 1
    row = _read_jsonl(output)[0]
    assert row["mos"] == pytest.approx(55.5)
    assert row["mos_std_dev"] == pytest.approx(7.0)
    assert row["n_ratings"] == 25


# ---------------------------------------------------------------------------
# 3. Resumable download — partial progress + restart picks up where left off
# ---------------------------------------------------------------------------


def test_resumable_download_picks_up_where_interrupted(tmp_path: Path) -> None:
    """Write a partial progress file, re-run, assert only missing clips retried."""
    clips = ["p1.mp4", "p2.mp4", "p3.mp4", "p4.mp4"]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clips)
    progress_path = live_vqc_dir / ".download-progress.json"

    _make_clip(live_vqc_dir / "clips" / "p1.mp4")
    _make_clip(live_vqc_dir / "clips" / "p2.mp4")
    progress_path.write_text(
        json.dumps(
            {
                "p1.mp4": {"state": "done"},
                "p2.mp4": {"state": "done"},
            }
        ),
        encoding="utf-8",
    )

    runner = _FakeRunner()
    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
        max_rows=None,
    )

    assert sorted(runner.download_attempts) == ["p3.mp4", "p4.mp4"]
    assert stats.written == 4
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0

    state_after = json.loads(progress_path.read_text(encoding="utf-8"))
    assert {k: v["state"] for k, v in state_after.items()} == {
        "p1.mp4": "done",
        "p2.mp4": "done",
        "p3.mp4": "done",
        "p4.mp4": "done",
    }


def test_resumable_download_persists_failures(tmp_path: Path) -> None:
    """A failed download writes to progress; re-run honours the non-retry contract."""
    clips = ["ok.mp4", "gone.mp4"]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clips)
    progress_path = live_vqc_dir / ".download-progress.json"

    runner1 = _FakeRunner(download_failures={"gone.mp4"})
    output = tmp_path / "out.jsonl"
    stats1 = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
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
    stats2 = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=runner2,
        min_csv_rows=2,
        max_rows=None,
    )
    assert "gone.mp4" not in runner2.download_attempts
    assert stats2.skipped_download == 1


# ---------------------------------------------------------------------------
# 4. Attrition tolerance — failures produce a warning, not crash
# ---------------------------------------------------------------------------


def test_attrition_threshold_warning(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    """Mocked download failures above threshold fire a WARNING; run completes."""
    n = 80
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 80, 10)}  # 8 failures = 10%
    assert len(failed) == 8
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        stats = LIVE_VQC.run(
            live_vqc_dir=live_vqc_dir,
            output=output,
            runner=runner,
            attrition_warn_threshold=0.05,
            min_csv_rows=2,
            max_rows=None,
        )

    assert stats.written == n - len(failed)
    assert stats.skipped_download == len(failed)
    assert stats.attrition_pct == pytest.approx(len(failed) / n)
    log_text = caplog.text
    assert "attrition" in log_text.lower()
    assert "exceeds" in log_text.lower()


def test_attrition_below_threshold_no_warning(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """Failures below the 10% threshold produce no attrition warning."""
    n = 60
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 18, 10)}  # 2 failures ≈ 3.3%
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        LIVE_VQC.run(
            live_vqc_dir=live_vqc_dir,
            output=output,
            runner=runner,
            min_csv_rows=2,
            max_rows=None,
        )

    assert "exceeds advisory threshold" not in caplog.text


# ---------------------------------------------------------------------------
# 5. Refuse-tiny cutoff — < 50 rows aborts
# ---------------------------------------------------------------------------


def test_refuses_tiny_csv(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """A CSV with < 50 rows must abort with a clear error."""
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=["only_one.mp4"], n_csv_rows=20)

    rc = LIVE_VQC.main(
        [
            "--live-vqc-dir",
            str(live_vqc_dir),
            "--output",
            str(tmp_path / "should_not_be_written.jsonl"),
        ]
    )
    assert rc != 0
    captured = capsys.readouterr()
    blob = captured.out + captured.err
    assert "sanity floor" in blob.lower() or "live-vqc" in blob.lower()


# ---------------------------------------------------------------------------
# 6. Geometry parse from ffprobe JSON
# ---------------------------------------------------------------------------


def test_geometry_parse_from_ffprobe_json(tmp_path: Path) -> None:
    """One mocked ffprobe (hevc, 720p) → row's geometry fields match."""
    live_vqc_dir = _scaffold_corpus_standard(
        tmp_path,
        clip_names=["clipA.mp4"],
        pre_existing_clips=["clipA.mp4"],
    )
    runner = _FakeRunner(
        ffprobe_payload=_ffprobe_ok_payload(
            width=1280, height=720, codec="hevc", pix_fmt="yuv420p"
        ),
    )
    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )

    rows = _read_jsonl(output)
    assert len(rows) == 1
    row = rows[0]
    assert row["src"] == "clipA.mp4"
    assert row["width"] == 1280
    assert row["height"] == 720
    assert row["framerate"] == pytest.approx(30.0)
    assert row["pix_fmt"] == "yuv420p"
    assert row["encoder_upstream"] == "hevc"
    assert row["duration_s"] == pytest.approx(6.0)
    assert stats.skipped_download == 0
    assert stats.skipped_broken == 0


# ---------------------------------------------------------------------------
# 7. Broken clips are skipped; run still completes
# ---------------------------------------------------------------------------


def test_skips_broken_clip_continues_run(tmp_path: Path) -> None:
    """One ffprobe failure must not abort the run — other clips still emit."""
    clip_names = ["good1.mp4", "broken.mp4", "good2.mp4"]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    runner = _FakeRunner(ffprobe_failures={"broken.mp4"})

    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
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
    """Re-running with the same corpus and output dedups by src_sha256."""
    clip_names = ["x.mp4", "y.mp4"]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    output = tmp_path / "out.jsonl"

    s1 = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s1.written == 2

    s2 = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s2.written == 0
    assert s2.dedups == 2
    assert len(_read_jsonl(output)) == 2


# ---------------------------------------------------------------------------
# 9. corpus / corpus_version metadata constants + schema key set
# ---------------------------------------------------------------------------


def test_corpus_metadata_columns_and_schema(tmp_path: Path) -> None:
    """Pin ``corpus`` = 'live-vqc', ``corpus_version`` non-empty, exact key set."""
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=["c.mp4"])
    output = tmp_path / "out.jsonl"
    LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        corpus_version="live-vqc-2019",
        min_csv_rows=1,
        max_rows=None,
    )
    row = _read_jsonl(output)[0]
    assert row["corpus"] == "live-vqc"
    assert isinstance(row["corpus_version"], str) and row["corpus_version"]
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
    """The script's docstring must state the no-redistribute posture + ADR ref."""
    doc = LIVE_VQC.__doc__ or ""
    assert "research use" in doc.lower() or "research-use" in doc.lower()
    assert "does **not** ship" in doc.lower() or "does not ship" in doc.lower()
    assert "ADR-0370" in doc
    assert "live.ece.utexas.edu" in doc


# ---------------------------------------------------------------------------
# 10. Missing-corpus-dir error path
# ---------------------------------------------------------------------------


def test_missing_live_vqc_dir_returns_clear_error(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    """Pointing the script at a non-existent dir surfaces an acquisition hint."""
    rc = LIVE_VQC.main(
        [
            "--live-vqc-dir",
            str(tmp_path / "nope"),
            "--output",
            str(tmp_path / "out.jsonl"),
        ]
    )
    assert rc != 0
    err = capsys.readouterr().err
    assert "live.ece.utexas.edu" in err or "live-vqc" in err.lower()


# ---------------------------------------------------------------------------
# 11. Progress file is atomically rewritten
# ---------------------------------------------------------------------------


def test_progress_file_is_atomic(tmp_path: Path) -> None:
    """save_progress must rename atomically — no half-truncated JSON survives."""
    progress_path = tmp_path / ".download-progress.json"
    LIVE_VQC.save_progress(progress_path, {"a.mp4": {"state": "done"}})
    payload = json.loads(progress_path.read_text(encoding="utf-8"))
    assert payload == {"a.mp4": {"state": "done"}}

    LIVE_VQC.save_progress(progress_path, {"b.mp4": {"state": "failed", "reason": "x"}})
    payload2 = json.loads(progress_path.read_text(encoding="utf-8"))
    assert "b.mp4" in payload2
    assert "a.mp4" not in payload2
    leftovers = list(tmp_path.glob(".download-progress.*.tmp"))
    assert leftovers == []


# ---------------------------------------------------------------------------
# 12. --max-rows cap + --full opt-in
# ---------------------------------------------------------------------------


def test_max_rows_caps_ingestion(tmp_path: Path) -> None:
    """max_rows=10 against a 100-row CSV ingests only the first 10."""
    n = 100
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)

    output = tmp_path / "out.jsonl"
    stats = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=10,
    )

    assert stats.written == 10
    rows = _read_jsonl(output)
    assert {r["src"] for r in rows} == {f"clip{i:03d}.mp4" for i in range(10)}


def test_full_flag_disables_max_rows_cap(tmp_path: Path) -> None:
    """``max_rows=None`` overrides any default cap and ingests the whole CSV."""
    n = 60
    clip_names = [f"clip{i:03d}.mp4" for i in range(n)]
    live_vqc_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)

    output_capped = tmp_path / "capped.jsonl"
    stats_capped = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output_capped,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=20,
    )
    assert stats_capped.written == 20

    output_full = tmp_path / "full.jsonl"
    stats_full = LIVE_VQC.run(
        live_vqc_dir=live_vqc_dir,
        output=output_full,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert stats_full.written == n

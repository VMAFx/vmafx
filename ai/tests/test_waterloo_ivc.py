# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.waterloo_ivc_to_corpus_jsonl` (ADR-0369).

These tests exercise the Waterloo IVC 4K-VQA adapter's pure-
Python contract end-to-end on synthetic fixtures. They do
**not** require ffprobe or curl on the host, nor do they need
the multi-TB Waterloo IVC corpus on disk — every external call
is routed through the script's injectable ``runner`` seam.

Waterloo-IVC-specific paths covered:

* Canonical headerless ``encoder, vid, res, dist, mos`` 5-tuple
  is auto-detected and parsed into synthesised filenames.
* Standard-CSV (LSVQ / KonViD-150k) shape is also accepted when
  an operator pre-mangles ``scores.txt`` into the named-column
  format.
* Native 0–100 MOS scale survives round-trip verbatim (no
  rescaling at ingest).
* Resumable downloads — partial progress JSON + restart picks
  up where the prior run left off.
* Attrition tolerance — mocked failure rate above
  ``--attrition-warn-threshold`` produces a WARNING summary, not
  an exception.
* Refuse-tiny cutoff — passing a < 100-row CSV exits non-zero
  with a hint pointing at the canonical 1 200-row scores table.
* ``--max-rows`` cap — laptop-class default ingests only the
  first N rows; ``--full`` opts into whole-corpus ingestion.

Plus the standard cross-corpus paths (geometry parse, broken-
clip skip, append+dedup on re-run, atomic progress writes,
license-text wording).
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
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "waterloo_ivc_to_corpus_jsonl.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("waterloo_ivc_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


WATERLOO = _load_module()


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _make_clip(path: Path, *, content: bytes | None = None) -> None:
    """Create a placeholder clip file (content irrelevant under mocked ffprobe)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if content is None:
        content = f"waterloo-fixture:{path.name}".encode("utf-8")
    path.write_bytes(content)


def _make_canonical_scores_txt(path: Path, rows: list[tuple[str, str, str, str, float]]) -> None:
    """Write a synthetic Waterloo IVC canonical headerless ``scores.txt``.

    Rows are ``(encoder, video_number, resolution, distortion, mos)``.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for enc, vid, res, dist, mos in rows:
            fh.write(f"{enc}, {vid}, {res}, {dist}, {mos}\n")


def _make_standard_csv(
    path: Path,
    rows: list[dict[str, Any]],
    *,
    header_aliases: dict[str, str] | None = None,
) -> None:
    """Write a synthetic LSVQ / KonViD-150k-shaped CSV for the standard-shape branch."""
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


def _ffprobe_ok_payload(
    *,
    width: int = 3840,
    height: int = 2160,
    codec: str = "hevc",
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
                    "duration": "10.000",
                    "pix_fmt": pix_fmt,
                    "codec_name": codec,
                }
            ],
            "format": {"duration": "10.000"},
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


def _scaffold_corpus_canonical(
    tmp_path: Path,
    *,
    clip_count: int = 10,
    pre_existing_clips: list[str] | None = None,
) -> Path:
    """Build a minimal ``.workingdir2/waterloo-ivc-4k/``-shaped tree with the
    canonical headerless ``scores.txt`` shape."""
    waterloo_dir = tmp_path / "waterloo-ivc-4k"
    waterloo_dir.mkdir(parents=True)
    (waterloo_dir / "clips").mkdir()

    encoders = ("HEVC", "AVC", "VP9", "AVS2", "AV1")
    resolutions = ("540p", "1080p", "2160p")
    distortions = ("1", "2", "3", "4")
    rows: list[tuple[str, str, str, str, float]] = []
    # Iterate the (encoder × video × resolution × distortion) lattice
    # so every synthesised filename is unique within a single scaffold.
    # 5 encoders × 20 videos × 3 resolutions × 4 distortions = 1 200
    # combinations, comfortably above any test's clip_count.
    seen = 0
    for enc in encoders:
        for vid_i in range(1, 21):
            for res in resolutions:
                for dist in distortions:
                    if seen >= clip_count:
                        break
                    mos = 10.0 + (seen * 0.7) % 80.0
                    rows.append((enc, str(vid_i), res, dist, round(mos, 2)))
                    seen += 1
                if seen >= clip_count:
                    break
            if seen >= clip_count:
                break
        if seen >= clip_count:
            break
    _make_canonical_scores_txt(waterloo_dir / "manifest.csv", rows)

    # Canonical headerless scores.txt has no URL column — clips must
    # already exist on disk (operator-staged via the bulk archive
    # download). Pre-stage every synthesised filename to mirror the
    # post-archive-extraction state.
    for enc, vid, res, dist, _mos in rows:
        fname = WATERLOO._synthesise_canonical_filename(enc, vid, res, dist, suffix=".yuv")
        _make_clip(waterloo_dir / "clips" / fname)

    for name in pre_existing_clips or []:
        _make_clip(waterloo_dir / "clips" / name)
    return waterloo_dir


def _scaffold_corpus_standard(
    tmp_path: Path,
    *,
    clip_names: list[str],
) -> Path:
    """Build a tree with the standard-shape (LSVQ / KonViD-150k) CSV."""
    waterloo_dir = tmp_path / "waterloo-ivc-4k"
    waterloo_dir.mkdir(parents=True)
    (waterloo_dir / "clips").mkdir()
    rows = [
        {
            "name": clip_names[i],
            "url": f"https://example.invalid/{clip_names[i]}",
            "mos": 25.0 + 5.0 * i,
            "sd": 3.21,
            "n": 30,
        }
        for i in range(len(clip_names))
    ]
    _make_standard_csv(waterloo_dir / "manifest.csv", rows)
    return waterloo_dir


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    return [
        json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    ]


# ---------------------------------------------------------------------------
# 1. Canonical headerless 5-tuple is auto-detected
# ---------------------------------------------------------------------------


def test_canonical_headerless_scores_txt_detected(tmp_path: Path) -> None:
    """Upstream ``scores.txt`` shape (no header, 5 columns) parses correctly."""
    waterloo_dir = _scaffold_corpus_canonical(tmp_path, clip_count=120)
    output = tmp_path / "out.jsonl"
    runner = _FakeRunner()
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner,
        min_csv_rows=10,
        max_rows=None,
    )
    assert stats.written == 120
    rows = _read_jsonl(output)
    assert len(rows) == 120
    # Synthesised filename matches the canonical encoder_vid_res_dist convention.
    first_src = rows[0]["src"]
    parts = first_src.rsplit(".", 1)[0].split("_")
    assert len(parts) == 4
    assert parts[0] in {"HEVC", "AVC", "VP9", "AVS2", "AV1"}


def test_canonical_headerless_synthesises_filename(tmp_path: Path) -> None:
    """Filename synthesis uses the encoder_vid_res_dist convention."""
    fname = WATERLOO._synthesise_canonical_filename("HEVC", "1", "540p", "1", suffix=".yuv")
    assert fname == "HEVC_1_540p_1.yuv"


# ---------------------------------------------------------------------------
# 2. Standard CSV shape (pre-mangled) is also accepted
# ---------------------------------------------------------------------------


def test_standard_csv_shape_parses(tmp_path: Path) -> None:
    """LSVQ / KonViD-150k-shaped CSV header is recognised when present."""
    waterloo_dir = tmp_path / "waterloo-ivc-4k"
    waterloo_dir.mkdir()
    (waterloo_dir / "clips").mkdir()
    rows = [
        {
            "name": f"clip{i:03d}.mp4",
            "url": f"https://example.invalid/clip{i:03d}.mp4",
            "mos": 30.0 + i * 0.5,
            "sd": 2.5,
            "n": 28,
        }
        for i in range(150)
    ]
    _make_standard_csv(waterloo_dir / "manifest.csv", rows)

    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=10,
        max_rows=None,
    )
    assert stats.written == 150
    out_rows = _read_jsonl(output)
    by_src = {r["src"]: r for r in out_rows}
    assert "clip000.mp4" in by_src
    assert by_src["clip000.mp4"]["mos"] == pytest.approx(30.0)
    assert by_src["clip000.mp4"]["mos_std_dev"] == pytest.approx(2.5)
    assert by_src["clip000.mp4"]["n_ratings"] == 28


def test_standard_csv_alias_headers(tmp_path: Path) -> None:
    """Alias spellings on the standard branch (MOS / video_name / num_ratings)."""
    waterloo_dir = tmp_path / "waterloo-ivc-4k"
    waterloo_dir.mkdir()
    (waterloo_dir / "clips").mkdir()
    rows = [
        {
            "name": f"alias{i}.mp4",
            "url": f"https://example.invalid/alias{i}.mp4",
            "mos": 40.5,
            "sd": 1.1,
            "n": 24,
        }
        for i in range(110)
    ]
    _make_standard_csv(
        waterloo_dir / "manifest.csv",
        rows,
        header_aliases={
            "name": "video_name",
            "url": "download_url",
            "mos": "MOS",
            "sd": "mos_std",
            "n": "num_ratings",
        },
    )
    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=10,
        max_rows=None,
    )
    assert stats.written == 110
    row = _read_jsonl(output)[0]
    assert row["mos"] == pytest.approx(40.5)
    assert row["mos_std_dev"] == pytest.approx(1.1)
    assert row["n_ratings"] == 24


# ---------------------------------------------------------------------------
# 3. MOS native 0–100 scale survives verbatim
# ---------------------------------------------------------------------------


def test_mos_native_0_100_scale_verbatim(tmp_path: Path) -> None:
    """Waterloo IVC MOS lives on 0–100; ingest must record verbatim, no rescaling."""
    waterloo_dir = tmp_path / "waterloo-ivc-4k"
    waterloo_dir.mkdir()
    (waterloo_dir / "clips").mkdir()
    # Pin 4 rows with known canonical MOS values from upstream scores.txt.
    canonical_rows = [
        ("HEVC", "1", "540p", "1", 18.21),
        ("HEVC", "1", "540p", "2", 39.46),
        ("HEVC", "1", "540p", "3", 50.23),
        ("HEVC", "1", "540p", "4", 77.26),
    ]
    # Pad to satisfy the min_rows=2 floor used in this test.
    padded = canonical_rows + [("VP9", str(i), "1080p", "1", 22.5 + i * 0.1) for i in range(2, 200)]
    _make_canonical_scores_txt(waterloo_dir / "manifest.csv", padded)
    # Pre-stage clips for every row (canonical scores.txt has no URLs).
    for enc, vid, res, dist, _mos in padded:
        fname = WATERLOO._synthesise_canonical_filename(enc, vid, res, dist, suffix=".yuv")
        _make_clip(waterloo_dir / "clips" / fname)

    output = tmp_path / "out.jsonl"
    WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=4,
    )
    rows = _read_jsonl(output)
    assert len(rows) == 4
    mos_values = sorted(r["mos"] for r in rows)
    assert mos_values == pytest.approx([18.21, 39.46, 50.23, 77.26])
    # All values must be in the 0–100 native band (no 1–5 rescaling).
    for r in rows:
        assert 0.0 <= r["mos"] <= 100.0


# ---------------------------------------------------------------------------
# 4. Resumable download — partial progress + restart picks up where left off
# ---------------------------------------------------------------------------


def test_resumable_download_picks_up_where_interrupted(tmp_path: Path) -> None:
    """Write a partial progress file, re-run, assert only missing clips retried."""
    waterloo_dir = _scaffold_corpus_standard(
        tmp_path,
        clip_names=["a.yuv", "b.yuv", "c.yuv", "d.yuv"],
    )
    progress_path = waterloo_dir / ".download-progress.json"

    _make_clip(waterloo_dir / "clips" / "a.yuv")
    _make_clip(waterloo_dir / "clips" / "b.yuv")
    progress_path.write_text(
        json.dumps({"a.yuv": {"state": "done"}, "b.yuv": {"state": "done"}}),
        encoding="utf-8",
    )

    runner = _FakeRunner()
    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
        max_rows=None,
    )
    assert sorted(runner.download_attempts) == ["c.yuv", "d.yuv"]
    assert stats.written == 4
    assert stats.skipped_download == 0


def test_resumable_download_persists_failures(tmp_path: Path) -> None:
    """A failed download writes to progress; re-run honours non-retry contract."""
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=["ok.yuv", "gone.yuv"])
    progress_path = waterloo_dir / ".download-progress.json"

    runner1 = _FakeRunner(download_failures={"gone.yuv"})
    output = tmp_path / "out.jsonl"
    stats1 = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner1,
        min_csv_rows=2,
        max_rows=None,
    )
    assert stats1.written == 1
    assert stats1.skipped_download == 1
    state_after = json.loads(progress_path.read_text(encoding="utf-8"))
    assert state_after["gone.yuv"]["state"] == "failed"
    assert "404" in state_after["gone.yuv"]["reason"]

    runner2 = _FakeRunner()  # would succeed if attempted
    stats2 = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner2,
        min_csv_rows=2,
        max_rows=None,
    )
    assert "gone.yuv" not in runner2.download_attempts
    assert stats2.written == 0
    assert stats2.skipped_download == 1


# ---------------------------------------------------------------------------
# 5. Attrition tolerance
# ---------------------------------------------------------------------------


def test_attrition_threshold_warning(tmp_path: Path, caplog: pytest.LogCaptureFixture) -> None:
    """8% mocked download failures: run completes, WARNING summary fires."""
    n = 100
    clip_names = [f"clip{i:03d}.yuv" for i in range(n)]
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 80, 10)}
    assert len(failed) == 8
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        stats = WATERLOO.run(
            waterloo_ivc_dir=waterloo_dir,
            output=output,
            runner=runner,
            attrition_warn_threshold=0.05,
            min_csv_rows=2,
            max_rows=None,
        )
    assert stats.written == n - len(failed)
    assert stats.skipped_download == len(failed)
    assert stats.attrition_pct == pytest.approx(0.08)
    assert "attrition" in caplog.text.lower()
    assert "exceeds" in caplog.text.lower()


def test_attrition_below_threshold_no_warning(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """3% download failures stay below the 10% default; no attrition warning."""
    n = 100
    clip_names = [f"clip{i:03d}.yuv" for i in range(n)]
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    failed = {clip_names[i] for i in range(0, 30, 10)}
    runner = _FakeRunner(download_failures=failed)

    output = tmp_path / "out.jsonl"
    with caplog.at_level(logging.WARNING):
        WATERLOO.run(
            waterloo_ivc_dir=waterloo_dir,
            output=output,
            runner=runner,
            min_csv_rows=2,
            max_rows=None,
        )
    assert "exceeds advisory threshold" not in caplog.text


# ---------------------------------------------------------------------------
# 6. Refuse-tiny cutoff — < 100 rows aborts
# ---------------------------------------------------------------------------


def test_refuses_tiny_csv(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    """A CSV with < 100 rows must abort with a clear error."""
    waterloo_dir = _scaffold_corpus_canonical(tmp_path, clip_count=20)
    rc = WATERLOO.main(
        [
            "--waterloo-ivc-dir",
            str(waterloo_dir),
            "--output",
            str(tmp_path / "should_not_be_written.jsonl"),
        ]
    )
    assert rc != 0
    captured = capsys.readouterr()
    blob = captured.out + captured.err
    assert "waterloo" in blob.lower() or "sanity floor" in blob.lower()


# ---------------------------------------------------------------------------
# 7. Geometry parse from ffprobe JSON (4K codec)
# ---------------------------------------------------------------------------


def test_geometry_parse_from_ffprobe_json(tmp_path: Path) -> None:
    """One mocked ffprobe (HEVC, 2160p) → row's geometry fields match."""
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=["clip4k.yuv"])
    _make_clip(waterloo_dir / "clips" / "clip4k.yuv")
    runner = _FakeRunner(
        ffprobe_payload=_ffprobe_ok_payload(
            width=3840, height=2160, codec="hevc", pix_fmt="yuv420p10le"
        ),
    )
    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )
    rows = _read_jsonl(output)
    assert len(rows) == 1
    row = rows[0]
    assert row["src"] == "clip4k.yuv"
    assert row["width"] == 3840
    assert row["height"] == 2160
    assert row["framerate"] == pytest.approx(30.0)
    assert row["pix_fmt"] == "yuv420p10le"
    assert row["encoder_upstream"] == "hevc"
    assert row["duration_s"] == pytest.approx(10.0)
    assert stats.skipped_broken == 0


# ---------------------------------------------------------------------------
# 8. Broken clips skip; run still completes
# ---------------------------------------------------------------------------


def test_skips_broken_clip_continues_run(tmp_path: Path) -> None:
    """One ffprobe failure must not abort the run — other clips still emit."""
    clip_names = ["good1.yuv", "broken.yuv", "good2.yuv"]
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=clip_names)
    runner = _FakeRunner(ffprobe_failures={"broken.yuv"})

    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner,
        min_csv_rows=2,
        max_rows=None,
    )
    assert stats.written == 2
    assert stats.skipped_broken == 1
    rows = _read_jsonl(output)
    assert {r["src"] for r in rows} == {"good1.yuv", "good2.yuv"}


# ---------------------------------------------------------------------------
# 9. Append + dedup on re-run (idempotency)
# ---------------------------------------------------------------------------


def test_existing_jsonl_dedup_on_rerun(tmp_path: Path) -> None:
    """Re-running with the same corpus and the same output dedups by src_sha256."""
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=["a.yuv", "b.yuv"])
    output = tmp_path / "out.jsonl"

    s1 = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s1.written == 2
    s2 = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=2,
        max_rows=None,
    )
    assert s2.written == 0
    assert s2.dedups == 2
    assert len(_read_jsonl(output)) == 2


# ---------------------------------------------------------------------------
# 10. corpus / corpus_version metadata constants + license-text wording
# ---------------------------------------------------------------------------


def test_corpus_metadata_columns_constant(tmp_path: Path) -> None:
    """Pin ``corpus`` = 'waterloo-ivc-4k' and the canonical schema."""
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=["c.yuv"])
    output = tmp_path / "out.jsonl"
    WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        corpus_version="waterloo-ivc-4k-201908",
        min_csv_rows=1,
        max_rows=None,
    )
    row = _read_jsonl(output)[0]
    assert row["corpus"] == "waterloo-ivc-4k"
    assert row["corpus_version"] == "waterloo-ivc-4k-201908"
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
    doc = WATERLOO.__doc__ or ""
    assert "Waterloo" in doc or "IVC" in doc
    assert "ADR-0369" in doc
    # Permissive academic licence acknowledgement requirement is recorded.
    assert "Image and Vision Computing" in doc or "IVC" in doc
    # Native 0–100 MOS scale and the cross-corpus caveat are documented.
    assert "0" in doc and "100" in doc
    # Provenance: canonical scores.txt URL is cited.
    assert "ivc.uwaterloo.ca" in doc


# ---------------------------------------------------------------------------
# 11. Missing-corpus-dir error path
# ---------------------------------------------------------------------------


def test_missing_corpus_dir_returns_clear_error(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    """Pointing the script at a non-existent dir surfaces a download hint."""
    rc = WATERLOO.main(
        [
            "--waterloo-ivc-dir",
            str(tmp_path / "nope"),
            "--output",
            str(tmp_path / "out.jsonl"),
        ]
    )
    assert rc != 0
    err = capsys.readouterr().err
    assert "ivc.uwaterloo.ca" in err.lower() or "scores.txt" in err.lower()


# ---------------------------------------------------------------------------
# 12. Atomic progress-file rewrite
# ---------------------------------------------------------------------------


def test_progress_file_is_atomic(tmp_path: Path) -> None:
    """save_progress must rename atomically — no half-truncated JSON survives."""
    progress_path = tmp_path / ".download-progress.json"
    WATERLOO.save_progress(progress_path, {"a.yuv": {"state": "done"}})
    payload = json.loads(progress_path.read_text(encoding="utf-8"))
    assert payload == {"a.yuv": {"state": "done"}}

    WATERLOO.save_progress(progress_path, {"b.yuv": {"state": "failed", "reason": "x"}})
    payload2 = json.loads(progress_path.read_text(encoding="utf-8"))
    assert "b.yuv" in payload2
    assert "a.yuv" not in payload2
    leftovers = list(tmp_path.glob(".download-progress.*.tmp"))
    assert leftovers == []


# ---------------------------------------------------------------------------
# 13. --max-rows cap (laptop-class default) + --full opts in
# ---------------------------------------------------------------------------


def test_max_rows_caps_ingestion(tmp_path: Path) -> None:
    """max_rows=10 against a 200-row CSV ingests only the first 10."""
    waterloo_dir = _scaffold_corpus_canonical(tmp_path, clip_count=200)
    output = tmp_path / "out.jsonl"
    stats = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=_FakeRunner(),
        min_csv_rows=10,
        max_rows=10,
    )
    assert stats.written == 10
    assert len(_read_jsonl(output)) == 10


def test_full_flag_disables_max_rows_cap(tmp_path: Path) -> None:
    """``max_rows=None`` (the ``--full`` path) ingests the whole CSV."""
    waterloo_dir = _scaffold_corpus_canonical(tmp_path, clip_count=150)
    output_capped = tmp_path / "capped.jsonl"
    stats_capped = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output_capped,
        runner=_FakeRunner(),
        min_csv_rows=10,
        max_rows=20,
    )
    assert stats_capped.written == 20

    output_full = tmp_path / "full.jsonl"
    stats_full = WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output_full,
        runner=_FakeRunner(),
        min_csv_rows=10,
        max_rows=None,
    )
    assert stats_full.written == 150


# ---------------------------------------------------------------------------
# 14. Encoder name from ffprobe is recorded verbatim (no upfront collapse)
# ---------------------------------------------------------------------------


def test_encoder_upstream_recorded_verbatim(tmp_path: Path) -> None:
    """ENCODER_VOCAB v4 collapse is trainer-side; ingest records ffprobe codec verbatim."""
    waterloo_dir = _scaffold_corpus_standard(tmp_path, clip_names=["av1_clip.yuv"])
    runner = _FakeRunner(
        ffprobe_payload=_ffprobe_ok_payload(
            width=3840, height=2160, codec="av1", pix_fmt="yuv420p10le"
        ),
    )
    output = tmp_path / "out.jsonl"
    WATERLOO.run(
        waterloo_ivc_dir=waterloo_dir,
        output=output,
        runner=runner,
        min_csv_rows=1,
        max_rows=None,
    )
    row = _read_jsonl(output)[0]
    # Native ffprobe codec_name; the ENCODER_VOCAB v4 collapse to
    # "professional-graded" (per ADR-0369) lives in the trainer,
    # not in this adapter.
    assert row["encoder_upstream"] == "av1"
    assert "professional-graded" not in row["encoder_upstream"]

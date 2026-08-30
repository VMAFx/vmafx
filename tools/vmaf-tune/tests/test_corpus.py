# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase A smoke tests — mocks ffmpeg + vmaf so no binaries required."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CORPUS_ROW_KEYS, SCHEMA_VERSION  # noqa: E402
from vmaftune.codec_adapters import get_adapter, known_codecs  # noqa: E402
from vmaftune.corpus import (  # noqa: E402
    CorpusJob,
    CorpusOptions,
    coarse_grid_crfs,
    coarse_to_fine_search,
    fine_grid_crfs,
    iter_rows,
    write_jsonl,
)
from vmaftune.encode import (  # noqa: E402
    EncodeRequest,
    build_ffmpeg_command,
    iter_grid,
    parse_versions,
)
from vmaftune.score import ScoreRequest, build_vmaf_command, parse_vmaf_json  # noqa: E402


class _FakeCompleted:
    """Minimal stand-in for ``subprocess.CompletedProcess``."""

    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _make_yuv(path: Path, nbytes: int = 1024) -> Path:
    path.write_bytes(b"\x80" * nbytes)
    return path


def test_known_codecs_includes_x264_and_multi_codec_adapters():
    codecs = known_codecs()
    assert "libx264" in codecs
    assert "libx265" in codecs  # multi-codec registry ships with x265+
    a = get_adapter("libx264")
    assert a.encoder == "libx264"
    assert a.invert_quality is True


def test_x264_validate_rejects_bad_inputs():
    a = get_adapter("libx264")
    a.validate("medium", 23)
    with pytest.raises(ValueError):
        a.validate("nope", 23)
    with pytest.raises(ValueError):
        a.validate("medium", 100)


def test_iter_grid_is_deterministic_and_complete():
    cells = iter_grid(["fast", "medium"], [22, 28, 34])
    assert cells == [
        ("fast", 22),
        ("fast", 28),
        ("fast", 34),
        ("medium", 22),
        ("medium", 28),
        ("medium", 34),
    ]


def test_build_ffmpeg_command_shape():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=Path("out.mp4"),
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    assert cmd[0] == "ffmpeg"
    assert "-c:v" in cmd and cmd[cmd.index("-c:v") + 1] == "libx264"
    assert "-preset" in cmd and cmd[cmd.index("-preset") + 1] == "medium"
    assert "-crf" in cmd and cmd[cmd.index("-crf") + 1] == "23"
    assert cmd[-1] == "out.mp4"


def test_build_vmaf_command_shape():
    req = ScoreRequest(
        reference=Path("ref.yuv"),
        distorted=Path("dist.mp4"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
    )
    cmd = build_vmaf_command(req, json_output=Path("v.json"), vmaf_bin="vmaf")
    assert cmd[0] == "vmaf"
    assert "--reference" in cmd
    assert "--pixel_format" in cmd
    assert cmd[cmd.index("--pixel_format") + 1] == "420"


def test_parse_versions_extracts_known_lines():
    stderr = "ffmpeg version 6.1.1 built with gcc\nx264 - core 164 r3107\n"
    ffm, enc = parse_versions(stderr)
    assert ffm == "6.1.1"
    assert enc == "libx264-164"


def test_parse_versions_returns_unknown_on_miss():
    assert parse_versions("nothing matches") == ("unknown", "unknown")


def test_parse_vmaf_json_modern_shape():
    payload = {"pooled_metrics": {"vmaf": {"mean": 91.42}}}
    assert parse_vmaf_json(payload) == pytest.approx(91.42)


def test_parse_vmaf_json_legacy_shape():
    assert parse_vmaf_json({"VMAF score": 88.0}) == pytest.approx(88.0)


def test_parse_vmaf_json_raises_on_missing():
    with pytest.raises(ValueError):
        parse_vmaf_json({})


def test_corpus_row_keys_match_init_contract():
    # Schema-shape contract — Phase B / C will rely on this. v2 added
    # ``clip_mode`` for the sample-clip mode (ADR-0301); v3 added the
    # HDR provenance triple (ADR-0300 status update 2026-05-08), the
    # canonical-6 per-feature aggregate columns (ADR-0331), and the ten
    # ``enc_internal_*`` aggregates (ADR-0332).
    assert SCHEMA_VERSION == 3
    assert "vmaf_score" in CORPUS_ROW_KEYS
    assert "bitrate_kbps" in CORPUS_ROW_KEYS
    assert "encode_time_ms" in CORPUS_ROW_KEYS
    assert "run_id" in CORPUS_ROW_KEYS
    assert "clip_mode" in CORPUS_ROW_KEYS
    assert "hdr_transfer" in CORPUS_ROW_KEYS
    assert "hdr_primaries" in CORPUS_ROW_KEYS
    assert "hdr_forced" in CORPUS_ROW_KEYS
    assert "adm2_mean" in CORPUS_ROW_KEYS
    assert "vif_scale0_mean" in CORPUS_ROW_KEYS
    assert "motion2_std" in CORPUS_ROW_KEYS
    assert "enc_internal_qp_mean" in CORPUS_ROW_KEYS
    assert "enc_internal_skip_ratio" in CORPUS_ROW_KEYS


def test_smoke_corpus_end_to_end_with_mocks(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        out_path = Path(cmd[-1])
        out_path.write_bytes(b"\x00" * 4096)  # encode-size proxy
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    def fake_score_run(cmd, capture_output, text, check):
        # write the JSON the parser expects
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 92.5}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23), ("slow", 28)),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))

    assert len(rows) == 2
    for r in rows:
        assert set(CORPUS_ROW_KEYS) == set(r.keys())
        assert r["encoder"] == "libx264"
        assert r["vmaf_score"] == pytest.approx(92.5)
        assert r["encode_size_bytes"] == 4096
        assert r["bitrate_kbps"] == pytest.approx(4096 * 8 / 1000 / 2.0)
        assert r["exit_status"] == 0
        assert r["schema_version"] == SCHEMA_VERSION

    # JSONL writer round-trip
    out = tmp_path / "out.jsonl"
    n = write_jsonl(rows, out)
    assert n == 2
    parsed = [json.loads(line) for line in out.read_text().splitlines()]
    assert parsed[0]["preset"] == "medium"
    assert parsed[1]["preset"] == "slow"


def test_build_ffmpeg_command_inserts_sample_clip_flags():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=Path("out.mp4"),
        sample_clip_seconds=10.0,
        sample_clip_start_s=25.0,
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    # -ss / -t must appear *before* -i so FFmpeg input-side seeks the
    # raw YUV instead of decoding the full source first.
    i_pos = cmd.index("-i")
    ss_pos = cmd.index("-ss")
    t_pos = cmd.index("-t")
    assert ss_pos < i_pos
    assert t_pos < i_pos
    assert cmd[ss_pos + 1] == "25.0"
    assert cmd[t_pos + 1] == "10.0"


def test_build_ffmpeg_command_no_sample_clip_flags_when_off():
    req = EncodeRequest(
        source=Path("ref.yuv"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        encoder="libx264",
        preset="medium",
        crf=23,
        output=Path("out.mp4"),
    )
    cmd = build_ffmpeg_command(req, ffmpeg_bin="ffmpeg")
    assert "-ss" not in cmd
    # `-t` is the encode-side flag we insert; FFmpeg has no other usage
    # in this command so its absence is the no-clip signal.
    assert "-t" not in cmd


def test_build_vmaf_command_appends_frame_skip_and_count():
    req = ScoreRequest(
        reference=Path("ref.yuv"),
        distorted=Path("dist.mp4"),
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        frame_skip_ref=600,
        frame_cnt=240,
    )
    cmd = build_vmaf_command(req, json_output=Path("v.json"), vmaf_bin="vmaf")
    assert "--frame_skip_ref" in cmd
    assert cmd[cmd.index("--frame_skip_ref") + 1] == "600"
    assert "--frame_cnt" in cmd
    assert cmd[cmd.index("--frame_cnt") + 1] == "240"


def test_sample_clip_mode_tags_rows_and_passes_argv(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")
    captured: dict[str, list[str]] = {}

    def fake_encode_run(cmd, capture_output, text, check):
        captured["encode"] = list(cmd)
        Path(cmd[-1]).write_bytes(b"\x00" * 4096)
        return _FakeCompleted(
            returncode=0,
            stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n",
        )

    def fake_score_run(cmd, capture_output, text, check):
        captured["score"] = list(cmd)
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 90.1}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=60.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
        sample_clip_seconds=10.0,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))

    assert len(rows) == 1
    assert rows[0]["clip_mode"] == "sample_10s"
    # Centre window of a 60s source: start = (60 - 10) / 2 = 25.0
    enc_cmd = captured["encode"]
    assert enc_cmd[enc_cmd.index("-ss") + 1] == "25.0"
    assert enc_cmd[enc_cmd.index("-t") + 1] == "10.0"
    # Score-side: 25 * 24 = 600 skip frames, 10 * 24 = 240 frames.
    score_cmd = captured["score"]
    assert score_cmd[score_cmd.index("--frame_skip_ref") + 1] == "600"
    assert score_cmd[score_cmd.index("--frame_cnt") + 1] == "240"
    # Bitrate is computed against the slice duration, not the source.
    assert rows[0]["bitrate_kbps"] == pytest.approx(4096 * 8 / 1000 / 10.0)


def test_sample_clip_mode_falls_back_to_full_when_source_too_short(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        Path(cmd[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        Path(cmd[out_idx]).parent.mkdir(parents=True, exist_ok=True)
        Path(cmd[out_idx]).write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 88.0}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=8.0,  # shorter than the 10s sample-clip request
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
        sample_clip_seconds=10.0,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))

    assert rows[0]["clip_mode"] == "full"


def test_default_full_clip_mode_tag(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")

    def fake_encode_run(cmd, capture_output, text, check):
        Path(cmd[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\n")

    def fake_score_run(cmd, capture_output, text, check):
        out_idx = cmd.index("--output") + 1
        Path(cmd[out_idx]).parent.mkdir(parents=True, exist_ok=True)
        Path(cmd[out_idx]).write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 95.0}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0\n")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=fake_encode_run, score_runner=fake_score_run))
    assert rows[0]["clip_mode"] == "full"


def test_corpus_row_keys_includes_clip_mode():
    assert "clip_mode" in CORPUS_ROW_KEYS
    # ADR-0332 bumped to v3 by adding the ten ``enc_internal_*`` columns.
    assert SCHEMA_VERSION == 3


def test_encode_failure_emits_row_with_skipped_score(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref.yuv")

    def failing_encode(cmd, capture_output, text, check):
        return _FakeCompleted(returncode=1, stderr="x264 boom")

    def never_score(*a, **kw):  # pragma: no cover - must not be called
        raise AssertionError("score must not run when encode failed")

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=failing_encode, score_runner=never_score))
    assert len(rows) == 1
    assert rows[0]["exit_status"] == 1
    assert rows[0]["vmaf_binary_version"] == "skipped"


# ---------------------------------------------------------------------------
# Coarse-to-fine search (ADR-0306)
# ---------------------------------------------------------------------------


def test_coarse_grid_crfs_canonical_5_points():
    # Defaults match the ADR-0296 canonical example.
    assert coarse_grid_crfs() == (10, 20, 30, 40, 50)


def test_coarse_grid_crfs_full_range():
    # Caller can opt back into the 0..51 sweep at the cost of 1 extra encode.
    assert coarse_grid_crfs(crf_min=0, crf_max=51, coarse_step=10) == (
        0,
        10,
        20,
        30,
        40,
        50,
    )


def test_coarse_grid_crfs_rejects_bad_step():
    with pytest.raises(ValueError):
        coarse_grid_crfs(coarse_step=0)
    with pytest.raises(ValueError):
        coarse_grid_crfs(crf_min=40, crf_max=10)


def test_fine_grid_crfs_canonical_around_30():
    # ±5 around CRF=30, step=1, exclude the coarse points -> 10 fine points.
    fine = fine_grid_crfs(
        30,
        fine_radius=5,
        fine_step=1,
        exclude=(10, 20, 30, 40, 50),
    )
    assert fine == (25, 26, 27, 28, 29, 31, 32, 33, 34, 35)
    assert len(fine) == 10


def test_fine_grid_crfs_clamps_at_boundary():
    # Best=2, radius=5 -> [-3..7] clamps to [0..7]; exclude=() so 0..7 = 8 pts.
    fine = fine_grid_crfs(2, fine_radius=5, fine_step=1)
    assert fine == (0, 1, 2, 3, 4, 5, 6, 7)


def _crf_to_score(crf: int) -> float:
    """Monotone decreasing VMAF model for tests.

    Starts at ~99 at CRF=0 and drops about 1.4 points per CRF unit, so
    the target=92 example bites at CRF=27 and best-coarse is CRF=30.
    """
    return 99.0 - 1.4 * crf


def _make_runners(*, scores_by_crf):
    """Build (encode, score) runners that synthesise rows for given CRFs.

    The score runner inspects the JSON output path's filename for the
    ``crf<N>`` token written by the encoder filename so each scored
    encode sees the right VMAF.
    """

    def fake_encode(cmd, capture_output, text, check):
        # ADR-0332: x264 corpus rows now run a pass-1 stats invocation
        # before the main encode. The pass-1 writes to /dev/null, has
        # no ``crf<N>`` filename token, and emits no version banner —
        # short-circuit it.
        if "-pass" in cmd and cmd[cmd.index("-pass") + 1] == "1":
            return _FakeCompleted(returncode=0, stderr="")
        out_path = Path(cmd[-1])
        out_path.write_bytes(b"\x00" * 4096)
        # Pull crf from the encode output filename, e.g. ref__libx264__medium__crf30.mp4
        crf = int(out_path.stem.rsplit("crf", 1)[-1])
        # Stash the CRF in stderr so the score runner can pick it up.
        return _FakeCompleted(
            returncode=0,
            stderr=(f"ffmpeg version 6.1.1\nx264 - core 164 r3107\n# crf={crf}\n"),
        )

    def fake_score(cmd, capture_output, text, check):
        # The score runner's --reference is the source; --distorted is the encode.
        dist_idx = cmd.index("--distorted") + 1
        dist = Path(cmd[dist_idx])
        crf = int(dist.stem.rsplit("crf", 1)[-1])
        score = scores_by_crf[crf]
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": score}}}))
        return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")

    return fake_encode, fake_score


def test_coarse_to_fine_canonical_visits_15_points(tmp_path: Path):
    """Coarse-to-fine with target=92 visits exactly 5 + 10 = 15 encodes."""
    src = _make_yuv(tmp_path / "ref.yuv")
    # Synthesise scores: monotone-decreasing in CRF; target=92 gates at ~CRF 5,
    # but we want best-coarse to be 30 to exercise refinement around the middle.
    scores = {c: _crf_to_score(c) for c in range(0, 52)}
    # Make CRF=30 score exactly 95 (passes target=92), CRF=40 score 85 (fails).
    # That makes best-coarse = 30 (highest CRF still passing).
    scores[10] = 99.0
    scores[20] = 96.0
    scores[30] = 92.5  # <- highest passing
    scores[40] = 85.0
    scores[50] = 70.0
    for c in range(11, 30):
        scores[c] = 92.5 + (30 - c) * 0.2  # passes
    for c in range(31, 40):
        scores[c] = 92.5 - (c - 30) * 0.5  # 31:92.0 ... 35:90.0 ... 39:88.0

    enc_run, score_run = _make_runners(scores_by_crf=scores)

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 0),),  # CRF axis is overridden by coarse-to-fine
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(
        coarse_to_fine_search(
            job,
            opts,
            target_vmaf=92.0,
            encode_runner=enc_run,
            score_runner=score_run,
        )
    )

    assert len(rows) == 15, f"expected 15 visited encodes, got {len(rows)}"
    visited_crfs = sorted({int(r["crf"]) for r in rows})
    # 5 coarse + 10 fine (25..29, 31..35) = 15 unique CRFs around the best=30
    assert visited_crfs == [
        10,
        20,
        25,
        26,
        27,
        28,
        29,
        30,
        31,
        32,
        33,
        34,
        35,
        40,
        50,
    ]


def test_coarse_to_fine_one_pass_shortcut_when_coarse_max_meets_target(tmp_path: Path):
    """If the highest-CRF coarse point already meets target, skip the fine pass."""
    src = _make_yuv(tmp_path / "ref.yuv")
    # All 5 coarse points pass target=70 -> best-coarse is CRF=50, the
    # max of the coarse grid -> refinement is skipped (1-pass shortcut).
    scores = {10: 99.0, 20: 95.0, 30: 90.0, 40: 80.0, 50: 75.0}
    # Fill rest defensively in case fine pass is wrongly invoked.
    for c in range(0, 52):
        scores.setdefault(c, _crf_to_score(c))

    enc_run, score_run = _make_runners(scores_by_crf=scores)

    job = CorpusJob(
        source=src,
        width=64,
        height=64,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 0),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        keep_encodes=False,
        src_sha256=False,
    )
    rows = list(
        coarse_to_fine_search(
            job,
            opts,
            target_vmaf=70.0,
            encode_runner=enc_run,
            score_runner=score_run,
        )
    )

    assert len(rows) == 5, f"expected 5 (coarse only), got {len(rows)}"
    visited_crfs = sorted({int(r["crf"]) for r in rows})
    assert visited_crfs == [10, 20, 30, 40, 50]


def test_coarse_to_fine_speedup_vs_full_grid_is_documented(tmp_path: Path):
    """Sanity-check the 3.5x speedup claim: 52 / 15 ~= 3.466..."""
    full_grid_points = 52  # CRF 0..51 step 1
    canonical_points = 15  # 5 coarse + 10 fine
    ratio = full_grid_points / canonical_points
    assert 3.4 < ratio < 3.5

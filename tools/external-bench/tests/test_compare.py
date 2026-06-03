# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""Unit tests for the external-competitor benchmark harness.

Every test here stubs ``subprocess.run`` so the test suite never
depends on the user having ``x264-pVMAF`` or ``dover-mobile``
installed. Per ADR-0332 the harness ships only wrappers; tests
exercise the schema-merge + aggregation + rendering paths.
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import compare  # noqa: E402

# --- test fixture constants -------------------------------------------------

EXPECTED_FRAMES_DEFAULT = 2
EXPECTED_PLCC_CANNED = 0.95
EXPECTED_RUNTIME_TOTAL_SUM = 300
EXPECTED_FAILED_RC = 3
EXPECTED_MISSING_CORPUS_RC = 4
FIXTURE_WIDTH = 576
FIXTURE_HEIGHT = 324
FIXTURE_DIS_COUNT = 2

# --- helpers ---------------------------------------------------------------


def _canned_output(competitor: str, n_frames: int = 3) -> dict:
    return {
        "frames": [
            {
                "frame_idx": i,
                "predicted_vmaf_or_mos": 80.0 + i,
                "runtime_ms": 1.5,
            }
            for i in range(n_frames)
        ],
        "summary": {
            "competitor": competitor,
            "plcc": 0.95,
            "srocc": 0.93,
            "rmse": 2.5,
            "runtime_total_ms": 1.5 * n_frames,
            "params": 12345,
            "gflops": 0.5,
        },
    }


def _make_stub_runner(by_competitor: dict[str, dict]):
    """Build a stub `subprocess.run` that writes canned JSON to --out."""

    def stub_run(cmd, *_args, **_kwargs):
        # Find --out, write canned JSON for the competitor inferred
        # from the wrapper path.
        out_idx = cmd.index("--out")
        out_path = pathlib.Path(cmd[out_idx + 1])
        wrapper = cmd[1]  # bash <wrapper>
        for name, _ in compare.WRAPPERS.items():
            if name in wrapper:
                competitor = name
                break
        else:
            raise AssertionError(f"unknown wrapper in cmd: {cmd}")
        out_path.write_text(json.dumps(by_competitor.get(competitor, _canned_output(competitor))))
        return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

    return stub_run


def _write_fake_vmaf_tune(tmp_path: pathlib.Path) -> pathlib.Path:
    """Create a tiny `vmaf-tune` stand-in for shell-wrapper smoke tests."""
    fake = tmp_path / "vmaf-tune"
    fake.write_text(
        """#!/usr/bin/env python3
import json
import pathlib
import sys

out = pathlib.Path(sys.argv[sys.argv.index("--json") + 1])
out.write_text(json.dumps({
    "frames": [{
        "frame": 0,
        "predicted_mos": 4.25,
        "predicted_vmaf": 91.5,
        "runtime_ms": 3.0,
    }],
    "plcc": 0.75,
    "srocc": 0.70,
    "rmse": 1.25,
    "runtime_total_ms": 3.0,
    "params": 123,
    "gflops": 0.01,
}))
""",
        encoding="utf-8",
    )
    fake.chmod(0o755)
    return fake


def _run_fork_shell_wrapper(tmp_path: pathlib.Path, competitor: str) -> dict:
    fake_vmaf_tune = _write_fake_vmaf_tune(tmp_path)
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    (model_dir / "nr_metric_v1.json").write_text("{}", encoding="utf-8")
    (model_dir / "fr_regressor_v2_ensemble_v1.json").write_text("{}", encoding="utf-8")

    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    ref.write_bytes(b"\x00" * 16)
    dis.write_bytes(b"\x00" * 16)
    out = tmp_path / f"{competitor}.json"

    cmd = [
        "bash",
        str(compare.WRAPPERS[competitor]),
        "--dis",
        str(dis),
        "--width",
        "16",
        "--height",
        "16",
        "--pixfmt",
        "yuv420p",
        "--fps",
        "24",
        "--out",
        str(out),
    ]
    if competitor == "fork-fr-regressor":
        cmd.extend(["--ref", str(ref)])

    env = os.environ.copy()
    env["EXTERNAL_BENCH_VMAF_TUNE"] = str(fake_vmaf_tune)
    env["EXTERNAL_BENCH_MODEL_DIR"] = str(model_dir)
    proc = subprocess.run(  # noqa: S603 - fixed argv runs repo wrapper with temp fixtures
        cmd,
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert proc.returncode == 0, proc.stderr
    return json.loads(out.read_text(encoding="utf-8"))


# --- tests -----------------------------------------------------------------


def test_run_wrapper_parses_canned_output(tmp_path: pathlib.Path) -> None:
    item = compare.CorpusItem(
        name="t/0",
        ref=tmp_path / "ref.yuv",
        dis=tmp_path / "dis.yuv",
        width=576,
        height=324,
    )
    out_path = tmp_path / "out.json"
    runner = _make_stub_runner({"x264-pvmaf": _canned_output("x264-pvmaf", n_frames=2)})

    result = compare.run_wrapper("x264-pvmaf", item, out_path, runner=runner)

    assert result["summary"]["competitor"] == "x264-pvmaf"
    assert len(result["frames"]) == EXPECTED_FRAMES_DEFAULT
    assert result["summary"]["plcc"] == EXPECTED_PLCC_CANNED


def test_fork_nr_shell_wrapper_emits_registry_competitor_key(tmp_path: pathlib.Path) -> None:
    payload = _run_fork_shell_wrapper(tmp_path, "fork-nr-metric")

    assert payload["summary"]["competitor"] == "fork-nr-metric"
    assert compare.validate_wrapper_output("fork-nr-metric", payload) is payload


def test_fork_fr_shell_wrapper_emits_registry_competitor_key(tmp_path: pathlib.Path) -> None:
    payload = _run_fork_shell_wrapper(tmp_path, "fork-fr-regressor")

    assert payload["summary"]["competitor"] == "fork-fr-regressor"
    assert compare.validate_wrapper_output("fork-fr-regressor", payload) is payload


def test_run_wrapper_propagates_failure(tmp_path: pathlib.Path) -> None:
    item = compare.CorpusItem(
        name="t/0",
        ref=None,
        dis=tmp_path / "dis.yuv",
        width=10,
        height=10,
    )

    def stub_run(cmd, *_a, **_kw):
        return subprocess.CompletedProcess(
            args=cmd,
            returncode=EXPECTED_FAILED_RC,
            stdout="",
            stderr="boom",
        )

    with pytest.raises(RuntimeError, match=f"rc={EXPECTED_FAILED_RC}"):
        compare.run_wrapper(
            "dover-mobile",
            item,
            tmp_path / "out.json",
            runner=stub_run,
        )


def test_validate_wrapper_output_rejects_missing_summary() -> None:
    payload = {"frames": []}
    with pytest.raises(ValueError, match="summary"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_wrong_competitor() -> None:
    payload = _canned_output("dover-mobile")
    with pytest.raises(ValueError, match=r"summary\.competitor"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_bad_frame_value() -> None:
    payload = _canned_output("x264-pvmaf")
    payload["frames"][0]["runtime_ms"] = "slow"
    with pytest.raises(ValueError, match=r"frames\[0\]\.runtime_ms"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_run_wrapper_rejects_invalid_json(tmp_path: pathlib.Path) -> None:
    item = compare.CorpusItem(
        name="t/0",
        ref=None,
        dis=tmp_path / "dis.yuv",
        width=10,
        height=10,
    )

    def stub_run(cmd, *_a, **_kw):
        out_idx = cmd.index("--out")
        pathlib.Path(cmd[out_idx + 1]).write_text("{not json", encoding="utf-8")
        return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

    with pytest.raises(RuntimeError, match="invalid JSON"):
        compare.run_wrapper("dover-mobile", item, tmp_path / "out.json", runner=stub_run)


def test_run_wrapper_rejects_invalid_schema(tmp_path: pathlib.Path) -> None:
    item = compare.CorpusItem(
        name="t/0",
        ref=None,
        dis=tmp_path / "dis.yuv",
        width=10,
        height=10,
    )

    def stub_run(cmd, *_a, **_kw):
        out_idx = cmd.index("--out")
        pathlib.Path(cmd[out_idx + 1]).write_text(
            json.dumps({"frames": [], "summary": {"competitor": "dover-mobile"}}),
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

    with pytest.raises(RuntimeError, match="invalid schema"):
        compare.run_wrapper("dover-mobile", item, tmp_path / "out.json", runner=stub_run)


def test_aggregate_computes_means() -> None:
    results = [
        {
            "summary": {
                "plcc": 0.9,
                "srocc": 0.8,
                "rmse": 1.0,
                "runtime_total_ms": 100,
                "params": 1,
                "gflops": 0.1,
            }
        },
        {
            "summary": {
                "plcc": 1.0,
                "srocc": 0.9,
                "rmse": 2.0,
                "runtime_total_ms": 200,
                "params": 1,
                "gflops": 0.1,
            }
        },
    ]
    agg = compare.aggregate("x", results)
    assert agg.n_clips == EXPECTED_FRAMES_DEFAULT
    assert agg.plcc_mean == pytest.approx(0.95)
    assert agg.srocc_mean == pytest.approx(0.85)
    assert agg.rmse_mean == pytest.approx(1.5)
    assert agg.runtime_total_ms == EXPECTED_RUNTIME_TOTAL_SUM


def test_render_table_includes_all_competitors() -> None:
    aggs = [
        compare.CompetitorAggregate(
            competitor="fork-fr-regressor",
            n_clips=3,
            plcc_mean=0.99,
            srocc_mean=0.98,
            rmse_mean=1.2,
            runtime_total_ms=12.5,
            params=1000,
            gflops=0.05,
        ),
        compare.CompetitorAggregate(
            competitor="x264-pvmaf",
            n_clips=3,
            plcc_mean=0.91,
            srocc_mean=0.89,
            rmse_mean=3.4,
            runtime_total_ms=200.0,
            params=5000,
            gflops=2.0,
        ),
    ]
    table = compare.render_table(aggs)
    assert "fork-fr-regressor" in table
    assert "x264-pvmaf" in table
    assert "PLCC" in table
    assert "0.9900" in table
    assert "0.9100" in table


def test_main_emits_table_with_stubbed_wrappers(
    tmp_path: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    # Build a fake corpus (two distorted variants of one source).
    src = tmp_path / "netflix" / "src01_576x324_24fps" / "ref"
    src.mkdir(parents=True)
    (src / "ref.yuv").write_bytes(b"\x00" * 16)
    dis = tmp_path / "netflix" / "src01_576x324_24fps" / "dis"
    dis.mkdir(parents=True)
    (dis / "dis_a.yuv").write_bytes(b"\x00" * 16)
    (dis / "dis_b.yuv").write_bytes(b"\x00" * 16)

    runner = _make_stub_runner({})
    # Inject the runner into compare.run_wrapper via monkeypatch on
    # subprocess.run, since main() calls run_wrapper without a custom
    # runner kwarg.
    monkeypatch.setattr(compare.subprocess, "run", runner)

    rc = compare.main(
        [
            "--bvi-dvc-root",
            str(tmp_path / "does-not-exist"),
            "--netflix-public-root",
            str(tmp_path / "netflix"),
            "--out-json",
            str(tmp_path / "agg.json"),
        ]
    )
    assert rc == 0

    captured = capsys.readouterr()
    # All four competitors should appear with n_clips=2.
    for c in ("fork-fr-regressor", "fork-nr-metric", "x264-pvmaf", "dover-mobile"):
        assert c in captured.out

    agg_data = json.loads((tmp_path / "agg.json").read_text())
    assert {a["competitor"] for a in agg_data} == set(compare.WRAPPERS.keys())
    for a in agg_data:
        assert a["n_clips"] == FIXTURE_DIS_COUNT


def test_main_errors_clearly_when_corpus_missing(
    tmp_path: pathlib.Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    rc = compare.main(
        [
            "--bvi-dvc-root",
            str(tmp_path / "absent-bvi"),
            "--netflix-public-root",
            str(tmp_path / "absent-nf"),
        ]
    )
    assert rc == EXPECTED_MISSING_CORPUS_RC
    err = capsys.readouterr().err
    assert "no corpus found" in err
    assert "BVI-DVC" in err
    assert "Netflix Public" in err


def test_corpus_discovery_pairs_netflix_drop(tmp_path: pathlib.Path) -> None:
    root = tmp_path / "netflix"
    src = root / "src01_576x324_24fps"
    (src / "ref").mkdir(parents=True)
    (src / "ref" / "ref.yuv").write_bytes(b"\x00")
    (src / "dis").mkdir(parents=True)
    (src / "dis" / "d1.yuv").write_bytes(b"\x00")
    (src / "dis" / "d2.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path / "no-bvi",
        netflix_root=root,
    )
    assert len(items) == FIXTURE_DIS_COUNT
    assert all(i.width == FIXTURE_WIDTH and i.height == FIXTURE_HEIGHT for i in items)
    assert all(i.ref is not None and i.ref.name == "ref.yuv" for i in items)


# --- coverage backfill: BVI-DVC discovery ----------------------------------


def test_bvi_dvc_discovers_ref_dis_pairs(tmp_path: pathlib.Path) -> None:
    """BVI-DVC test fold pairs every ``<stem>__dis*.yuv`` against the ref."""
    fold = tmp_path / "test"
    fold.mkdir()
    stem = "clip01_1920x1080_24fps"
    (fold / f"{stem}__ref.yuv").write_bytes(b"\x00")
    (fold / f"{stem}__dis1.yuv").write_bytes(b"\x00")
    (fold / f"{stem}__dis2.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path,
        netflix_root=tmp_path / "no-nf",
    )
    assert len(items) == FIXTURE_DIS_COUNT
    assert all(i.width == 1920 and i.height == 1080 for i in items)  # noqa: PLR2004
    assert all(i.name.startswith("bvi-dvc/") for i in items)
    assert {i.dis.name for i in items} == {f"{stem}__dis1.yuv", f"{stem}__dis2.yuv"}


def test_bvi_dvc_skips_ref_with_no_geometry(tmp_path: pathlib.Path) -> None:
    """Refs whose stem lacks a ``WxH`` token are silently skipped."""
    fold = tmp_path / "test"
    fold.mkdir()
    # No 'NxM' token anywhere in the stem.
    (fold / "no_geometry_here__ref.yuv").write_bytes(b"\x00")
    (fold / "no_geometry_here__dis1.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path,
        netflix_root=tmp_path / "no-nf",
    )
    assert items == []


# --- coverage backfill: Netflix discovery edge cases -----------------------


def test_netflix_skips_src_dir_missing_ref_or_dis(tmp_path: pathlib.Path) -> None:
    """src dirs without both ``ref/`` and ``dis/`` subtrees are skipped."""
    root = tmp_path / "netflix"
    # Has ref/ but no dis/.
    (root / "src01_576x324_24fps" / "ref").mkdir(parents=True)
    (root / "src01_576x324_24fps" / "ref" / "ref.yuv").write_bytes(b"\x00")
    # Has dis/ but no ref/.
    (root / "src02_576x324_24fps" / "dis").mkdir(parents=True)
    (root / "src02_576x324_24fps" / "dis" / "d.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path / "no-bvi",
        netflix_root=root,
    )
    assert items == []


def test_netflix_skips_src_dir_with_empty_ref_dir(tmp_path: pathlib.Path) -> None:
    """src dirs with an empty ``ref/`` directory are skipped."""
    root = tmp_path / "netflix"
    src = root / "src01_576x324_24fps"
    (src / "ref").mkdir(parents=True)  # no *.yuv inside
    (src / "dis").mkdir(parents=True)
    (src / "dis" / "d.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path / "no-bvi",
        netflix_root=root,
    )
    assert items == []


def test_netflix_skips_src_dir_without_geometry_token(tmp_path: pathlib.Path) -> None:
    """src dirs whose name lacks a ``WxH`` token are skipped."""
    root = tmp_path / "netflix"
    src = root / "no_geometry_token_here"
    (src / "ref").mkdir(parents=True)
    (src / "ref" / "r.yuv").write_bytes(b"\x00")
    (src / "dis").mkdir(parents=True)
    (src / "dis" / "d.yuv").write_bytes(b"\x00")

    items = compare.discover_corpus(
        bvi_dvc_root=tmp_path / "no-bvi",
        netflix_root=root,
    )
    assert items == []


# --- coverage backfill: validate_wrapper_output rejections -----------------


def test_validate_wrapper_output_rejects_non_dict_payload() -> None:
    with pytest.raises(ValueError, match="JSON object"):
        compare.validate_wrapper_output("x264-pvmaf", ["not", "a", "dict"])


def test_validate_wrapper_output_rejects_non_list_frames() -> None:
    payload = {"frames": "oops", "summary": {}}
    with pytest.raises(ValueError, match="frames must be a list"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_non_dict_frame() -> None:
    payload = {"frames": ["not-an-object"], "summary": {}}
    with pytest.raises(ValueError, match=r"frames\[0\] must be an object"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_non_integer_frame_idx() -> None:
    payload = _canned_output("x264-pvmaf")
    payload["frames"][0]["frame_idx"] = 1.5
    with pytest.raises(ValueError, match=r"frames\[0\]\.frame_idx"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_boolean_frame_idx() -> None:
    """``bool`` is an ``int`` subclass in Python; the validator rejects it
    explicitly so ``True``/``False`` cannot masquerade as a frame index."""
    payload = _canned_output("x264-pvmaf")
    payload["frames"][0]["frame_idx"] = True
    with pytest.raises(ValueError, match=r"frames\[0\]\.frame_idx"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


def test_validate_wrapper_output_rejects_boolean_summary_metric() -> None:
    """Same bool-as-number guard for the summary numeric fields."""
    payload = _canned_output("x264-pvmaf")
    payload["summary"]["plcc"] = True
    with pytest.raises(ValueError, match=r"summary\.plcc"):
        compare.validate_wrapper_output("x264-pvmaf", payload)


# --- coverage backfill: run_wrapper failure surfaces -----------------------


def test_run_wrapper_errors_when_output_file_missing(tmp_path: pathlib.Path) -> None:
    """A wrapper that returns rc=0 but never writes ``--out`` is rejected."""
    item = compare.CorpusItem(
        name="t/0",
        ref=None,
        dis=tmp_path / "dis.yuv",
        width=10,
        height=10,
    )

    def stub_run(cmd, *_a, **_kw):
        # Deliberately do NOT create the --out file.
        return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

    with pytest.raises(RuntimeError, match="did not produce"):
        compare.run_wrapper(
            "dover-mobile",
            item,
            tmp_path / "missing-out.json",
            runner=stub_run,
        )


# --- coverage backfill: main() limit + per-item skip path ------------------


def test_main_honours_limit_flag(
    tmp_path: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """``--limit N`` truncates the corpus before dispatching wrappers."""
    src = tmp_path / "netflix" / "src01_576x324_24fps"
    (src / "ref").mkdir(parents=True)
    (src / "ref" / "ref.yuv").write_bytes(b"\x00")
    (src / "dis").mkdir(parents=True)
    for n in range(5):
        (src / "dis" / f"d{n}.yuv").write_bytes(b"\x00")

    runner = _make_stub_runner({})
    monkeypatch.setattr(compare.subprocess, "run", runner)

    rc = compare.main(
        [
            "--bvi-dvc-root",
            str(tmp_path / "no-bvi"),
            "--netflix-public-root",
            str(tmp_path / "netflix"),
            "--competitors",
            "x264-pvmaf",
            "--limit",
            "2",
            "--out-json",
            str(tmp_path / "agg.json"),
        ]
    )
    assert rc == 0
    capsys.readouterr()  # discard table
    agg = json.loads((tmp_path / "agg.json").read_text())
    assert len(agg) == 1
    assert agg[0]["competitor"] == "x264-pvmaf"
    assert agg[0]["n_clips"] == FIXTURE_DIS_COUNT  # 2 after --limit


def test_main_skips_failing_wrapper_and_continues(
    tmp_path: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """A wrapper that errors for a corpus item logs ``skip`` and the run
    still completes for the remaining wrappers / items."""
    src = tmp_path / "netflix" / "src01_576x324_24fps"
    (src / "ref").mkdir(parents=True)
    (src / "ref" / "ref.yuv").write_bytes(b"\x00")
    (src / "dis").mkdir(parents=True)
    (src / "dis" / "d.yuv").write_bytes(b"\x00")

    # x264-pvmaf intentionally fails (rc!=0); dover-mobile succeeds.
    def runner(cmd, *_a, **_kw):
        wrapper = cmd[1]
        out_idx = cmd.index("--out")
        out_path = pathlib.Path(cmd[out_idx + 1])
        if "x264-pvmaf" in wrapper:
            return subprocess.CompletedProcess(args=cmd, returncode=2, stdout="", stderr="boom")
        out_path.write_text(json.dumps(_canned_output("dover-mobile")))
        return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

    monkeypatch.setattr(compare.subprocess, "run", runner)

    rc = compare.main(
        [
            "--bvi-dvc-root",
            str(tmp_path / "no-bvi"),
            "--netflix-public-root",
            str(tmp_path / "netflix"),
            "--competitors",
            "x264-pvmaf",
            "dover-mobile",
        ]
    )
    assert rc == 0

    captured = capsys.readouterr()
    assert "skip x264-pvmaf" in captured.err
    # Aggregated table still printed; dover-mobile produced data, x264-pvmaf
    # is present with n_clips=0.
    assert "x264-pvmaf" in captured.out
    assert "dover-mobile" in captured.out

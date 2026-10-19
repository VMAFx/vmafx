#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Mini retrain: the one-shot retrain's tooling, end to end, on a generated corpus.

The real retrain (``docs/ai/retrain-runbook-1246.md``) must not be the first
time its tools run together. This driver runs the same scripts, with the same
flags, on a corpus of twelve generated clip pairs (:mod:`aiutils.mini_corpus`):

    fixture -> extract -> verify_features -> combine -> registry_init
    -> train / export / validate for vmaf_tiny_v2, v3, v4
    -> train + export fr_regressor_v1 (leave-one-source-out, registry upsert)
    -> registry_validate -> gate (PLCC / SROCC / RMSE per model)

Each stage is a :class:`aiutils.pipeline.Stage`: it writes a manifest, a killed
run resumes from the manifests, and a missing or corrupt input fails with the
stage name before the stage runs. The thresholds of the ``mini`` profile prove
the plumbing on 144 rows; the ``full`` profile holds the runbook's section 8.1
gates for the real run.

Usage::

    python ai/scripts/mini_retrain.py run --run-dir runs/mini --vmaf-bin core/build/tools/vmaf
"""

from __future__ import annotations

import argparse
import importlib
import json
import shutil
import sys
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _PATHS.script_path
REPO_ROOT = _PATHS.repo_root
SCRIPTS = REPO_ROOT / "ai" / "scripts"

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import write_text_atomic  # noqa: E402
from aiutils.mini_corpus import (  # noqa: E402
    CLIP_FRAMES,
    HEIGHT,
    SOURCES,
    WIDTH,
    generate_mini_corpus,
)
from aiutils.pipeline import (  # noqa: E402
    Stage,
    StageContext,
    StageError,
    run_pipeline,
)
from aiutils.retrain_checks import (  # noqa: E402
    DataCheckError,
    Thresholds,
    gate_verdict,
    metrics,
    require_finite_columns,
)

TEACHER = "vmaf_v1.0.16_3d0h"
CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)
PAIRS_PER_SOURCE = 3
LABEL_SCALE_FLOOR = 50.0
EXPECT_ROWS = len(SOURCES) * PAIRS_PER_SOURCE * CLIP_FRAMES
LOCKS = (REPO_ROOT / "ai" / "requirements-dev-lock.txt",)
REF_YUV = REPO_ROOT / "testdata" / "ref_576x324_48f.yuv"
DIS_YUV = REPO_ROOT / "testdata" / "dis_576x324_48f.yuv"
TINY_FAMILIES = ("v2", "v3", "v4")

# Gates per profile. ``mini`` proves the plumbing on a corpus small enough for
# CI; it does not claim the models are good. ``full`` is the runbook section
# 8.1 gate set; the runbook states no RMSE bound for the tiny and FR models, so
# none is applied there (the number is still recorded in the report).
PROFILES: dict[str, dict[str, Any]] = {
    "mini": {
        "epochs": 200,
        "fr_epochs": 60,
        "batch_size": 16,
        "lr": 0.01,
        "gates": {
            "vmaf_tiny_v2": Thresholds(min_plcc=0.50, min_srocc=0.50, max_rmse=13.0),
            "vmaf_tiny_v3": Thresholds(min_plcc=0.50, min_srocc=0.50, max_rmse=13.0),
            "vmaf_tiny_v4": Thresholds(min_plcc=0.50, min_srocc=0.50, max_rmse=13.0),
            "fr_regressor_v1": Thresholds(min_plcc=0.50, min_srocc=0.50, max_rmse=13.0),
        },
        "loso_min_plcc": 0.0,
    },
    "full": {
        "epochs": 90,
        "fr_epochs": 100,
        "batch_size": 256,
        "lr": 0.001,
        "gates": {
            "vmaf_tiny_v2": Thresholds(min_plcc=0.990),
            "vmaf_tiny_v3": Thresholds(min_plcc=0.990),
            "vmaf_tiny_v4": Thresholds(min_plcc=0.990),
            "fr_regressor_v1": Thresholds(min_plcc=0.95),
        },
        "loso_min_plcc": 0.95,
    },
}


# ---------------------------------------------------------------- stage bodies


def _pandas() -> Any:
    """pandas as ``Any``: its stubs are not installed everywhere the type check runs."""
    return importlib.import_module("pandas")


def _cmd_fixture(args: argparse.Namespace) -> int:
    corpus = generate_mini_corpus(args.out, ref_yuv=REF_YUV, dis_yuv=DIS_YUV, seed=args.seed)
    print(f"[mini-retrain] fixture: {corpus.pairs} pairs, digest {corpus.sha256}")
    return 0


def verify_feature_table(parquet: Path, *, teacher: str, expect_rows: int | None) -> dict[str, Any]:
    """Check an extracted feature table; raise :class:`DataCheckError` naming the defect."""
    from ai.data.feature_extractor import FULL_FEATURES

    df = _pandas().read_parquet(parquet)
    base = ["source", "frame_index", "teacher_model", "vmaf"]
    missing = [c for c in (*base, *FULL_FEATURES) if c not in df.columns]
    if missing:
        raise DataCheckError(f"feature table {parquet.name}: missing column(s) {missing}")
    teachers = sorted(str(t) for t in df["teacher_model"].dropna().unique())
    if teachers != [teacher]:
        raise DataCheckError(
            f"feature table {parquet.name}: teacher_model {teachers}, want {teacher}"
        )
    if expect_rows is not None and len(df) != expect_rows:
        raise DataCheckError(f"feature table {parquet.name}: {len(df)} rows, want {expect_rows}")
    require_finite_columns(df, [*CANONICAL_6, "vmaf"], what=f"feature table {parquet.name}")
    dead = [c for c in FULL_FEATURES if df[c].isna().all()]
    if dead:
        raise DataCheckError(f"feature table {parquet.name}: column(s) {dead} are NaN in every row")
    if not df["vmaf"].between(0.0, 100.0).all():
        raise DataCheckError(f"feature table {parquet.name}: vmaf outside [0, 100]")
    if float(df["vmaf"].max()) <= LABEL_SCALE_FLOOR:
        raise DataCheckError(
            f"feature table {parquet.name}: largest vmaf is {float(df['vmaf'].max()):.4f}; "
            "the teacher scores on 0-100, this looks like a 0-1 scale"
        )
    return {"rows": len(df), "sources": int(df["source"].nunique()), "teacher_model": teacher}


def _cmd_verify(args: argparse.Namespace) -> int:
    try:
        report = verify_feature_table(args.parquet, teacher=TEACHER, expect_rows=args.expect_rows)
    except DataCheckError as exc:
        print(f"[mini-retrain] verify_features: {exc}", file=sys.stderr)
        return 1
    write_text_atomic(args.out, json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"[mini-retrain] verify_features ok: {report}")
    return 0


def _cmd_registry_init(args: argparse.Namespace) -> int:
    source = json.loads((REPO_ROOT / "model" / "tiny" / "registry.json").read_text())
    empty = {k: v for k, v in source.items() if k != "models"}
    empty["models"] = []
    write_text_atomic(args.out, json.dumps(empty, indent=2, sort_keys=True) + "\n")
    shutil.copyfile(REPO_ROOT / "model" / "tiny" / "registry.schema.json", args.schema_out)
    return 0


# ----------------------------------------------------------------------- gate


def _predict_tiny(onnx_path: Path, x: Any) -> Any:
    import numpy as np
    import onnxruntime as ort

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name
    return np.asarray(sess.run(None, {name: x.astype(np.float32)})[0]).reshape(-1)


def _predict_fr(onnx_path: Path, sidecar: Path, x: Any) -> Any:
    meta = json.loads(sidecar.read_text())
    mean = _np(meta["feature_mean"])
    std = _np(meta["feature_std"])
    return _predict_tiny(onnx_path, (x - mean) / std)


def _np(values: Any) -> Any:
    import numpy as np

    return np.asarray(values, dtype=np.float64)


def _gate_rows(run: Path, profile: dict[str, Any]) -> list[dict[str, Any]]:
    df = _pandas().read_parquet(run / "features" / "combined.parquet")
    x = df[list(CANONICAL_6)].to_numpy(dtype="float64")
    y = df["vmaf"].to_numpy(dtype="float64")
    rows: list[dict[str, Any]] = []
    models = run / "models"
    for fam in TINY_FAMILIES:
        mid = f"vmaf_tiny_{fam}"
        pred = _predict_tiny(models / f"{mid}.onnx", x)
        rows.append(_gate_row(mid, pred, y, profile["gates"][mid]))
    pred = _predict_fr(models / "fr_regressor_v1.onnx", models / "fr_regressor_v1.json", x)
    row = _gate_row("fr_regressor_v1", pred, y, profile["gates"]["fr_regressor_v1"])
    summary = json.loads((run / "reports" / "fr_regressor_v1_metrics.json").read_text())["loso"][
        "summary"
    ]
    row["loso_mean_plcc"] = summary["mean_plcc"]
    row["loso_folds"] = summary["n_folds"]
    row["failures"] += gate_verdict(
        {"plcc": summary["mean_plcc"], "srocc": 1.0, "rmse": 0.0},
        Thresholds(min_plcc=profile["loso_min_plcc"]),
    )
    row["verdict"] = "fail" if row["failures"] else "pass"
    rows.append(row)
    return rows


def _gate_row(model: str, pred: Any, truth: Any, gate: Thresholds) -> dict[str, Any]:
    measured = metrics(pred, truth)
    failures = gate_verdict(measured, gate)
    return {
        "model": model,
        "scope": "in-sample, every row of the combined table",
        "rows": len(truth),
        **{k: (v if v == v else None) for k, v in measured.items()},
        "gate": {"min_plcc": gate.min_plcc, "min_srocc": gate.min_srocc, "max_rmse": gate.max_rmse},
        "failures": failures,
        "verdict": "fail" if failures else "pass",
    }


def _cmd_gate(args: argparse.Namespace) -> int:
    profile = PROFILES[args.profile]
    rows = _gate_rows(args.run_dir, profile)
    report = {
        "schema": "retrain-gate-report-v1",
        "profile": args.profile,
        "teacher_model": TEACHER,
        "models": rows,
        "passed": all(r["verdict"] == "pass" for r in rows),
    }
    write_text_atomic(args.out, json.dumps(report, indent=2, sort_keys=True) + "\n")
    for r in rows:
        print(f"[mini-retrain] gate {r['model']}: {r['verdict']} {r['failures']}")
    return 0 if report["passed"] else 1


# --------------------------------------------------------------- stage listing


def _py(script: str, *rest: str) -> tuple[str, ...]:
    return (sys.executable, str(SCRIPTS / script), *rest)


def _self(*rest: str) -> tuple[str, ...]:
    return (sys.executable, str(SCRIPT_PATH), *rest)


def _rows_work(parquet: Path):  # type: ignore[no-untyped-def]
    def work(_ctx: StageContext) -> dict[str, int]:
        return {"rows": len(_pandas().read_parquet(parquet))}

    return work


def _tiny_stages(run: Path, fam: str, prof: dict[str, Any], seed: int) -> list[Stage]:
    comb = run / "features" / "combined.parquet"
    ckpt, stats = run / "ckpt" / f"vmaf_tiny_{fam}.pt", run / "ckpt" / f"vmaf_tiny_{fam}_stats.json"
    onnx, side = run / "models" / f"vmaf_tiny_{fam}.onnx", run / "models" / f"vmaf_tiny_{fam}.json"
    return [
        Stage(
            f"train_tiny_{fam}",
            _py(
                f"train_vmaf_tiny_{fam}.py",
                *("--parquet", str(comb), "--out-ckpt", str(ckpt), "--out-stats", str(stats)),
                *("--epochs", str(prof["epochs"]), "--seed", str(seed)),
                *("--batch-size", str(prof["batch_size"]), "--lr", str(prof["lr"])),
            ),
            inputs=(comb,),
            outputs=(ckpt, stats),
            stable=(ckpt,),
            seed=seed,
            code=(SCRIPTS / f"train_vmaf_tiny_{fam}.py",),
            work=_rows_work(comb),
        ),
        Stage(
            f"export_tiny_{fam}",
            _py(
                f"export_vmaf_tiny_{fam}.py",
                *("--ckpt", str(ckpt), "--out-onnx", str(onnx), "--out-sidecar", str(side)),
            ),
            inputs=(ckpt,),
            outputs=(onnx, side),
            stable=(onnx,),
            code=(SCRIPTS / f"export_vmaf_tiny_{fam}.py",),
        ),
        Stage(
            f"validate_tiny_{fam}",
            _py(
                f"validate_vmaf_tiny_{fam}.py",
                *("--onnx", str(onnx), "--parquet", str(comb), "--rows", str(EXPECT_ROWS)),
                *(
                    "--min-plcc",
                    str(prof["gates"][f"vmaf_tiny_{fam}"].min_plcc),
                    "--out-json",
                    str(run / "reports" / f"validate_{fam}.json"),
                ),
            ),
            inputs=(onnx, comb),
            outputs=(run / "reports" / f"validate_{fam}.json",),
            code=(SCRIPTS / f"validate_vmaf_tiny_{fam}.py",),
        ),
    ]


def _data_stages(run: Path, *, vmaf_bin: Path, seed: int) -> list[Stage]:
    """Fixture, extraction and the feature checks."""
    corpus, feats = run / "corpus", run / "features"
    nf = feats / "netflix.parquet"
    return [
        Stage(
            "fixture",
            _self("_fixture", "--out", str(corpus), "--seed", str(seed)),
            inputs=(REF_YUV, DIS_YUV),
            outputs=(corpus,),
            seed=seed,
            stable=(corpus,),
            code=(SCRIPT_PATH, REPO_ROOT / "ai/src/aiutils/mini_corpus.py"),
        ),
        Stage(
            "extract",
            _py(
                "extract_full_features.py",
                *("--data-root", str(corpus), "--vmaf-bin", str(vmaf_bin)),
                *("--cache-dir", str(run / "cache"), "--out", str(nf)),
                *("--manifest-out", str(feats / "netflix.manifest.json")),
                *("--assume-dims", f"{WIDTH}x{HEIGHT}", "--vmaf-model", f"version={TEACHER}"),
            ),
            inputs=(corpus, vmaf_bin),
            outputs=(nf, feats / "netflix.manifest.json"),
            stable=(nf,),
            code=(SCRIPTS / "extract_full_features.py",),
            work=_rows_work(nf),
        ),
        Stage(
            "verify_features",
            _self(
                "_verify",
                "--parquet",
                str(nf),
                "--out",
                str(run / "reports/verify.json"),
                "--expect-rows",
                str(EXPECT_ROWS),
            ),
            inputs=(nf,),
            outputs=(run / "reports" / "verify.json",),
            stable=(run / "reports" / "verify.json",),
            code=(SCRIPT_PATH,),
        ),
    ]


def _combine_stages(run: Path) -> list[Stage]:
    """Combination of the shards and the registry seed the later stages update."""
    feats = run / "features"
    nf, comb = feats / "netflix.parquet", feats / "combined.parquet"
    reg_seed = run / "models" / "registry.seed.json"
    schema = run / "models" / "registry.schema.json"
    return [
        Stage(
            "combine",
            _py(
                "combine_full_feature_parquets.py",
                *("--input", f"netflix={nf}", "--out", str(comb)),
                *("--manifest-out", str(feats / "combined.manifest.json")),
            ),
            inputs=(nf, run / "reports" / "verify.json"),
            outputs=(comb, feats / "combined.manifest.json"),
            stable=(comb,),
            code=(SCRIPTS / "combine_full_feature_parquets.py",),
            work=_rows_work(comb),
        ),
        Stage(
            "registry_init",
            _self("_registry_init", "--out", str(reg_seed), "--schema-out", str(schema)),
            outputs=(reg_seed, schema),
            stable=(reg_seed, schema),
            code=(SCRIPT_PATH,),
        ),
    ]


def build_stages(run: Path, *, vmaf_bin: Path, profile: str, seed: int) -> list[Stage]:
    """The retrain pipeline for ``run``; every path lives under it."""
    prof = PROFILES[profile]
    comb = run / "features" / "combined.parquet"
    models = run / "models"
    stages = _data_stages(run, vmaf_bin=vmaf_bin, seed=seed) + _combine_stages(run)
    for fam in TINY_FAMILIES:
        stages += _tiny_stages(run, fam, prof, seed)
    regs = (models / "registry.seed.json", models / "registry.json")
    stages.append(_fr_stage(run, comb, regs, prof, seed))
    stages += _gate_stages(run, comb, regs[1], models / "registry.schema.json", profile)
    return stages


def _fr_stage(
    run: Path, comb: Path, regs: tuple[Path, Path], prof: dict[str, Any], seed: int
) -> Stage:
    reg_seed, reg = regs
    onnx, side = run / "models" / "fr_regressor_v1.onnx", run / "models" / "fr_regressor_v1.json"
    metrics_json = run / "reports" / "fr_regressor_v1_metrics.json"
    return Stage(
        "train_fr_regressor_v1",
        _py(
            "train_fr_regressor.py",
            *("--parquet", str(comb), "--features", "canonical6"),
            *("--epochs", str(prof["fr_epochs"]), "--seed", str(seed)),
            *("--batch-size", str(prof["batch_size"]), "--lr", str(prof["lr"])),
            *("--ship-threshold", str(prof["loso_min_plcc"])),
            *("--out-onnx", str(onnx), "--out-sidecar", str(side)),
            *("--registry", str(reg), "--metrics-out", str(metrics_json)),
        ),
        inputs=(comb, reg_seed),
        outputs=(onnx, side, metrics_json, reg),
        copies=((reg_seed, reg),),
        stable=(onnx, reg),
        seed=seed,
        code=(SCRIPTS / "train_fr_regressor.py",),
        work=_rows_work(comb),
    )


def _gate_stages(run: Path, comb: Path, reg: Path, schema: Path, profile: str) -> list[Stage]:
    onnx, side = run / "models" / "fr_regressor_v1.onnx", run / "models" / "fr_regressor_v1.json"
    metrics_json = run / "reports" / "fr_regressor_v1_metrics.json"
    gate = run / "reports" / "gate_report.json"
    return [
        Stage(
            "registry_validate",
            _py(
                "validate_model_registry.py",
                str(reg),
                "--schema",
                str(schema),
                "--out-json",
                str(run / "reports" / "registry_validate.json"),
            ),
            inputs=(reg, schema),
            outputs=(run / "reports" / "registry_validate.json",),
            code=(SCRIPTS / "validate_model_registry.py",),
        ),
        Stage(
            "gate",
            _self("_gate", "--run-dir", str(run), "--out", str(gate), "--profile", profile),
            inputs=(
                comb,
                onnx,
                side,
                metrics_json,
                *(run / "models" / f"vmaf_tiny_{f}.onnx" for f in TINY_FAMILIES),
            ),
            outputs=(gate,),
            stable=(gate,),
            code=(SCRIPT_PATH, REPO_ROOT / "ai/src/aiutils/retrain_checks.py"),
        ),
    ]


# ------------------------------------------------------------------------ CLI


def _build_parser() -> argparse.ArgumentParser:
    ap = make_argument_parser(prog="mini_retrain.py", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run", help="run (or resume) the mini retrain")
    run.add_argument("--run-dir", type=Path, required=True)
    run.add_argument("--vmaf-bin", type=Path, required=True)
    run.add_argument("--profile", choices=sorted(PROFILES), default="mini")
    run.add_argument("--seed", type=int, default=0)
    fx = sub.add_parser("_fixture")
    fx.add_argument("--out", type=Path, required=True)
    fx.add_argument("--seed", type=int, default=0)
    vf = sub.add_parser("_verify")
    vf.add_argument("--parquet", type=Path, required=True)
    vf.add_argument("--out", type=Path, required=True)
    vf.add_argument("--expect-rows", type=int, default=None)
    ri = sub.add_parser("_registry_init")
    ri.add_argument("--out", type=Path, required=True)
    ri.add_argument("--schema-out", type=Path, required=True)
    gt = sub.add_parser("_gate")
    gt.add_argument("--run-dir", type=Path, required=True)
    gt.add_argument("--out", type=Path, required=True)
    gt.add_argument("--profile", choices=sorted(PROFILES), default="mini")
    return ap


def _cmd_run(args: argparse.Namespace) -> int:
    stages = build_stages(
        args.run_dir.resolve(),
        vmaf_bin=args.vmaf_bin.resolve(),
        profile=args.profile,
        seed=args.seed,
    )
    try:
        results = run_pipeline(
            stages, args.run_dir.resolve(), repo_root=REPO_ROOT, lock_files=LOCKS
        )
    except StageError as exc:
        print(f"[mini-retrain] FAILED {exc}", file=sys.stderr)
        return 1
    print(f"[mini-retrain] ok: {[(r.name, r.status) for r in results]}")
    return 0


_DISPATCH = {
    "run": _cmd_run,
    "_fixture": _cmd_fixture,
    "_verify": _cmd_verify,
    "_registry_init": _cmd_registry_init,
    "_gate": _cmd_gate,
}


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(collect_cli_argv(argv))
    return _DISPATCH[args.cmd](args)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

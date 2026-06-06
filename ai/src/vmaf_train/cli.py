# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""`vmaf-train` command-line entry point.

Subcommands:
  extract-features   run libvmaf on a dataset and dump features → parquet
  fit                train a model from a YAML config
  export             export a trained checkpoint to ONNX (with roundtrip)
  eval               evaluate an ONNX model on a feature parquet (PLCC/SROCC/RMSE)
  register           write the sidecar metadata JSON for a shipped model
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import typer
from rich.console import Console

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[3]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

app = typer.Typer(add_completion=False, no_args_is_help=True)
console = Console()
DEFAULT_VMAF_BINARY = Path("core") / "build-cpu" / "tools" / "vmaf"


def _write_cli_report_json(
    path: Path,
    payload: dict[str, Any],
    *,
    args: dict[str, Any],
    inputs: dict[str, Any],
    outputs: dict[str, Any] | None = None,
) -> None:
    """Write a vmaf-train JSON report with ADR-0661 run provenance."""
    report_outputs = {"json_report": str(path)}
    if outputs:
        report_outputs.update(outputs)
    payload["run_provenance"] = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=sys.argv[1:],
        args=args,
        inputs=inputs,
        outputs=report_outputs,
    )
    write_manifest_json(path, payload)


@app.command("extract-features")
def extract_features_cmd(
    dataset: str = typer.Option(..., help="Dataset name (nflx, konvid-1k, ...)"),
    output: Path = typer.Option(..., help="Output parquet path"),
    vmaf_binary: Path = typer.Option(DEFAULT_VMAF_BINARY, help="Path to the vmaf CLI binary"),
) -> None:
    """Dump per-frame libvmaf features → parquet for C1 training."""
    from .data.datasets import load_manifest
    from .data.feature_dump import DEFAULT_FEATURES, Entry, dump_features

    manifest = load_manifest(dataset)
    if not manifest:
        console.print(f"[yellow]Manifest for '{dataset}' is empty — nothing to extract.[/yellow]")
        raise typer.Exit(code=2)

    entries = [
        Entry(
            key=m.key,
            ref=Path(m.path),
            dis=Path(m.path),
            width=0,
            height=0,
            mos=m.mos,
        )
        for m in manifest
    ]
    out = dump_features(entries, output, vmaf_binary=vmaf_binary, features=DEFAULT_FEATURES)
    console.print(f"[green]Wrote features to {out}[/green]")


@app.command("fit")
def fit_cmd(
    config: Path = typer.Option(..., exists=True, dir_okay=False, help="YAML config path"),
    cache: Path | None = typer.Option(None, help="Override features cache (.parquet / .npz)"),
    output: Path | None = typer.Option(None, help="Override output run directory"),
    epochs: int | None = typer.Option(None),
    seed: int | None = typer.Option(None),
) -> None:
    """Train a model from a YAML config."""
    from .train import load_config, train

    overrides: dict[str, object] = {}
    if cache is not None:
        overrides["cache"] = str(cache)
    if output is not None:
        overrides["output"] = str(output)
    if epochs is not None:
        overrides["epochs"] = epochs
    if seed is not None:
        overrides["seed"] = seed

    cfg = load_config(config, overrides=overrides)
    ckpt = train(cfg)
    console.print(f"[green]Training done. Last checkpoint: {ckpt}[/green]")


def _parse_tune_param_specs(specs: list[str]) -> list[tuple[str, str, tuple[str, ...]]]:
    """Parse ``--param name=kind:...`` sweep specs into validated tuples."""
    parsed: list[tuple[str, str, tuple[str, ...]]] = []
    for spec in specs:
        if "=" not in spec:
            raise typer.BadParameter(
                f"{spec!r} must use name=kind:... syntax, e.g. hidden=choice:16,32"
            )
        name, body = spec.split("=", 1)
        name = name.strip()
        if not name:
            raise typer.BadParameter(f"{spec!r} has an empty parameter name")
        parts = tuple(part.strip() for part in body.split(":"))
        kind = parts[0] if parts else ""
        if kind == "float":
            if len(parts) not in (3, 4):
                raise typer.BadParameter(f"{spec!r} must be name=float:LOW:HIGH[:log]")
            try:
                float(parts[1])
                float(parts[2])
            except ValueError as exc:
                raise typer.BadParameter(f"{spec!r} has non-numeric float bounds") from exc
            if len(parts) == 4 and parts[3] != "log":
                raise typer.BadParameter(f"{spec!r} only supports the optional ':log' suffix")
        elif kind == "int":
            if len(parts) != 3:
                raise typer.BadParameter(f"{spec!r} must be name=int:LOW:HIGH")
            try:
                int(parts[1])
                int(parts[2])
            except ValueError as exc:
                raise typer.BadParameter(f"{spec!r} has non-integer bounds") from exc
        elif kind == "choice":
            if len(parts) != 2 or not parts[1]:
                raise typer.BadParameter(f"{spec!r} must be name=choice:A,B,...")
            if not [item for item in parts[1].split(",") if item]:
                raise typer.BadParameter(f"{spec!r} must include at least one choice")
        else:
            raise typer.BadParameter(
                f"{spec!r} has unknown kind {kind!r}; use float, int, or choice"
            )
        parsed.append((name, kind, parts[1:]))
    return parsed


def _coerce_choice(value: str) -> object:
    for caster in (int, float):
        try:
            return caster(value)
        except ValueError:
            pass
    if value.lower() == "true":
        return True
    if value.lower() == "false":
        return False
    return value


def _make_tune_suggest(
    specs: list[tuple[str, str, tuple[str, ...]]],
) -> Callable[[Any], dict[str, object]]:
    def suggest(trial: Any) -> dict[str, object]:
        overrides: dict[str, object] = {}
        for name, kind, args in specs:
            if kind == "float":
                low = float(args[0])
                high = float(args[1])
                log = len(args) == 3 and args[2] == "log"
                overrides[name] = trial.suggest_float(name, low, high, log=log)
            elif kind == "int":
                overrides[name] = trial.suggest_int(name, int(args[0]), int(args[1]))
            elif kind == "choice":
                choices = [_coerce_choice(item) for item in args[0].split(",") if item]
                overrides[name] = trial.suggest_categorical(name, choices)
        return overrides

    return suggest


@app.command("tune")
def tune_cmd(
    config: Path = typer.Option(..., exists=True, dir_okay=False, help="Base YAML config path"),
    param: list[str] | None = typer.Option(
        None,
        "--param",
        "-p",
        help=(
            "Model-arg search spec. Repeatable. Forms: "
            "name=float:LOW:HIGH[:log], name=int:LOW:HIGH, name=choice:A,B"
        ),
    ),
    trials: int = typer.Option(20, "--trials", min=1, help="Number of Optuna trials"),
    study_name: str = typer.Option("vmaf-train-sweep", help="Optuna study name"),
    storage: str | None = typer.Option(None, help="Optional Optuna storage URL"),
    cache: Path | None = typer.Option(None, help="Override features cache (.parquet / .npz)"),
    output: Path | None = typer.Option(None, help="Override sweep output root"),
    epochs: int | None = typer.Option(None, help="Override epochs per trial"),
    seed: int | None = typer.Option(None, help="Override base seed"),
) -> None:
    """Run an Optuna sweep over model_args from a base YAML config."""
    from .train import load_config
    from .tune import sweep

    specs = _parse_tune_param_specs(list(param or []))
    if not specs:
        raise typer.BadParameter("at least one --param search spec is required")

    overrides: dict[str, object] = {}
    if cache is not None:
        overrides["cache"] = str(cache)
    if output is not None:
        overrides["output"] = str(output)
    if epochs is not None:
        overrides["epochs"] = epochs
    if seed is not None:
        overrides["seed"] = seed

    cfg = load_config(config, overrides=overrides)
    try:
        study = sweep(
            cfg,
            _make_tune_suggest(specs),
            n_trials=trials,
            study_name=study_name,
            storage=storage,
        )
    except ImportError as exc:
        console.print("[red]vmaf-train tune requires the optional ai[tune] extra[/red]")
        console.print("[yellow]Install with: pip install -e 'ai[tune]'[/yellow]")
        raise typer.Exit(code=2) from exc

    console.print(f"[green]Sweep done. Best value: {study.best_value:g}[/green]")
    console.print(f"[cyan]Best params: {study.best_params}[/cyan]")


@app.command("export")
def export_cmd(
    checkpoint: Path = typer.Option(..., exists=True, help="Lightning checkpoint"),
    output: Path = typer.Option(..., help="Output .onnx path"),
    model: str = typer.Option(
        "fr_regressor", help="Model family: fr_regressor|nr_metric|learned_filter"
    ),
    opset: int = typer.Option(17),
    atol: float = typer.Option(1e-5, help="Roundtrip tolerance (torch vs onnxruntime)"),
) -> None:
    """Export a trained checkpoint to ONNX with a roundtrip validation step."""
    from .models import export_to_onnx
    from .train import MODEL_REGISTRY

    if model not in MODEL_REGISTRY:
        console.print(f"[red]unknown model family: {model}[/red]")
        raise typer.Exit(code=2)
    model_cls = MODEL_REGISTRY[model]
    loaded = model_cls.load_from_checkpoint(str(checkpoint)).eval()
    input_name = "features" if model == "fr_regressor" else "input"
    output_name = "score" if model in ("fr_regressor", "nr_metric") else "output"
    export_to_onnx(
        loaded,
        output,
        input_name=input_name,
        output_name=output_name,
        opset=opset,
        atol=atol,
    )
    console.print(f"[green]Wrote {output} (opset={opset}, roundtrip atol={atol:g})[/green]")


@app.command("eval")
def eval_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model"),
    features: Path = typer.Option(..., exists=True, help="Feature parquet"),
    split: str = typer.Option("test", help="train|val|test"),
    input_name: str = typer.Option("features"),
) -> None:
    """Evaluate an ONNX model on a held-out split."""
    import pandas as pd

    from .data.splits import split_keys
    from .datamodule import FEATURE_COLUMNS
    from .eval import evaluate_onnx

    df = pd.read_parquet(features)
    if "key" in df.columns:
        keys = sorted(df["key"].astype(str).unique())
        splits = split_keys(keys)
        chosen = getattr(splits, split)
        df = df[df["key"].isin(set(chosen))]
    cols = [c for c in FEATURE_COLUMNS if c in df.columns]
    x = df[cols].to_numpy()
    y = df["mos"].to_numpy()
    report = evaluate_onnx(model, x, y, input_name=input_name)
    console.print(f"[cyan]{report.pretty()}[/cyan]")


@app.command("manifest-scan")
def manifest_scan_cmd(
    dataset: str = typer.Option(..., help="Dataset name (nflx, konvid-1k, ...)"),
    root: Path = typer.Option(..., exists=True, file_okay=False, help="Local dataset root"),
    mos_csv: Path | None = typer.Option(
        None,
        "--mos-csv",
        exists=True,
        dir_okay=False,
        help="Optional CSV with columns key,mos",
    ),
) -> None:
    """Populate a dataset manifest from a local cache.

    Scans @p root for YUV/Y4M/MP4/MKV/WebM files, pins each by SHA-256, and
    joins an optional MOS CSV (columns: key,mos). Overwrites the in-tree
    manifest file for @p dataset.
    """
    from .data.manifest_scan import scan, write_manifest

    entries = scan(dataset, root, mos_csv)
    if not entries:
        console.print(f"[yellow]No video files found under {root}[/yellow]")
        raise typer.Exit(code=2)
    dst = write_manifest(dataset, entries)
    with_mos = sum(1 for e in entries if e.mos is not None)
    console.print(
        f"[green]Wrote {dst} with {len(entries)} entries " f"({with_mos} with MOS)[/green]"
    )


@app.command("validate-norm")
def validate_norm_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model or its .json sidecar"),
    features: Path = typer.Option(..., exists=True, help="Feature parquet"),
    fail_on_warning: bool = typer.Option(
        False, "--fail-on-warning", help="Exit 2 if any drift exceeds threshold"
    ),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
) -> None:
    """Compare a sidecar's declared feature normalization against real data.

    Flags features whose declared mean drifts > 1σ from the observed
    data's mean, or where > 5% of samples are > 3σ from the declared
    mean. Catches the "trained on dataset A, deployed on dataset B"
    class of silent correctness bug.
    """
    from .validate_norm import render_table, validate_norm

    report = validate_norm(model, features)
    console.print(render_table(report))
    if json_out:
        _write_cli_report_json(
            json_out,
            report.to_dict(),
            args={
                "model": model,
                "features": features,
                "fail_on_warning": fail_on_warning,
                "json": json_out,
            },
            inputs={"model": model, "features": features},
        )
    if fail_on_warning and not report.ok:
        raise typer.Exit(code=2)


@app.command("profile")
def profile_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model to profile"),
    shape: list[str] | None = typer.Option(
        None,
        "--shape",
        help='Input shape as "N,C,H,W" (repeatable). Defaults to graph shape.',
    ),
    provider: list[str] | None = typer.Option(
        None,
        "--provider",
        help="ORT provider (repeatable). Defaults to all available.",
    ),
    warmup: int = typer.Option(5, help="Warmup iterations"),
    iters: int = typer.Option(100, help="Timed iterations"),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
) -> None:
    """Measure latency + peak RSS delta for a model across providers.

    Produces a table with mean / p50 / p99 latency and peak-RSS delta per
    (provider, shape). Useful both for picking a deployment target and
    as a CI gate ("this model must stay under 20 ms on CPU").
    """
    from .profile import profile_model, render_table

    shapes: list[tuple[int, ...]] | None = None
    if shape:
        shapes = [tuple(int(x) for x in s.split(",")) for s in shape]

    report = profile_model(
        model,
        shapes=shapes,
        providers=list(provider) if provider else None,
        warmup=warmup,
        iters=iters,
    )
    console.print(render_table(report))
    if json_out:
        _write_cli_report_json(
            json_out,
            report.to_dict(),
            args={
                "model": model,
                "shape": shape,
                "provider": provider,
                "warmup": warmup,
                "iters": iters,
                "json": json_out,
            },
            inputs={"model": model},
        )
        console.print(f"[green]Wrote {json_out}[/green]")


@app.command("audit-compat")
def audit_compat_cmd(
    model_dir: Path = typer.Option(
        Path("model/tiny"),
        exists=True,
        file_okay=False,
        help="Directory containing shipped .onnx models + sidecars",
    ),
    fail_on_warning: bool = typer.Option(
        False, "--fail-on-warning", help="Exit 2 if any audit issue is found"
    ),
) -> None:
    """Audit every tiny model in @p model_dir for feature-contract drift.

    Catches the common "new feature extractor broke old model" class of
    regression where libvmaf's FEATURE_COLUMNS count has changed but a
    shipped C1 model still expects the old shape.
    """
    from .audit import audit_dir, render_table

    audits = audit_dir(model_dir)
    console.print(render_table(audits))
    failed = [a for a in audits if not a.ok]
    if failed and fail_on_warning:
        console.print(f"[red]{len(failed)} model(s) have audit issues[/red]")
        raise typer.Exit(code=2)


@app.command("check-ops")
def check_ops_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model to validate"),
) -> None:
    """Check an ONNX model against libvmaf's op allowlist.

    Parses core/src/dnn/op_allowlist.c (the runtime source of truth)
    and reports any op the model uses that libvmaf would reject at load
    time. Exits 2 if forbidden ops are found.
    """
    from .op_allowlist import check_model

    report = check_model(model)
    if report.ok:
        console.print(f"[green]{report.pretty()}[/green]")
        return
    console.print(f"[red]{report.pretty()}[/red]")
    console.print(
        "[yellow]Extend core/src/dnn/op_allowlist.c only when a shipped model "
        "genuinely needs the op — see docs/tiny-ai/security.md[/yellow]"
    )
    raise typer.Exit(code=2)


@app.command("audit-learned-filter")
def audit_learned_filter_cmd(
    model: Path = typer.Option(..., exists=True, help="Learned-filter ONNX model"),
    frames: Path = typer.Option(
        ...,
        exists=True,
        dir_okay=False,
        help="NumPy .npy file of shape (N, H, W) with values in [0, peak]",
    ),
    peak: float = typer.Option(1.0, help="Max pixel value (1.0 for normalized luma)"),
    input_name: str = typer.Option("input"),
    ssim_min: float = typer.Option(0.6, help="Warn if per-frame SSIM(in, out) < this"),
    mean_shift_max: float = typer.Option(0.05),
    std_ratio_max: float = typer.Option(2.0),
    clip_fraction_max: float = typer.Option(0.01),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
    fail_on_warning: bool = typer.Option(False, "--fail-on-warning"),
) -> None:
    """Pre-deploy audit for a learned-filter (C3) ONNX model.

    Runs the filter over a corpus of frames and flags four failure
    modes: mean shift (output brighter/darker than input), std inflation
    (filter amplifies noise), clipping at codec boundaries, and SSIM
    collapse (filter destroyed structure). Catches the "trained on
    clean content, deployed on heavily-compressed content" class of
    silent failure before the model hits a production pipeline.
    """
    import numpy as np

    from .learned_filter_audit import audit_learned_filter, render_table

    corpus = np.load(frames)
    if corpus.ndim != 3:
        console.print(f"[red]{frames} must be (N, H, W); got {corpus.shape}[/red]")
        raise typer.Exit(code=2)
    frames_list = [corpus[i] for i in range(corpus.shape[0])]

    report = audit_learned_filter(
        model=model,
        frames=frames_list,
        peak=peak,
        input_name=input_name,
        ssim_min=ssim_min,
        mean_shift_max=mean_shift_max,
        std_ratio_max=std_ratio_max,
        clip_fraction_max=clip_fraction_max,
    )
    console.print(render_table(report))
    if json_out:
        _write_cli_report_json(
            json_out,
            report.to_dict(),
            args={
                "model": model,
                "frames": frames,
                "peak": peak,
                "input_name": input_name,
                "ssim_min": ssim_min,
                "mean_shift_max": mean_shift_max,
                "std_ratio_max": std_ratio_max,
                "clip_fraction_max": clip_fraction_max,
                "json": json_out,
                "fail_on_warning": fail_on_warning,
            },
            inputs={"model": model, "frames": frames},
        )
        console.print(f"[green]Wrote {json_out}[/green]")
    if fail_on_warning and not report.ok:
        raise typer.Exit(code=2)


@app.command("quantize-int8")
def quantize_int8_cmd(
    fp32: Path = typer.Option(..., exists=True, help="Input fp32 .onnx path"),
    output: Path = typer.Option(..., help="Output int8 .onnx path"),
    calibration: Path = typer.Option(
        ..., exists=True, help="Parquet feature cache used for PTQ calibration"
    ),
    input_name: str = typer.Option("features"),
    n_calibration: int = typer.Option(512, help="Calibration sample count"),
    batch_size: int = typer.Option(32, help="Calibration batch size"),
    rmse_gate: float = typer.Option(
        1.0,
        help="Exit 2 if the INT8-vs-fp32 RMSE on held-out samples exceeds this",
    ),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
) -> None:
    """Post-training quantize a fp32 ONNX model to INT8 (static PTQ, QDQ format).

    Uses a parquet feature cache as the calibration source (same schema
    as the features consumed by `vmaf-train eval`). Outputs drift
    statistics against held-out samples and exits 2 if the RMSE breaks
    the gate — protects against "we shipped the int8 model but it
    silently lost 5 VMAF points".
    """
    from .quantize import quantize_int8, render_table

    report = quantize_int8(
        fp32_path=fp32,
        int8_path=output,
        calibration=calibration,
        input_name=input_name,
        n_calibration=n_calibration,
        batch_size=batch_size,
    )
    console.print(render_table(report))
    if json_out:
        _write_cli_report_json(
            json_out,
            report.to_dict(),
            args={
                "fp32": fp32,
                "output": output,
                "calibration": calibration,
                "input_name": input_name,
                "n_calibration": n_calibration,
                "batch_size": batch_size,
                "rmse_gate": rmse_gate,
                "json": json_out,
            },
            inputs={"fp32": fp32, "calibration": calibration},
            outputs={"int8_model": str(output)},
        )
        console.print(f"[green]Wrote {json_out}[/green]")
    if report.rmse > rmse_gate:
        console.print(f"[red]INT8 drift RMSE {report.rmse:.3g} exceeds gate {rmse_gate:g}[/red]")
        raise typer.Exit(code=2)


@app.command("cross-backend")
def cross_backend_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model to check"),
    features: Path | None = typer.Option(
        None, exists=True, help="Feature parquet (if omitted, synthetic input is used)"
    ),
    provider: list[str] | None = typer.Option(
        None,
        "--provider",
        help="ORT provider (repeatable). Defaults to every non-CPU available provider.",
    ),
    shape: str | None = typer.Option(
        None, help='Synthetic input shape as "N,C,H,W" (ignored when --features is given)'
    ),
    n_rows: int = typer.Option(256, help="Max rows to pull from features parquet"),
    atol: float = typer.Option(
        1e-3, help="Absolute-error threshold — exits 2 on any provider above this"
    ),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
    fail_on_mismatch: bool = typer.Option(
        False, "--fail-on-mismatch", help="Exit 2 when any provider exceeds atol"
    ),
) -> None:
    """Run a model on CPU + every other execution provider and diff outputs.

    Guards against the "provider A passes CI, provider B silently ships a
    VMAF-point drift in prod" class of bug. Mirrors the ≤2-ULP discipline
    we apply to VMAF's own cross-backend scoring.
    """
    from .cross_backend import compare_backends, render_table

    parsed_shape: tuple[int, ...] | None = None
    if shape:
        parsed_shape = tuple(int(x) for x in shape.split(","))
    report = compare_backends(
        model_path=model,
        providers=list(provider) if provider else None,
        features=features,
        shape=parsed_shape,
        n_rows=n_rows,
        atol=atol,
    )
    console.print(render_table(report))
    if json_out:
        inputs: dict[str, Any] = {"model": model}
        if features is not None:
            inputs["features"] = features
        _write_cli_report_json(
            json_out,
            report.to_dict(),
            args={
                "model": model,
                "features": features,
                "provider": provider,
                "shape": shape,
                "n_rows": n_rows,
                "atol": atol,
                "json": json_out,
                "fail_on_mismatch": fail_on_mismatch,
            },
            inputs=inputs,
        )
        console.print(f"[green]Wrote {json_out}[/green]")
    if fail_on_mismatch and not report.ok:
        raise typer.Exit(code=2)


@app.command("bisect-model-quality")
def bisect_model_quality_cmd(
    models: list[Path] = typer.Argument(
        ..., help="Ordered list of ONNX checkpoints (head = assumed-good, tail = assumed-bad)"
    ),
    features: Path = typer.Option(
        ..., exists=True, help="Held-out features parquet with a `mos` column"
    ),
    min_plcc: float | None = typer.Option(None, help="PLCC lower bound"),
    min_srocc: float | None = typer.Option(None, help="SROCC lower bound"),
    max_rmse: float | None = typer.Option(None, help="RMSE upper bound"),
    input_name: str = typer.Option("input", help="ONNX model input tensor name"),
    json_out: Path | None = typer.Option(None, "--json", help="Write JSON report"),
    fail_on_first_bad: bool = typer.Option(
        False, "--fail-on-first-bad", help="Exit 2 when a regression is localized"
    ),
) -> None:
    """Binary-search a list of ONNX checkpoints for the first quality regression.

    Use on a timeline of checkpoints (training-run intermediates, release
    history) to pinpoint the step where PLCC/SROCC dropped below a gate.
    Assumes the list is ordered good→bad; if not, exits 0 with the verdict
    "no regression detected" or "first model already fails".
    """
    import pandas as pd

    from .bisect_model_quality import bisect_model_quality, render_table
    from .data.feature_dump import DEFAULT_FEATURES

    df = pd.read_parquet(features)
    if "mos" not in df.columns:
        raise typer.BadParameter("features parquet must contain a 'mos' column")
    df = df.dropna(subset=["mos"])
    feat_cols = [c for c in DEFAULT_FEATURES if c in df.columns]
    if not feat_cols:
        raise typer.BadParameter("features parquet has none of the expected feature columns")
    feat_matrix = df[feat_cols].to_numpy(dtype="float32")
    targets = df["mos"].to_numpy(dtype="float32")

    result = bisect_model_quality(
        models=list(models),
        features=feat_matrix,
        targets=targets,
        min_plcc=min_plcc,
        min_srocc=min_srocc,
        max_rmse=max_rmse,
        input_name=input_name,
    )
    console.print(render_table(result))
    if json_out:
        _write_cli_report_json(
            json_out,
            result.to_dict(),
            args={
                "models": list(models),
                "features": features,
                "min_plcc": min_plcc,
                "min_srocc": min_srocc,
                "max_rmse": max_rmse,
                "input_name": input_name,
                "json": json_out,
                "fail_on_first_bad": fail_on_first_bad,
            },
            inputs={"models": list(models), "features": features},
        )
        console.print(f"[green]Wrote {json_out}[/green]")
    if (
        fail_on_first_bad
        and result.first_bad_index is not None
        and result.last_good_index is not None
    ):
        raise typer.Exit(code=2)


@app.command("register")
def register_cmd(
    model: Path = typer.Option(..., exists=True, help="ONNX model to register"),
    kind: str = typer.Option(..., help="fr|nr|filter"),
    dataset: str | None = typer.Option(None),
    license: str | None = typer.Option(None, "--license"),
    train_commit: str | None = typer.Option(None),
    train_config: Path | None = typer.Option(None, exists=True),
    manifest: Path | None = typer.Option(None, exists=True),
) -> None:
    """Write the sidecar metadata JSON alongside a shipped ONNX model."""
    from .registry import register

    sidecar = register(
        onnx_path=model,
        kind=kind,
        dataset=dataset,
        license_=license,
        train_commit=train_commit,
        train_config=train_config,
        manifest=manifest,
    )
    console.print(f"[green]Wrote sidecar {sidecar}[/green]")


if __name__ == "__main__":
    app()

#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
"""External-competitor benchmark orchestrator.

Side-by-side numerical comparison between the fork's
``fr_regressor_v2_ensemble`` / ``nr_metric_v1`` predictors and two
external open-source competitors:

* **Synamedia/Quortex x264-pVMAF** (GPL-2.0).
* **DOVER-Mobile** (Apache-2.0 code, CC-BY-NC-SA 4.0 weights).

Per ADR-0332 the harness is wrapper-only: each competitor lives in
its own `run.sh` that invokes a user-installed external binary and
re-shapes its output into the harness schema. No GPL'd code lives
inside this fork.

Schema (every wrapper emits):

    {
      "frames": [
        {"frame_idx": int, "predicted_vmaf_or_mos": float,
         "runtime_ms": float}, ...
      ],
      "summary": {
        "competitor": str, "plcc": float, "srocc": float,
        "rmse": float, "runtime_total_ms": float,
        "params": int, "gflops": float
      }
    }

The orchestrator runs every wrapper across the requested corpus,
aggregates the four ``summary`` blocks, and emits a comparison
table to stdout (and optionally to ``--out-json``).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterable, Sequence
from typing import Any

HERE = pathlib.Path(__file__).resolve().parent

# ---- corpus discovery -------------------------------------------------------

# Default expected paths. When the user does not pass --bvi-dvc-root /
# --netflix-public-root we look in these locations and fail clearly
# if neither is present.
DEFAULT_BVI_DVC_ROOT = pathlib.Path.home() / ".workingdir2" / "bvi-dvc"
DEFAULT_NETFLIX_PUBLIC_ROOT = HERE.parent.parent / ".workingdir2" / "netflix"


@dataclasses.dataclass(frozen=True)
class CorpusItem:
    """One (ref, dis) pair on disk."""

    name: str
    ref: pathlib.Path | None  # None for no-reference runs
    dis: pathlib.Path
    width: int
    height: int
    fps: float = 24.0
    pixfmt: str = "yuv420p"


def _bvi_dvc_test_fold(root: pathlib.Path) -> list[CorpusItem]:
    """Default BVI-DVC test fold.

    The fork ingests BVI-DVC under ADR-0310. The test fold lists are
    deterministic by sorted source filename; we surface every
    ``<src>__dis<N>.yuv`` pair we find under ``<root>/test/``. If
    ``<root>`` does not exist we return an empty list — the caller
    decides whether to error.
    """
    fold = root / "test"
    if not fold.is_dir():
        return []
    items: list[CorpusItem] = []
    for ref in sorted(fold.glob("*__ref.yuv")):
        # filename convention: <stem>__ref.yuv + <stem>__dis*.yuv
        stem = ref.stem.removesuffix("__ref")
        # geometry encoded in the stem as e.g. ..._1920x1080_...
        w = h = 0
        for part in stem.split("_"):
            if "x" in part and part.replace("x", "").isdigit():
                ws, hs = part.split("x", 1)
                w, h = int(ws), int(hs)
                break
        if w == 0 or h == 0:
            continue
        for dis in sorted(fold.glob(f"{stem}__dis*.yuv")):
            items.append(
                CorpusItem(
                    name=f"bvi-dvc/{dis.name}",
                    ref=ref,
                    dis=dis,
                    width=w,
                    height=h,
                )
            )
    return items


def _netflix_public_drop(root: pathlib.Path) -> list[CorpusItem]:
    """Netflix Public Drop dis files (paired against the source ref).

    The Public Drop's local layout (see ADR-0310 / state.md) is
    ``<root>/<src>/{ref,dis}/*.yuv``. We pair every ``dis/*.yuv``
    against its sibling ``ref/*.yuv``.
    """
    if not root.is_dir():
        return []
    items: list[CorpusItem] = []
    for src_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        ref_dir = src_dir / "ref"
        dis_dir = src_dir / "dis"
        if not (ref_dir.is_dir() and dis_dir.is_dir()):
            continue
        refs = sorted(ref_dir.glob("*.yuv"))
        if not refs:
            continue
        ref = refs[0]
        # geometry from src_dir name e.g. ..._576x324_...
        w = h = 0
        for part in src_dir.name.split("_"):
            if "x" in part and part.replace("x", "").isdigit():
                ws, hs = part.split("x", 1)
                w, h = int(ws), int(hs)
                break
        if w == 0 or h == 0:
            continue
        for dis in sorted(dis_dir.glob("*.yuv")):
            items.append(
                CorpusItem(
                    name=f"netflix-public/{src_dir.name}/{dis.name}",
                    ref=ref,
                    dis=dis,
                    width=w,
                    height=h,
                )
            )
    return items


def discover_corpus(
    bvi_dvc_root: pathlib.Path | None,
    netflix_root: pathlib.Path | None,
) -> list[CorpusItem]:
    """Aggregate corpus from BVI-DVC test fold + Netflix Public Drop."""
    bvi_root = bvi_dvc_root or DEFAULT_BVI_DVC_ROOT
    nf_root = netflix_root or DEFAULT_NETFLIX_PUBLIC_ROOT
    return _bvi_dvc_test_fold(bvi_root) + _netflix_public_drop(nf_root)


# ---- wrapper invocation -----------------------------------------------------

WRAPPERS: dict[str, pathlib.Path] = {
    "fork-fr-regressor": HERE / "fork-fr-regressor" / "run.sh",
    "fork-nr-metric": HERE / "fork-nr-metric" / "run.sh",
    "x264-pvmaf": HERE / "x264-pvmaf" / "run.sh",
    "dover-mobile": HERE / "dover-mobile" / "run.sh",
}

# Subprocess injection point — overridden in tests.
SubprocessRunner = Callable[..., "subprocess.CompletedProcess[Any]"]


def _require_number(value: object, *, path: str) -> float:
    """Return ``value`` as float, rejecting booleans and non-numbers."""
    if isinstance(value, bool) or not isinstance(value, int | float):
        raise ValueError(f"{path} must be numeric")
    return float(value)


def validate_wrapper_output(competitor: str, payload: object) -> dict[str, Any]:
    """Validate and normalise one wrapper JSON payload.

    Wrapper scripts are the licence-boundary seam for this harness, so
    malformed JSON should fail at the seam with a clear error instead
    of surfacing later as ``KeyError`` / ``TypeError`` in aggregation.
    Extra keys are allowed; required keys and numeric fields are
    checked.
    """
    if not isinstance(payload, dict):
        raise ValueError("wrapper output must be a JSON object")

    frames = payload.get("frames")
    if not isinstance(frames, list):
        raise ValueError("frames must be a list")
    for idx, frame in enumerate(frames):
        if not isinstance(frame, dict):
            raise ValueError(f"frames[{idx}] must be an object")
        frame_idx = frame.get("frame_idx")
        if isinstance(frame_idx, bool) or not isinstance(frame_idx, int):
            raise ValueError(f"frames[{idx}].frame_idx must be an integer")
        _require_number(
            frame.get("predicted_vmaf_or_mos"),
            path=f"frames[{idx}].predicted_vmaf_or_mos",
        )
        _require_number(frame.get("runtime_ms"), path=f"frames[{idx}].runtime_ms")

    summary = payload.get("summary")
    if not isinstance(summary, dict):
        raise ValueError("summary must be an object")
    if summary.get("competitor") != competitor:
        raise ValueError(
            "summary.competitor must match wrapper name "
            f"{competitor!r}, got {summary.get('competitor')!r}"
        )
    for key in ("plcc", "srocc", "rmse", "runtime_total_ms", "params", "gflops"):
        _require_number(summary.get(key), path=f"summary.{key}")

    return payload


def run_wrapper(
    competitor: str,
    item: CorpusItem,
    out_path: pathlib.Path,
    runner: SubprocessRunner | None = None,
) -> dict[str, Any]:
    """Invoke one wrapper for one corpus item; return parsed JSON.

    The runner argument lets unit tests substitute a stub that
    writes canned `output.json` and returns a successful result
    without touching real binaries. Resolved at call time (rather
    than at definition time via a default arg binding) so tests
    that monkeypatch ``subprocess.run`` are honoured.
    """
    if runner is None:
        runner = subprocess.run
    wrapper = WRAPPERS[competitor]
    cmd: list[str] = [
        "bash",
        str(wrapper),
        "--dis",
        str(item.dis),
        "--width",
        str(item.width),
        "--height",
        str(item.height),
        "--fps",
        f"{item.fps}",
        "--pixfmt",
        item.pixfmt,
        "--out",
        str(out_path),
    ]
    if item.ref is not None:
        cmd.extend(["--ref", str(item.ref)])

    proc = runner(cmd, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"wrapper {competitor} failed (rc={proc.returncode}): " f"{proc.stderr.strip()}"
        )
    if not out_path.is_file():
        raise RuntimeError(f"wrapper {competitor} did not produce {out_path}")
    try:
        payload = json.loads(out_path.read_text())
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"wrapper {competitor} produced invalid JSON: {exc}") from exc
    try:
        return validate_wrapper_output(competitor, payload)
    except ValueError as exc:
        raise RuntimeError(f"wrapper {competitor} produced invalid schema: {exc}") from exc


# ---- aggregation ------------------------------------------------------------


@dataclasses.dataclass
class CompetitorAggregate:
    competitor: str
    n_clips: int
    plcc_mean: float
    srocc_mean: float
    rmse_mean: float
    runtime_total_ms: float
    params: int
    gflops: float


def _mean(xs: Sequence[float]) -> float:
    return sum(xs) / len(xs) if xs else float("nan")


def aggregate(
    competitor: str,
    results: Iterable[dict[str, Any]],
) -> CompetitorAggregate:
    summaries = [r["summary"] for r in results]
    return CompetitorAggregate(
        competitor=competitor,
        n_clips=len(summaries),
        plcc_mean=_mean([s.get("plcc", float("nan")) for s in summaries]),
        srocc_mean=_mean([s.get("srocc", float("nan")) for s in summaries]),
        rmse_mean=_mean([s.get("rmse", float("nan")) for s in summaries]),
        runtime_total_ms=sum(s.get("runtime_total_ms", 0.0) for s in summaries),
        params=int(summaries[0].get("params", 0)) if summaries else 0,
        gflops=float(summaries[0].get("gflops", 0.0)) if summaries else 0.0,
    )


def render_table(aggs: Sequence[CompetitorAggregate]) -> str:
    """Render a fixed-width comparison table as plain text."""
    headers = ("competitor", "n", "PLCC", "SROCC", "RMSE", "runtime_ms", "params", "GFLOPs")
    rows = [headers] + [
        (
            a.competitor,
            str(a.n_clips),
            f"{a.plcc_mean:.4f}",
            f"{a.srocc_mean:.4f}",
            f"{a.rmse_mean:.4f}",
            f"{a.runtime_total_ms:.0f}",
            str(a.params),
            f"{a.gflops:.2f}",
        )
        for a in aggs
    ]
    widths = [max(len(r[c]) for r in rows) for c in range(len(headers))]
    sep = "  ".join("-" * w for w in widths)
    out = []
    for i, r in enumerate(rows):
        out.append("  ".join(c.ljust(w) for c, w in zip(r, widths, strict=True)))
        if i == 0:
            out.append(sep)
    return "\n".join(out)


# ---- main -------------------------------------------------------------------


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="external-bench/compare.py",
        description=(
            "Side-by-side benchmark: fork fr_regressor_v2_ensemble + "
            "nr_metric_v1 vs Synamedia x264-pVMAF and DOVER-Mobile. "
            "Wrapper-only architecture per ADR-0332."
        ),
    )
    p.add_argument(
        "--bvi-dvc-root",
        type=pathlib.Path,
        default=None,
        help=f"BVI-DVC corpus root (default: {DEFAULT_BVI_DVC_ROOT})",
    )
    p.add_argument(
        "--netflix-public-root",
        type=pathlib.Path,
        default=None,
        help=f"Netflix Public Drop root " f"(default: {DEFAULT_NETFLIX_PUBLIC_ROOT})",
    )
    p.add_argument(
        "--competitors",
        nargs="+",
        default=list(WRAPPERS.keys()),
        choices=list(WRAPPERS.keys()),
        help="Subset of competitors to run (default: all four)",
    )
    p.add_argument(
        "--out-json",
        type=pathlib.Path,
        default=None,
        help="Write the aggregated comparison as JSON in addition to "
        "rendering the table on stdout.",
    )
    p.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Truncate the corpus to the first N items (smoke runs).",
    )
    return p.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(list(sys.argv[1:] if argv is None else argv))

    corpus = discover_corpus(args.bvi_dvc_root, args.netflix_public_root)
    if not corpus:
        bvi = args.bvi_dvc_root or DEFAULT_BVI_DVC_ROOT
        nf = args.netflix_public_root or DEFAULT_NETFLIX_PUBLIC_ROOT
        print(
            f"external-bench: no corpus found.\n"
            f"  BVI-DVC test fold expected at: {bvi}/test/\n"
            f"  Netflix Public Drop expected at: {nf}/<src>/{{ref,dis}}/*.yuv\n"
            f"Pass --bvi-dvc-root / --netflix-public-root or place the\n"
            f"corpora at the documented paths. The fork ships neither\n"
            f"(licence: see tools/external-bench/README.md).",
            file=sys.stderr,
        )
        return 4
    if args.limit > 0:
        corpus = corpus[: args.limit]

    aggs: list[CompetitorAggregate] = []
    with tempfile.TemporaryDirectory(prefix="external-bench-") as td:
        td_path = pathlib.Path(td)
        for competitor in args.competitors:
            results: list[dict[str, Any]] = []
            for i, item in enumerate(corpus):
                out_path = td_path / f"{competitor}-{i:04d}.json"
                try:
                    results.append(run_wrapper(competitor, item, out_path))
                except RuntimeError as e:
                    print(f"  skip {competitor}/{item.name}: {e}", file=sys.stderr)
            aggs.append(aggregate(competitor, results))

    table = render_table(aggs)
    print(table)

    if args.out_json:
        args.out_json.write_text(
            json.dumps(
                [dataclasses.asdict(a) for a in aggs],
                indent=2,
            )
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

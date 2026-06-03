#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Real-corpus LOSO validator for the ``fr_regressor_v2`` deep ensemble.

Companion to ``ai/scripts/run_ensemble_v2_real_corpus_loso.sh``
(ADR-0309). Reads ``loso_seed{0..4}.json`` artefacts from the run
directory, applies the two-part production-flip gate from
``scripts/ci/ensemble_prod_gate.py`` (ADR-0303), and emits one of:

* ``PROMOTE.json`` — gate passed; recommends flipping the five
  ``fr_regressor_v2_ensemble_v1_seed{0..4}`` rows in
  ``model/tiny/registry.json`` from ``smoke: true`` to ``smoke: false``.
* ``HOLD.json`` — gate failed; recommends keeping ``smoke: true`` and
  investigating diversity / hyperparameters.

Both files include a sha256 snapshot of the corpus YUV file list so
the verdict is reproducible against the exact data used. The
registry flip itself is intentionally a separate PR (per ADR-0309 +
the ai/AGENTS.md invariant) — this script only emits the verdict.

Usage::

    python ai/scripts/validate_ensemble_seeds.py runs/ensemble_v2_real/
    python ai/scripts/validate_ensemble_seeds.py --help

Exit codes mirror ``ensemble_prod_gate.py``: 0 on PROMOTE, 1 on HOLD,
2 on input error.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

# Hoist the gate evaluator from scripts/ci/ so we share a single
# source of truth for the threshold constants. ADR-0303 forbids
# divergent copies of the gate logic.
_SCRIPT_PATHS = bootstrap_ai_script(__file__)
_SCRIPT_PATH = _SCRIPT_PATHS.script_path
_REPO_ROOT = _SCRIPT_PATHS.repo_root
sys.path.insert(0, str(_REPO_ROOT / "scripts" / "ci"))

from ensemble_prod_gate import (  # noqa: E402  # type: ignore[import-not-found]  (sys.path edit above)
    DEFAULT_ENSEMBLE_SIZE,
    SHIP_GATE_MEAN_PLCC,
    SHIP_GATE_PLCC_SPREAD_MAX,
    evaluate_gate,
    load_seed_jsons,
)

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def build_argparser() -> argparse.ArgumentParser:
    """Build the CLI argparser; exposed for tests."""
    p = make_argument_parser(
        prog="validate_ensemble_seeds",
        description=(
            "Apply the ADR-0303 two-part production-flip gate to "
            "real-corpus LOSO artefacts and emit PROMOTE.json / HOLD.json. "
            "ADR-0309."
        ),
    )
    p.add_argument(
        "loso_dir",
        type=Path,
        help=(
            "Directory containing loso_seed{0..4}.json artefacts emitted "
            "by ai/scripts/run_ensemble_v2_real_corpus_loso.sh."
        ),
    )
    p.add_argument(
        "--corpus-root",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_NETFLIX_CORPUS_DIR",
                str(_REPO_ROOT / ".corpus" / "netflix"),
            )
        ),
        help=(
            "Corpus root used during the LOSO run; sha256-snapshotted "
            "into the verdict JSON for reproducibility (default: "
            "``$VMAF_NETFLIX_CORPUS_DIR`` or ``.corpus/netflix``)."
        ),
    )
    p.add_argument(
        "--seeds",
        type=str,
        default=",".join(str(i) for i in range(DEFAULT_ENSEMBLE_SIZE)),
        help="Comma-separated seed list (default: 0,1,2,3,4).",
    )
    return p


def _parse_seed_list(raw: str) -> list[int]:
    out: list[int] = []
    for raw_tok in raw.split(","):
        tok = raw_tok.strip()
        if not tok:
            continue
        out.append(int(tok))
    if not out:
        raise ValueError("--seeds must be a non-empty comma-separated list of ints")
    return out


def snapshot_corpus(corpus_root: Path) -> dict:
    """Compute a deterministic sha256 over the corpus YUV file list.

    We hash the *sorted relative paths and sizes*, not the YUV bytes
    themselves (37 GB would dominate validate runtime). That's enough
    to detect "wrong corpus revision" without re-reading every YUV.
    Returns ``{"present": False, ...}`` if the corpus root is missing
    so the verdict is still emittable for offline replays.
    """
    if not corpus_root.exists():
        return {
            "present": False,
            "corpus_root": str(corpus_root),
            "sha256": None,
            "ref_count": 0,
            "dis_count": 0,
        }

    yuvs = sorted(corpus_root.rglob("*.yuv"))
    hasher = hashlib.sha256()
    ref_count = 0
    dis_count = 0
    for path in yuvs:
        rel = path.relative_to(corpus_root).as_posix()
        size = path.stat().st_size
        hasher.update(f"{rel}\t{size}\n".encode("utf-8"))
        # Heuristic counts; mirrors the wrapper's logic.
        if "/ref/" in f"/{rel}/" or "ref" in path.stem.lower():
            ref_count += 1
        if "/dis/" in f"/{rel}/" or "dis" in path.stem.lower():
            dis_count += 1

    return {
        "present": True,
        "corpus_root": str(corpus_root),
        "sha256": hasher.hexdigest(),
        "ref_count": ref_count,
        "dis_count": dis_count,
        "yuv_count": len(yuvs),
    }


def _build_verdict(
    report: dict,
    corpus_snapshot: dict,
    seeds: list[int],
    loso_dir: Path,
    corpus_root: Path,
    mean_threshold: float,
    spread_max: float,
    argv: list[str],
) -> dict:
    """Assemble the verdict payload written to PROMOTE.json / HOLD.json."""
    verdict_kind = "PROMOTE" if report["passed"] else "HOLD"
    out_path = loso_dir / f"{verdict_kind}.json"
    if verdict_kind == "PROMOTE":
        recommendation = (
            "flip seeds smoke->false in model/tiny/registry.json "
            "(ADR-0309: registry-flip is a separate follow-up PR; "
            "DO NOT flip during a rebase or in the same PR as this "
            "verdict file)"
        )
    else:
        recommendation = (
            "keep smoke=true; investigate diversity / hyperparameters. "
            "Failing aspects: " + ", ".join(_failure_aspects(report))
        )
    return {
        "schema_version": 1,
        "verdict": verdict_kind,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "loso_dir": str(loso_dir),
        "seeds": seeds,
        "gate": report,
        "corpus": corpus_snapshot,
        "recommendation": recommendation,
        "adr": "ADR-0309",
        "parent_adr": "ADR-0303",
        "run_provenance": build_run_provenance(
            entrypoint=_SCRIPT_PATH,
            repo_root=_REPO_ROOT,
            argv=argv,
            args={
                "corpus_root": corpus_root,
                "loso_dir": loso_dir,
                "mean_threshold": mean_threshold,
                "plcc_spread_max": spread_max,
                "seeds": seeds,
            },
            inputs={"corpus_root": corpus_root, "loso_dir": loso_dir},
            outputs={"verdict": out_path},
        ),
    }


def _failure_aspects(report: dict) -> list[str]:
    aspects: list[str] = []
    if not report["mean_plcc_pass"]:
        aspects.append(
            f"mean_plcc {report['mean_plcc']:.4f} < " f"{report['mean_plcc_threshold']:.4f}"
        )
    if not report["plcc_spread_pass"]:
        aspects.append(
            f"plcc_spread {report['plcc_spread']:.4f} > " f"{report['plcc_spread_max']:.4f}"
        )
    if not report["per_seed_pass"]:
        aspects.append(f"failing seeds {report['failing_seeds']}")
    return aspects or ["unknown"]


def write_verdict(verdict: dict, loso_dir: Path) -> Path:
    """Write the verdict to PROMOTE.json or HOLD.json under ``loso_dir``."""
    out_path = loso_dir / f"{verdict['verdict']}.json"
    write_manifest_json(out_path, verdict)
    return out_path


def run_validation(
    loso_dir: Path,
    seeds: list[int],
    corpus_root: Path,
    mean_threshold: float = SHIP_GATE_MEAN_PLCC,
    spread_max: float = SHIP_GATE_PLCC_SPREAD_MAX,
    argv: list[str] | None = None,
) -> tuple[dict, Path]:
    """Run the full validate flow; returns ``(verdict, written_path)``.

    Pure-function entry point so tests can drive it without argv.
    """
    payloads = load_seed_jsons(loso_dir, seeds)
    report = evaluate_gate(
        payloads,
        mean_plcc_threshold=mean_threshold,
        plcc_spread_max=spread_max,
        per_seed_min=mean_threshold,
    )
    corpus_snapshot = snapshot_corpus(corpus_root)
    verdict = _build_verdict(
        report,
        corpus_snapshot,
        seeds,
        loso_dir,
        corpus_root,
        mean_threshold,
        spread_max,
        argv or [],
    )
    written = write_verdict(verdict, loso_dir)
    return verdict, written


def main(argv: list[str] | None = None) -> int:
    parser = build_argparser()
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    try:
        seeds = _parse_seed_list(args.seeds)
    except ValueError as exc:
        print(f"[validate-ensemble] error: {exc}", file=sys.stderr)
        return 2

    if not args.loso_dir.exists() or not args.loso_dir.is_dir():
        print(
            f"[validate-ensemble] error: loso_dir not found or not a "
            f"directory: {args.loso_dir}",
            file=sys.stderr,
        )
        return 2

    try:
        verdict, written = run_validation(args.loso_dir, seeds, args.corpus_root, argv=raw_argv)
    except (FileNotFoundError, ValueError) as exc:
        print(f"[validate-ensemble] error: {exc}", file=sys.stderr)
        return 2

    print(
        f"[validate-ensemble] verdict={verdict['verdict']} "
        f"mean_plcc={verdict['gate']['mean_plcc']:.4f} "
        f"spread={verdict['gate']['plcc_spread']:.4f} -> {written}"
    )
    return 0 if verdict["verdict"] == "PROMOTE" else 1


if __name__ == "__main__":
    sys.exit(main())

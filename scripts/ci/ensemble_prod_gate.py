#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Production-flip CI gate for the ``fr_regressor_v2`` deep ensemble (ADR-0303).

Reads the per-seed ``loso_seed{N}.json`` artefacts emitted by
``ai/scripts/train_fr_regressor_v2_ensemble_loso.py`` and decides
whether the ensemble seeds are clear to flip from ``smoke: true`` to
``smoke: false`` in ``model/tiny/registry.json``.

The gate is two-part per ADR-0303 / Research-0075:

1. **Mean per-seed PLCC ≥ 0.95** — inherits the ADR-0235 / ADR-0291
   ship gate; ensures every individual seed clears production
   threshold.
2. **PLCC spread ≤ 0.005** — ``max_i(PLCC_i) - min_i(PLCC_i) ≤ 0.005``
   protects the predictive-distribution semantics. Without it, the
   ensemble mean could mask a one-seed-wins-four-seeds-tie
   configuration that breaks the conformal calibration assumption
   (Research-0075 §Conformal calibration sketch).

Exit codes:

* ``0`` — gate passed; the seeds are clear to flip.
* ``1`` — gate failed (mean PLCC below threshold, spread above
  threshold, or per-seed gate violation).
* ``2`` — input error (missing JSONs, malformed schema, etc.).

This script is not wired into ``.github/workflows/`` in the same PR
as the scaffold (no real ``loso_seed{N}.json`` artefacts on master to
gate on yet); the follow-up flip PR adds the workflow job that
invokes this gate. Until then, the script is reachable manually:

    python scripts/ci/ensemble_prod_gate.py runs/ensemble_loso/
    python scripts/ci/ensemble_prod_gate.py --help
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Per ADR-0303 §Decision — the two parts of the production ship gate.
# Centralising the constants here means the workflow + the trainer's
# emitted JSON both reference a single source of truth.
SHIP_GATE_MEAN_PLCC: float = 0.95
SHIP_GATE_PLCC_SPREAD_MAX: float = 0.005

# Default ensemble size — five seeds per Lakshminarayanan 2017 +
# ADR-0303 / Research-0075. Configurable in case a future ADR
# supersedes with a larger ensemble.
DEFAULT_ENSEMBLE_SIZE: int = 5


def build_argparser() -> argparse.ArgumentParser:
    """Build the CLI argparser (exposed for test imports)."""
    p = argparse.ArgumentParser(
        prog="ensemble_prod_gate",
        description=(
            "Production-flip CI gate for fr_regressor_v2 deep ensemble "
            "(ADR-0303). Returns exit 0 iff mean(PLCC_i) >= "
            f"{SHIP_GATE_MEAN_PLCC} AND max(PLCC_i) - min(PLCC_i) <= "
            f"{SHIP_GATE_PLCC_SPREAD_MAX}."
        ),
    )
    p.add_argument(
        "loso_dir",
        type=Path,
        help=(
            "Directory containing loso_seed{N}.json artefacts emitted by "
            "ai/scripts/train_fr_regressor_v2_ensemble_loso.py."
        ),
    )
    p.add_argument(
        "--seeds",
        type=str,
        default="0,1,2,3,4",
        help=("Comma-separated seed list expected to be present " "(default: 0,1,2,3,4)."),
    )
    p.add_argument(
        "--mean-plcc-threshold",
        type=float,
        default=SHIP_GATE_MEAN_PLCC,
        help=(
            "Override the mean-PLCC ship gate (default "
            f"{SHIP_GATE_MEAN_PLCC} — DO NOT lower without superseding "
            "ADR-0303)."
        ),
    )
    p.add_argument(
        "--plcc-spread-max",
        type=float,
        default=SHIP_GATE_PLCC_SPREAD_MAX,
        help=(
            "Override the PLCC spread bound (default "
            f"{SHIP_GATE_PLCC_SPREAD_MAX} — DO NOT raise without "
            "superseding ADR-0303; the bound is load-bearing for the "
            "ensemble's predictive-distribution semantics)."
        ),
    )
    p.add_argument(
        "--per-seed-min",
        type=float,
        default=SHIP_GATE_MEAN_PLCC,
        help=(
            "Per-seed minimum PLCC threshold; defaults to the same "
            f"{SHIP_GATE_MEAN_PLCC} ship gate. A seed below this "
            "threshold blocks even an otherwise-passing ensemble."
        ),
    )
    p.add_argument(
        "--json",
        action="store_true",
        help="Emit a machine-readable JSON summary on stdout.",
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


def load_seed_jsons(loso_dir: Path, seeds: list[int]) -> dict[int, dict]:
    """Load every ``loso_seed{N}.json`` under ``loso_dir`` for the given seeds.

    Raises ``FileNotFoundError`` if any expected file is missing, or
    ``ValueError`` if a JSON is malformed.
    """
    out: dict[int, dict] = {}
    for seed in seeds:
        path = loso_dir / f"loso_seed{seed}.json"
        if not path.exists():
            raise FileNotFoundError(
                f"Expected per-seed LOSO artefact not found: {path}. "
                f"Run ai/scripts/train_fr_regressor_v2_ensemble_loso.py "
                f"first (see ADR-0303)."
            )
        with path.open("r", encoding="utf-8") as fh:
            payload = json.load(fh)
        if "mean_plcc" not in payload:
            raise ValueError(
                f"{path} is missing required 'mean_plcc' key. Schema is "
                f"documented in docs/research/0075-fr-regressor-v2-"
                f"ensemble-prod-flip.md."
            )
        out[seed] = payload
    return out


def evaluate_gate(
    seed_payloads: dict[int, dict],
    mean_plcc_threshold: float,
    plcc_spread_max: float,
    per_seed_min: float,
) -> dict:
    """Apply the two-part gate to the loaded per-seed payloads.

    Returns a structured summary with ``passed: bool`` and the
    individual gate components so callers can render a useful error.
    """
    per_seed_plccs = {seed: float(p["mean_plcc"]) for seed, p in seed_payloads.items()}
    plcc_values = list(per_seed_plccs.values())
    mean_plcc = sum(plcc_values) / len(plcc_values) if plcc_values else 0.0
    spread = max(plcc_values) - min(plcc_values) if plcc_values else 0.0

    failing_seeds = sorted(s for s, v in per_seed_plccs.items() if v < per_seed_min)

    mean_pass = mean_plcc >= mean_plcc_threshold
    spread_pass = spread <= plcc_spread_max
    per_seed_pass = len(failing_seeds) == 0

    passed = mean_pass and spread_pass and per_seed_pass

    return {
        "passed": passed,
        "mean_plcc": mean_plcc,
        "mean_plcc_threshold": mean_plcc_threshold,
        "mean_plcc_pass": mean_pass,
        "plcc_spread": spread,
        "plcc_spread_max": plcc_spread_max,
        "plcc_spread_pass": spread_pass,
        "per_seed_plccs": per_seed_plccs,
        "per_seed_min": per_seed_min,
        "per_seed_pass": per_seed_pass,
        "failing_seeds": failing_seeds,
    }


def _format_human(report: dict) -> str:
    lines = [
        "=== fr_regressor_v2 ensemble production-flip gate (ADR-0303) ===",
        f"Per-seed PLCC: {report['per_seed_plccs']}",
        f"Mean PLCC:    {report['mean_plcc']:.4f}  "
        f"(threshold >= {report['mean_plcc_threshold']:.4f})  "
        f"-> {'PASS' if report['mean_plcc_pass'] else 'FAIL'}",
        f"PLCC spread:  {report['plcc_spread']:.4f}  "
        f"(threshold <= {report['plcc_spread_max']:.4f})  "
        f"-> {'PASS' if report['plcc_spread_pass'] else 'FAIL'}",
        f"Per-seed >= {report['per_seed_min']:.4f}: "
        f"{'PASS' if report['per_seed_pass'] else 'FAIL'}",
    ]
    if report["failing_seeds"]:
        lines.append(f"  Failing seeds: {report['failing_seeds']}")
    lines.append(f"Verdict: {'PASS' if report['passed'] else 'FAIL'}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = build_argparser()
    args = parser.parse_args(argv)

    try:
        seeds = _parse_seed_list(args.seeds)
    except ValueError as exc:
        print(f"[ensemble-gate] error: {exc}", file=sys.stderr)
        return 2

    if not args.loso_dir.exists() or not args.loso_dir.is_dir():
        print(
            f"[ensemble-gate] error: loso_dir not found or not a directory: " f"{args.loso_dir}",
            file=sys.stderr,
        )
        return 2

    try:
        payloads = load_seed_jsons(args.loso_dir, seeds)
    except (FileNotFoundError, ValueError) as exc:
        print(f"[ensemble-gate] error: {exc}", file=sys.stderr)
        return 2

    report = evaluate_gate(
        payloads,
        mean_plcc_threshold=args.mean_plcc_threshold,
        plcc_spread_max=args.plcc_spread_max,
        per_seed_min=args.per_seed_min,
    )

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(_format_human(report))

    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train a local CHUG HDR subjective-MOS head.

CHUG is an HDR MOS corpus, not a KonViD variant. This entry point keeps
the operator-facing command and emitted manifest identity CHUG-specific
while reusing the small MOS-head training loop shared with the committed
KonViD MOS head.
"""

from __future__ import annotations

import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_ai_scripts=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from train_konvid_mos_head import (  # noqa: E402
    FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1,
    FEATURE_SCHEMA_CHUG_HDR_WIDE_V1,
    FEATURE_SCHEMA_KONVID_V1,
)
from train_konvid_mos_head import main as _train_mos_head_main  # noqa: E402

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402

DEFAULT_CHUG_DIR = Path(os.environ.get("VMAF_CHUG_DIR", str(REPO_ROOT / ".corpus" / "chug")))
DEFAULT_CHUG_OUTPUT_DIR = Path(
    os.environ.get("VMAF_CHUG_OUTPUT_DIR", str(REPO_ROOT / ".workingdir2" / "chug"))
)
DEFAULT_SHARD_DIR = DEFAULT_CHUG_DIR / "training" / "fr_canonical_shards" / "output"
DEFAULT_MODEL_ID = "chug_hdr_mos_head_v1"


def _discover_feature_jsonls(shard_dir: Path) -> list[Path]:
    """Return deterministic CHUG feature shards from ``shard_dir``."""
    return sorted(shard_dir.glob("shard_*.features.jsonl"))


def main(argv: Sequence[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(
        prog="train_chug_hdr_mos_head.py",
        description="Train a local CHUG HDR MOS head from reference-aligned feature JSONL.",
    )
    parser.epilog = (
        "Additional MOS-trainer flags such as --epochs, --batch-size, "
        "--k-folds, --seed, and --no-export are forwarded unchanged."
    )
    parser.add_argument(
        "--feature-jsonl",
        type=Path,
        action="append",
        default=[],
        help=(
            "CHUG feature JSONL shard from chug_extract_features.py; may be repeated. "
            "Defaults to shard_*.features.jsonl under --shard-dir."
        ),
    )
    parser.add_argument(
        "--feature-parquet",
        type=Path,
        action="append",
        default=[],
        help=(
            "Metadata-enriched CHUG FULL_FEATURES parquet from a correct "
            "full-reference extraction; may be repeated."
        ),
    )
    parser.add_argument(
        "--shard-dir",
        type=Path,
        default=DEFAULT_SHARD_DIR,
        help="Directory searched for shard_*.features.jsonl when --feature-jsonl is absent.",
    )
    parser.add_argument(
        "--model-id",
        default=DEFAULT_MODEL_ID,
        help="Model id recorded in the emitted manifest.",
    )
    parser.add_argument(
        "--feature-schema",
        choices=(
            FEATURE_SCHEMA_CHUG_HDR_WIDE_V1,
            FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1,
            FEATURE_SCHEMA_KONVID_V1,
        ),
        default=None,
        help=(
            "Feature schema for the local head. The default uses CHUG temporal "
            "quantiles/std plus HDR ladder metadata, or the display-aware "
            "schema when --display-profile-json is supplied; konvid-v1 keeps "
            "the older 11-column baseline for ablation runs."
        ),
    )
    parser.add_argument(
        "--display-profile-json",
        type=Path,
        default=None,
        help=(
            "Optional target-display profile JSON. When set and --feature-schema "
            "is omitted, the wrapper selects chug-hdr-display-v1."
        ),
    )
    parser.add_argument(
        "--out-onnx",
        type=Path,
        default=DEFAULT_CHUG_OUTPUT_DIR / f"{DEFAULT_MODEL_ID}.onnx",
    )
    parser.add_argument(
        "--out-card",
        type=Path,
        default=DEFAULT_CHUG_OUTPUT_DIR / f"{DEFAULT_MODEL_ID}_card.md",
        help="Reserved for the generated/local model card path.",
    )
    parser.add_argument(
        "--out-manifest",
        type=Path,
        default=DEFAULT_CHUG_OUTPUT_DIR / f"{DEFAULT_MODEL_ID}.json",
    )
    args, forwarded = parser.parse_known_args(raw_argv)

    feature_parquets = list(args.feature_parquet)
    feature_jsonls = list(args.feature_jsonl)
    if not feature_jsonls and not feature_parquets:
        feature_jsonls = _discover_feature_jsonls(args.shard_dir)
    if not feature_jsonls and not feature_parquets:
        print(
            f"[chug-hdr-mos] no feature JSONL shards found under {args.shard_dir}; "
            "pass --feature-jsonl/--feature-parquet explicitly or set VMAF_CHUG_DIR.",
            file=sys.stderr,
        )
        return 2
    feature_schema = args.feature_schema
    if feature_schema is None:
        feature_schema = (
            FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1
            if args.display_profile_json is not None
            else FEATURE_SCHEMA_CHUG_HDR_WIDE_V1
        )

    # The shared trainer still has legacy KonViD defaults. Pass explicit
    # impossible paths so a CHUG run cannot accidentally ingest local KoNViD
    # rows alongside the HDR MOS shards.
    delegated: list[str] = [
        "--konvid-1k",
        str(args.shard_dir / "__no_konvid_1k_for_chug__.jsonl"),
        "--konvid-150k",
        str(args.shard_dir / "__no_konvid_150k_for_chug__.jsonl"),
        "--model-id",
        args.model_id,
        "--feature-schema",
        feature_schema,
        "--log-prefix",
        "chug-hdr-mos",
        "--out-onnx",
        str(args.out_onnx),
        "--out-card",
        str(args.out_card),
        "--out-manifest",
        str(args.out_manifest),
        "--run-entrypoint",
        str(SCRIPT_PATH),
        "--run-argv-json",
        json.dumps(raw_argv),
    ]
    for shard in feature_jsonls:
        delegated.extend(["--feature-jsonl", str(shard)])
    for table in feature_parquets:
        delegated.extend(["--feature-parquet", str(table)])
    if args.display_profile_json is not None:
        delegated.extend(["--display-profile-json", str(args.display_profile_json)])
    delegated.extend(forwarded)
    return _train_mos_head_main(delegated)


__all__ = [
    "DEFAULT_CHUG_DIR",
    "DEFAULT_CHUG_OUTPUT_DIR",
    "DEFAULT_MODEL_ID",
    "DEFAULT_SHARD_DIR",
    "FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1",
    "FEATURE_SCHEMA_CHUG_HDR_WIDE_V1",
    "FEATURE_SCHEMA_KONVID_V1",
    "_discover_feature_jsonls",
    "main",
]


if __name__ == "__main__":
    sys.exit(main())

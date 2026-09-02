#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""BVI-DVC → vmaf-tune corpus JSONL adapter (ADR-0310).

Companion to :mod:`ai.scripts.bvi_dvc_to_full_features`. The parquet
script emits a per-frame feature pool consumed by the tiny-AI student
trainers (`vmaf_tiny_v*`, `fr_regressor_v1`). This script takes the
*same* libvmaf JSON cache the parquet stage produced and re-shapes
each `(source, preset, CRF)` triple into the vmaf-tune Phase A corpus
row schema (:data:`vmaftune.CORPUS_ROW_KEYS`) — the input contract
:mod:`ai.scripts.train_fr_regressor_v2` consumes.

Pipeline::

    bvi_dvc_to_full_features.py  (parquet + cached vmaf JSON)
                  │
                  ▼
    bvi_dvc_to_corpus_jsonl.py   (one JSONL row per encode)
                  │
                  ▼
    merge_corpora.py             (concatenate with Netflix shard)

The script is harness-only — it does not invoke ffmpeg or libvmaf.
Heavy GPU / CPU feature extraction stays in
``bvi_dvc_to_full_features.py``; this stage is a pure JSON → JSONL
transform that runs in seconds.

Output: ``runs/bvi_dvc_corpus.jsonl`` (gitignored).
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import math
import sys
import uuid
from pathlib import Path

from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_vmaf_tune_src=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
_REPO_ROOT = _SCRIPT_PATHS.repo_root

from vmaftune import CANONICAL6_FEATURES, CORPUS_ROW_KEYS, SCHEMA_VERSION  # noqa: E402

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def _stable_sha(key: str) -> str:
    """Deterministic 64-hex-char surrogate for ``src_sha256``.

    The cached libvmaf JSON does not preserve the source MP4's content
    hash. Hashing the BVI-DVC clip key (e.g.
    ``DBookcaseBVITexture_480x272_120fps_10bit_420``) yields a stable
    identifier for de-duplication across re-runs without forcing a
    re-decode of the source archive.
    """
    return hashlib.sha256(key.encode("utf-8")).hexdigest()


def _row_from_cache(
    cache_path: Path,
    *,
    crf: int,
    preset: str,
    encoder: str,
    pix_fmt: str,
) -> dict:
    """Build one :data:`CORPUS_ROW_KEYS`-shaped row from a cached vmaf JSON."""
    payload = json.loads(cache_path.read_text())
    pooled = payload.get("pooled_metrics", {}).get("vmaf", {})
    vmaf_score = float(pooled.get("mean", payload["frames"][-1]["metrics"]["vmaf"]))
    frames = payload.get("frames", [])

    key = cache_path.stem
    # Filename pattern: e.g. "DBookcaseBVITexture_480x272_120fps_10bit_420".
    parts = key.split("_")
    if len(parts) >= 3 and "x" in parts[1]:
        width, height = (int(x) for x in parts[1].split("x"))
        fps_token = parts[2]  # e.g. "120fps"
        framerate = float(fps_token.replace("fps", "")) if fps_token.endswith("fps") else 0.0
    else:
        width, height, framerate = 0, 0, 0.0

    duration_s = (len(frames) / framerate) if framerate > 0 else 0.0
    canonical_aggs: dict[str, float] = {}
    pooled_metrics = payload.get("pooled_metrics", {})
    for feature in CANONICAL6_FEATURES:
        pooled_feature = pooled_metrics.get(feature, {})
        canonical_aggs[f"{feature}_mean"] = float(pooled_feature.get("mean", math.nan))
        canonical_aggs[f"{feature}_std"] = float(pooled_feature.get("stddev", math.nan))

    row: dict = {
        "schema_version": SCHEMA_VERSION,
        "run_id": uuid.uuid4().hex,
        "timestamp": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "src": f"bvi-dvc:{key}",
        "src_sha256": _stable_sha(key),
        "width": width,
        "height": height,
        "pix_fmt": pix_fmt,
        "framerate": framerate,
        "duration_s": duration_s,
        "encoder": encoder,
        "encoder_version": "",
        "preset": preset,
        "crf": crf,
        "extra_params": "",
        "encode_path": "",
        "encode_size_bytes": 0,
        "bitrate_kbps": 0.0,
        "encode_time_ms": 0,
        "vmaf_score": vmaf_score,
        "vmaf_model": "vmaf_v0.6.1",
        "score_time_ms": 0,
        "ffmpeg_version": "",
        "vmaf_binary_version": "",
        "exit_status": 0,
        # BVI-DVC clips are processed end-to-end; the parquet
        # pipeline does not slice them via the ADR-0297
        # sample-clip mode, so the corpus row is always "full".
        "clip_mode": "full",
        # The BVI-DVC cache adapter predates HDR/shot/encoder-stat schema
        # columns. Preserve a uniform v3 row with explicit unavailable values.
        "hdr_transfer": "",
        "hdr_primaries": "",
        "hdr_forced": False,
        "shot_count": 0,
        "shot_avg_duration_sec": 0.0,
        "shot_duration_std_sec": 0.0,
        **canonical_aggs,
        "enc_internal_qp_mean": 0.0,
        "enc_internal_qp_std": 0.0,
        "enc_internal_bits_mean": 0.0,
        "enc_internal_bits_std": 0.0,
        "enc_internal_mv_mean": 0.0,
        "enc_internal_mv_std": 0.0,
        "enc_internal_itex_mean": 0.0,
        "enc_internal_ptex_mean": 0.0,
        "enc_internal_intra_ratio": 0.0,
        "enc_internal_skip_ratio": 0.0,
    }
    missing = set(CORPUS_ROW_KEYS) - row.keys()
    assert not missing, f"row missing keys {missing}"
    return row


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(prog="bvi_dvc_to_corpus_jsonl.py")
    ap.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache" / "vmaf-tiny-ai-bvi-dvc-full",
        help="Directory of cached libvmaf JSON files (one per clip).",
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=_REPO_ROOT / "runs" / "bvi_dvc_corpus.jsonl",
    )
    ap.add_argument(
        "--encoder",
        default="libx264",
        help="Encoder label baked into each row's `encoder` field.",
    )
    ap.add_argument("--preset", default="fast")
    ap.add_argument("--crf", type=int, default=35)
    ap.add_argument("--pix-fmt", default="yuv420p10le")
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <output>.manifest.json and "
            "records cache inputs, selected adapter labels, row counts, and exact "
            "CLI args used to build the JSONL."
        ),
    )
    args = ap.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.output.with_suffix(".manifest.json")

    if not args.cache_dir.is_dir():
        print(f"error: cache dir not found: {args.cache_dir}", file=sys.stderr)
        return 2
    caches = sorted(args.cache_dir.glob("*.json"))
    if not caches:
        print(f"error: no .json files under {args.cache_dir}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = 0
    with args.output.open("w", encoding="utf-8") as fp:
        for cache_path in caches:
            row = _row_from_cache(
                cache_path,
                crf=args.crf,
                preset=args.preset,
                encoder=args.encoder,
                pix_fmt=args.pix_fmt,
            )
            fp.write(json.dumps(row, sort_keys=True) + "\n")
            rows += 1
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "bvi-dvc-corpus-jsonl-manifest-v1",
            "row_schema_version": SCHEMA_VERSION,
            "stats": {"cache_files": len(caches), "rows": rows},
            "encoder": args.encoder,
            "preset": args.preset,
            "crf": args.crf,
            "pix_fmt": args.pix_fmt,
            "run_provenance": build_run_provenance(
                entrypoint=SCRIPT_PATH,
                repo_root=_REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={"cache_dir": args.cache_dir},
                outputs={"jsonl": args.output, "manifest": args.manifest_out},
            ),
        },
    )
    print(
        f"[bvi-dvc-jsonl] wrote {rows} rows to {args.output}; manifest {args.manifest_out}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

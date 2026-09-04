#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""KoNViD-1k → VMAF-pair acquisition pipeline.

Takes the raw KoNViD-1k .mp4 sources (acquired via
``ai/scripts/fetch_konvid_1k.py`` to ``$VMAF_DATA_ROOT/konvid-1k/
KoNViD_1k_videos/``) and produces a parquet with the same per-frame
schema the LOSO regressor consumes from ``NetflixFrameDataset``:

    columns: (key, frame_index, vif_scale0..3, adm2, motion2, vmaf)

For each clip, the script:

1. Decodes the .mp4 to YUV (yuv420p, 8-bit) — the reference.
2. Re-encodes via libx264 with a fixed CRF — the distorted variant.
3. Runs the libvmaf CLI on (ref, dis) to extract the 6
   ``vmaf_v0.6.1`` model features + per-frame VMAF teacher score.
4. Appends one parquet row per frame.

Closes the gap from Research-0023 §5: the existing 9-source Netflix
Public corpus is fully utilised by the LOSO sweep; expanding to a
different / larger corpus (KoNViD-1k, BVI-DVC, AOM-CTC) addresses
the FoxBird-class content-distribution variance. KoNViD-1k is
locally available at ``$VMAF_DATA_ROOT/konvid-1k/`` so this is the
natural starting point.

Usage::

    # smoke (5 clips, ~30 s wall):
    python ai/scripts/konvid_to_vmaf_pairs.py --max-clips 5

    # full run (1 200 clips):
    python ai/scripts/konvid_to_vmaf_pairs.py

The output parquet lands at
``ai/data/konvid_vmaf_pairs.parquet`` (gitignored). Re-runs are
idempotent if `--cache-dir` is set — per-clip JSON caches under
``$VMAF_TINY_AI_CACHE/konvid-1k/<key>.json`` are reused.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

import pandas as pd

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
REPO_ROOT = _SCRIPT_PATHS.repo_root

from ai.data.scores import DEFAULT_MODEL, resolve_teacher_model  # noqa: E402

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

# vmaf_v0.6.1 model features — same set the LOSO trainer expects.
DEFAULT_FEATURES = (
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "adm2",
    "motion2",
)


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kw)


def _decode_yuv(src_mp4: Path, out_yuv: Path) -> tuple[int, int, int]:
    """ffmpeg-decode @p src_mp4 to yuv420p; return (w, h, n_frames)."""
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,nb_frames",
            "-of",
            "default=nw=1:nk=1",
            str(src_mp4),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    w, h, n = (int(x) for x in probe.stdout.strip().split("\n"))
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src_mp4),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(out_yuv),
        ]
    )
    return w, h, n


def _encode_dis(src_mp4: Path, out_yuv: Path, crf: int) -> None:
    """Round-trip through libx264 @ @p crf to introduce compression artefacts."""
    intermediate = out_yuv.with_suffix(".dis.mp4")
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src_mp4),
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            str(crf),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "mp4",
            str(intermediate),
        ]
    )
    _run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(intermediate),
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(out_yuv),
        ]
    )
    intermediate.unlink(missing_ok=True)


def _run_vmaf(
    vmaf_bin: Path,
    ref_yuv: Path,
    dis_yuv: Path,
    w: int,
    h: int,
    out_json: Path,
    model: Path | str | None = None,
) -> None:
    """Run libvmaf CLI on (ref_yuv, dis_yuv); auto-loaded model features
    (`vif`, `adm`, `motion`) emit `integer_*` keys in the JSON without
    needing explicit `--feature` flags."""
    resolved = resolve_teacher_model(model)
    _run(
        [
            str(vmaf_bin),
            "--reference",
            str(ref_yuv),
            "--distorted",
            str(dis_yuv),
            "--width",
            str(w),
            "--height",
            str(h),
            "--pixel_format",
            "420",
            "--bitdepth",
            "8",
            "--model",
            resolved.arg,
            "--threads",
            "1",
            "--no_cuda",
            "--no_sycl",
            "--output",
            str(out_json),
            "--json",
            "-q",
        ]
    )


def _lookup(metrics: dict, name: str) -> float:
    for key in (name, f"integer_{name}"):
        val = metrics.get(key)
        if val is not None:
            return float(val)
    prefix = f"integer_{name}_"
    for k, val in metrics.items():
        if k.startswith(prefix) and val is not None:
            return float(val)
    raise KeyError(f"Metric {name!r} not found in frame metrics")


def _frames_to_rows(key: str, vmaf_json: Path, teacher_model: str = DEFAULT_MODEL) -> list[dict]:
    """Extract one (key, frame, teacher_model, *features, vmaf) row per frame from libvmaf JSON."""
    with vmaf_json.open() as f:
        d = json.load(f)
    rows = []
    for fr in d["frames"]:
        m = fr["metrics"]
        row = {
            "key": key,
            "frame_index": fr["frameNum"],
            "teacher_model": teacher_model,
        }
        for feat in DEFAULT_FEATURES:
            row[feat] = _lookup(m, feat)
        row["vmaf"] = float(m["vmaf"])
        rows.append(row)
    return rows


def _process_clip(
    key: str,
    src_mp4: Path,
    vmaf_bin: Path,
    model: Path | str | None,
    crf: int,
    cache_dir: Path | None,
    scratch: Path,
) -> list[dict]:
    resolved = resolve_teacher_model(model)
    if cache_dir is not None:
        cache_path = cache_dir / f"{key}.json"
        if cache_path.is_file():
            with cache_path.open() as f:
                return json.load(f)
    ref_yuv = scratch / f"{key}_ref.yuv"
    dis_yuv = scratch / f"{key}_dis.yuv"
    vmaf_json = scratch / f"{key}_vmaf.json"
    try:
        w, h, _n = _decode_yuv(src_mp4, ref_yuv)
        _encode_dis(src_mp4, dis_yuv, crf)
        _run_vmaf(vmaf_bin, ref_yuv, dis_yuv, w, h, vmaf_json, resolved.arg)
        rows = _frames_to_rows(key, vmaf_json, teacher_model=resolved.name)
    finally:
        for p in (ref_yuv, dis_yuv, vmaf_json):
            p.unlink(missing_ok=True)
    if cache_dir is not None:
        cache_path = cache_dir / f"{key}.json"
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        with cache_path.open("w") as f:
            json.dump(rows, f)
    return rows


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="konvid_to_vmaf_pairs.py",
        description=__doc__,
    )
    ap.add_argument(
        "--konvid-root",
        type=Path,
        default=Path(os.environ.get("VMAF_DATA_ROOT", str(Path.home() / "datasets"))) / "konvid-1k",
        help="KoNViD-1k root (contains KoNViD_1k_videos/).",
    )
    ap.add_argument(
        "--vmaf-bin",
        type=Path,
        default=REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
        help="Path to the vmaf CLI binary.",
    )
    ap.add_argument(
        "--model",
        default=None,
        help="Path or version name for teacher VMAF model (default: single-source default model).",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "ai" / "data" / "konvid_vmaf_pairs.parquet",
        help="Output parquet path.",
    )
    ap.add_argument(
        "--scratch",
        type=Path,
        default=Path(os.environ.get("VMAF_TINY_AI_SCRATCH", "/tmp/konvid_vmaf_pairs_scratch")),
        help=(
            "Scratch directory for intermediate YUV/JSON "
            "(default: $VMAF_TINY_AI_SCRATCH, else /tmp/konvid_vmaf_pairs_scratch)."
        ),
    )
    ap.add_argument(
        "--cache-dir",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_TINY_AI_CACHE",
                str(Path.home() / ".cache" / "vmaf-tiny-ai"),
            )
        )
        / "konvid-1k",
        help="Per-clip JSON cache (set --no-cache to disable).",
    )
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument(
        "--crf",
        type=int,
        default=35,
        help="libx264 CRF for the synthetic distortion (default 35; matches "
        "the Netflix-corpus dis-pair recipe in docs/benchmarks.md).",
    )
    ap.add_argument(
        "--max-clips",
        type=int,
        default=None,
        help="Cap number of clips processed (smoke / dry-run).",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json and "
            "records clip/frame counts, failed clips, cache settings, and exact "
            "CLI args used to build the parquet."
        ),
    )
    args = ap.parse_args(raw_argv)
    if args.manifest_out is None:
        args.manifest_out = args.out.with_suffix(".manifest.json")

    resolved_teacher = resolve_teacher_model(args.model)

    videos_dir = args.konvid_root / "KoNViD_1k_videos"
    if not videos_dir.is_dir():
        print(f"error: KoNViD videos not found at {videos_dir}", file=sys.stderr)
        return 2
    if not args.vmaf_bin.is_file() or not os.access(args.vmaf_bin, os.X_OK):
        print(f"error: libvmaf CLI not executable: {args.vmaf_bin}", file=sys.stderr)
        return 2
    if resolved_teacher.is_path and not Path(resolved_teacher.resolved).is_file():
        print(f"error: model not found at {resolved_teacher.resolved}", file=sys.stderr)
        return 2

    cache_dir = None if args.no_cache else args.cache_dir
    args.scratch.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)

    clips = sorted(videos_dir.glob("*.mp4"))
    if args.max_clips is not None:
        clips = clips[: args.max_clips]
    print(f"[konvid] processing {len(clips)} clips → {args.out}", flush=True)

    all_rows: list[dict] = []
    failed_clips: list[str] = []
    t0 = time.monotonic()
    for i, src_mp4 in enumerate(clips):
        key = f"KoNViD_1k_videos_{src_mp4.stem}"
        try:
            rows = _process_clip(
                key,
                src_mp4,
                args.vmaf_bin,
                resolved_teacher.arg,
                args.crf,
                cache_dir,
                args.scratch,
            )
        except subprocess.CalledProcessError as exc:
            print(f"[konvid] {key} FAILED: {shlex.join(exc.cmd)}", file=sys.stderr)
            failed_clips.append(key)
            continue
        all_rows.extend(rows)
        if (i + 1) % 10 == 0 or i == len(clips) - 1:
            print(
                f"[konvid] {i + 1}/{len(clips)} clips, {len(all_rows)} frames, "
                f"{time.monotonic() - t0:.1f}s",
                flush=True,
            )

    df = pd.DataFrame(all_rows)
    df.to_parquet(args.out, index=False)
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "konvid-vmaf-pairs-manifest-v1",
            "teacher_model": resolved_teacher.name,
            "features": list(DEFAULT_FEATURES),
            "stats": {
                "clips_selected": len(clips),
                "clips_failed": len(failed_clips),
                "clips_processed": len(clips) - len(failed_clips),
                "frames": len(df),
                "elapsed_s": round(time.monotonic() - t0, 6),
            },
            "failed_clips": failed_clips,
            "crf": args.crf,
            "cache_enabled": cache_dir is not None,
            "run_provenance": build_run_provenance(
                entrypoint=Path(__file__),
                repo_root=REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={
                    "konvid_root": args.konvid_root,
                    "videos_dir": videos_dir,
                    "cache_dir": cache_dir,
                    "vmaf_bin": args.vmaf_bin,
                    "model": resolved_teacher.name,
                },
                outputs={"parquet": args.out, "manifest": args.manifest_out},
            ),
        },
    )
    print(
        f"[konvid] wrote {args.out} ({len(df)} frames, {len(clips)} clips); "
        f"manifest {args.manifest_out}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

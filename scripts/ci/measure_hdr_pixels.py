#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Measure whether HDR scores differ across architectures on identical converted pixels.

`dump` runs on x86-64: scores all eight HDR models through the zimg conversion with the preload
shim attached, keeps the converted pictures, and scores them again with the conversion bypassed.
`score` runs on another leg: scores the same pictures with the conversion bypassed and compares
every feature of every frame to x86-64 exactly, then does the same for the full conversion chain.
Each leg also scores one planted picture, and fails when the comparison reports it as exact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

MODEL_GLOB = "hdr_vmaf_v1.1.8*/hdr_vmaf_v1.1.8*.json"
MODEL_COUNT = 8
SIZE_1080 = (1920, 1080)
SIZE_2160 = (3840, 2160)
DOCK_REF = (
    "ref_dock-na-HDR_res2160_rate5p0_maxRate100000_bufSize200000_fps59p940_576x324_1frame.yuv"
)
DOCK_DIS = "dis_dock-na-HDR_res1440_rate15p0_maxRate10000_bufSize20000_fps59p940_576x324_1frame.yuv"
MOTION_REF = "sollevante_ref_1920x1080_8f.yuv"
MOTION_DIS = "sollevante_dis_1920x1080_8f.yuv"
DOCK = (DOCK_REF, DOCK_DIS, 576, 324, 10, "bt2020nc")
MOTION = (MOTION_REF, MOTION_DIS, 1920, 1080, 10, "bt2020nc")
PLANT_OFFSET_BYTES = 2 * (1920 * 540 + 960)
PLANT_XOR = 0x40
TAIL_CHARS = 800


class MeasureError(RuntimeError):
    """The measurement cannot be trusted; the job goes red."""


def size_of(model: str) -> tuple[int, int]:
    """Return the picture size the named model scores at."""
    return SIZE_2160 if "2160" in model else SIZE_1080


def bypass_shape(model: str, directory: Path, prefix: str = "") -> tuple[Any, ...]:
    """Return the input shape of the kept pictures, which need no conversion."""
    width, height = size_of(model)
    ref = directory / f"{prefix}ref_{width}x{height}.yuv"
    dis = directory / f"{prefix}dis_{width}x{height}.yuv"
    return (str(ref), str(dis), width, height, 16, "ictcp")


def color_args(matrix: str) -> list[str]:
    """Return the PQ tags of both inputs."""
    args: list[str] = []
    for side in ("ref", "dist"):
        args += [f"--color_range_{side}", "limited", f"--color_primaries_{side}", "bt2020"]
        args += [f"--color_trc_{side}", "smpte2084", f"--color_matrix_{side}", matrix]
    return args


def score(
    cli: str, shape: tuple[Any, ...], model: str, out: Path, env: dict[str, str] | None = None
) -> list[dict[str, float]]:
    """Score one pair and return the per-frame metrics; a failed run stops the job."""
    ref, dis, width, height, bits, matrix = shape
    argv = [cli, "-r", ref, "-d", dis, "-w", str(width), "-h", str(height), "-p", "420"]
    argv += ["-b", str(bits), *color_args(matrix), "--model", f"version={model}"]
    argv += ["--precision", "max", "--json", "-o", str(out)]
    run_env = {**os.environ, **(env or {})}
    proc = subprocess.run(  # noqa: S603 - fixed argv, the CLI under measurement
        argv, capture_output=True, text=True, env=run_env, check=False
    )
    if proc.returncode != 0:
        raise MeasureError(f"{model}: vmaf rc={proc.returncode}: {proc.stderr[-TAIL_CHARS:]}")
    frames = json.loads(out.read_text(encoding="utf-8"))["frames"]
    if not frames:
        raise MeasureError(f"{model}: vmaf wrote no frames")
    return [frame["metrics"] for frame in frames]


def compare(reference: list[dict[str, float]], measured: list[dict[str, float]]) -> dict[str, Any]:
    """Compare every feature of every frame exactly."""
    if len(reference) != len(measured):
        raise MeasureError(f"frame count {len(measured)} != {len(reference)}")
    differing: set[str] = set()
    worst = 0.0
    for ref_frame, leg_frame in zip(reference, measured, strict=True):
        for key in sorted(set(ref_frame) | set(leg_frame)):
            if key not in ref_frame or key not in leg_frame:
                raise MeasureError(f"feature {key} missing on one side")
            if ref_frame[key] != leg_frame[key]:
                differing.add(key)
                worst = max(worst, abs(ref_frame[key] - leg_frame[key]))
    mean = sum(frame["vmaf"] for frame in measured) / len(measured)
    return {"exact": not differing, "features": sorted(differing), "max_abs": worst, "vmaf": mean}


def sha(path: Path) -> str:
    """Return the sha256 of one file."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def models_of(model_dir: Path) -> list[str]:
    """Return the HDR model names; a different count stops the job."""
    names = sorted(path.stem for path in model_dir.glob(MODEL_GLOB))
    if len(names) != MODEL_COUNT:
        raise MeasureError(f"found {len(names)} HDR models, expected {MODEL_COUNT}: {names}")
    return names


def input_shape(shape: tuple[Any, ...], directory: Path) -> tuple[Any, ...]:
    """Return a shape with its file names resolved in the input directory."""
    return (str(directory / shape[0]), str(directory / shape[1]), *shape[2:])


def dump_one(
    args: argparse.Namespace, model: str, kept: dict[tuple[int, int], tuple[str, str]]
) -> dict[str, list[dict[str, float]]]:
    """Score one model on x86-64 with the shim attached and keep its converted pictures."""
    work = args.dir
    env = {"LD_PRELOAD": str(args.shim.resolve()), "ZDUMP": str(work / "x")}
    for name in ("x.0.yuv", "x.1.yuv"):
        (work / name).unlink(missing_ok=True)
    full = score(args.cli, input_shape(DOCK, work), model, work / "o.json", env)
    width, height = size_of(model)
    digests = (sha(work / "x.0.yuv"), sha(work / "x.1.yuv"))
    if (width, height) not in kept:
        kept[(width, height)] = digests
        shutil.copy(work / "x.0.yuv", work / f"ref_{width}x{height}.yuv")
        shutil.copy(work / "x.1.yuv", work / f"dis_{width}x{height}.yuv")
    if kept[(width, height)] != digests:
        raise MeasureError(
            f"{model}: converted pictures differ from the other {width}x{height} models"
        )
    bypass = score(args.cli, bypass_shape(model, work), model, work / "o.json")
    control = compare(full, bypass)
    print(f"x86-64 {model} control bypass-vs-full exact={control['exact']} vmaf={control['vmaf']}")
    if not control["exact"]:
        raise MeasureError(
            f"{model}: bypass differs from the full chain on x86-64, the method does not hold"
        )
    motion = score(args.cli, input_shape(MOTION, work), model, work / "o.json")
    return {"full": full, "bypass": bypass, "motion": motion}


def cmd_dump(args: argparse.Namespace) -> int:
    """Write the x86-64 reference file and the converted pictures."""
    kept: dict[tuple[int, int], tuple[str, str]] = {}
    reference = {model: dump_one(args, model, kept) for model in models_of(args.models)}
    (args.dir / "x86.json").write_text(json.dumps(reference), encoding="utf-8")
    for name in ("x.0.yuv", "x.1.yuv", "o.json"):
        (args.dir / name).unlink(missing_ok=True)
    print(f"x86-64 wrote {len(reference)} models")
    return 0


def plant(args: argparse.Namespace, model: str) -> None:
    """Fail unless the comparison reports one changed sample in the distorted picture."""
    work = args.dir
    width, height = size_of(model)
    shutil.copy(work / f"ref_{width}x{height}.yuv", args.out.parent / f"p_ref_{width}x{height}.yuv")
    data = bytearray((work / f"dis_{width}x{height}.yuv").read_bytes())
    data[PLANT_OFFSET_BYTES + 1] ^= PLANT_XOR
    (args.out.parent / f"p_dis_{width}x{height}.yuv").write_bytes(data)
    shape = bypass_shape(model, args.out.parent, "p_")
    measured = score(args.cli, shape, model, args.out.parent / "o.json")
    row = compare(
        json.loads((work / "x86.json").read_text(encoding="utf-8"))[model]["bypass"], measured
    )
    print(f"{args.leg} PLANT {model} exact={row['exact']} features={len(row['features'])}")
    if row["exact"]:
        raise MeasureError("planted picture compared exact, the comparison does not hold")


def print_row(
    leg: str, kind: str, model: str, row: dict[str, Any], reference: list[dict[str, float]]
) -> None:
    """Print one comparison row."""
    mean = sum(frame["vmaf"] for frame in reference) / len(reference)
    word = "exact" if row["exact"] else "differs"
    print(
        f"{leg} {kind} {model} {word} features={len(row['features'])} "
        f"max_abs={row['max_abs']:.3e} vmaf_x86={mean:.17g} vmaf_leg={row['vmaf']:.17g}"
    )


def cmd_score(args: argparse.Namespace) -> int:
    """Compare this leg to x86-64 on identical pictures and through the full chain."""
    work = args.dir
    reference = json.loads((work / "x86.json").read_text(encoding="utf-8"))
    models = models_of(args.models)
    out = args.out.parent / "o.json"
    rows: dict[str, dict[str, Any]] = {}
    for model in models:
        rows[model] = {}
        cases = (
            ("bypass", bypass_shape(model, work)),
            ("full", input_shape(DOCK, work)),
            ("motion", input_shape(MOTION, work)),
        )
        for kind, shape in cases:
            row = compare(reference[model][kind], score(args.cli, shape, model, out))
            rows[model][kind] = row
            print_row(args.leg, kind, model, row, reference[model][kind])
    plant(args, models[0])
    args.out.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    for kind in ("bypass", "full", "motion"):
        exact = sum(rows[model][kind]["exact"] for model in models)
        print(f"{args.leg} SUMMARY {kind} exact {exact} of {len(models)}")
    return 0


def main() -> int:
    """Parse the command line and run one mode."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("dump", "score"))
    parser.add_argument("--cli", required=True)
    parser.add_argument("--dir", type=Path, required=True, help="pictures and x86.json")
    parser.add_argument("--models", type=Path, default=Path("model"))
    parser.add_argument("--shim", type=Path, help="preload shim, dump mode")
    parser.add_argument("--leg", default="leg", help="leg name, score mode")
    parser.add_argument("--out", type=Path, help="result file, score mode")
    args = parser.parse_args()
    try:
        return cmd_dump(args) if args.mode == "dump" else cmd_score(args)
    except MeasureError as error:
        print(f"measure_hdr_pixels: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

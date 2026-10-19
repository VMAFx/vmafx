#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Differential harness of the Rust feature-extractor twins (ADR-1713).

Every cell runs the same ``vmaf`` binary twice, with ``VMAF_FEATURE_IMPL=c``
and with ``VMAF_FEATURE_IMPL=rust``, at ``--precision max --json``, and fails
unless both runs have the same frames, the same metric names on every frame,
equal IEEE doubles for every value (NaN equals NaN), and the JSON receipt
(``feature_backends``) names the C extractor in the first run and its Rust
twin (``<name>_rust``) in the second. No tolerance exists: the contract is
bit identity.

Cells are derived, not listed: for each extractor, the default options plus
every distinct ``feature_opts_dicts`` entry the ``vmaf_v1.0.16`` models give
it, plus, for two entries that differ only in ``adm_norm_view_dist``, the
first with the second's distance as ``adm_norm_view_dist_extra`` (ADR-2795);
with ``--models``, each of those models scored end to end.

Exit status: 0 all cells equal (or refused by both sides with the same
status), 1 a mismatch, 2 a run or fixture failure.
A missing fixture fails unless ``--skip-missing``; ``--skip-missing-exit N``
also returns ``N`` when every cell was skipped (Meson's 77).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import re
import struct
import sys
import tempfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.cross_backend_parity_gate import load_frames
from scripts.lib.safe_subprocess import run as run_command

REPO = Path(__file__).resolve().parents[2]
MODEL_GLOB = "model/vmaf_v1.0.16*/*.json"
TWIN_SOURCES = "core/src/rust/feature/*/src/lib.rs"
TWIN_PATTERN = re.compile(r'twin::<\w+>\(c"([a-z0-9_]+)",\s*c"([a-z0-9_]+)"\)')
RUN_TIMEOUT_SECONDS = 3600
MAX_REPORTED_DIFFS = 10

# Model feature name -> the C extractor that writes it. A v1.0.16 model
# naming a feature missing here fails the run instead of being dropped.
MODEL_FEATURE_EXTRACTOR = {
    "Cambi_feature_cambi_score": "cambi",
    "Speed_chroma_feature_speed_chroma_uv_score": "speed_chroma",
    "VMAF_integer_feature_adm3_score": "adm",
    "VMAF_integer_feature_motion3_score": "motion",
}


@dataclasses.dataclass(frozen=True)
class Fixture:
    key: str
    ref: str
    dist: str
    width: int
    height: int
    bitdepth: int
    pix_fmt: str = "420"


FIXTURES = {
    f.key: f
    for f in (
        Fixture(
            "netflix",
            "python/test/resource/yuv/src01_hrc00_576x324.yuv",
            "python/test/resource/yuv/src01_hrc01_576x324.yuv",
            576,
            324,
            8,
        ),
        Fixture(
            "checker1",
            "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
            "python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv",
            1920,
            1080,
            8,
        ),
        Fixture(
            "checker10",
            "python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
            "python/test/resource/yuv/checkerboard_1920_1080_10_3_10_0.yuv",
            1920,
            1080,
            8,
        ),
        Fixture(
            "bbb4k",
            "testdata/bbb/ref_3840x2160_200f.yuv",
            "testdata/bbb/dis_3840x2160_200f.yuv",
            3840,
            2160,
            8,
        ),
        Fixture(
            "sparks10",
            "python/test/resource/yuv/sparks_ref_480x270.yuv42010le.yuv",
            "python/test/resource/yuv/sparks_dis_480x270.yuv42010le.yuv",
            480,
            270,
            10,
        ),
    )
}


@dataclasses.dataclass(frozen=True)
class Cell:
    """One comparison: an extractor with options, or a whole model."""

    label: str
    fixture: Fixture
    feature_arg: str | None = None  # `--feature` value; None for a model cell
    model_path: str | None = None
    c_name: str | None = None
    threads: int = 0


@dataclasses.dataclass
class Outcome:
    cell: Cell
    status: str  # "equal", "refused", "mismatch", "error", "skipped"
    frames: int = 0
    detail: list[str] = dataclasses.field(default_factory=list)


def discover_twins(root: Path) -> dict[str, str]:
    """C name -> Rust twin name, from the `TWINS` of every lane crate."""

    twins: dict[str, str] = {}
    for src in sorted(root.glob(TWIN_SOURCES)):
        for c_name, rust_name in TWIN_PATTERN.findall(src.read_text(encoding="utf-8")):
            twins[c_name] = rust_name
    return twins


def model_files(root: Path) -> list[Path]:
    return sorted(root.glob(MODEL_GLOB))


def model_option_sets(root: Path) -> dict[str, list[dict[str, Any]]]:
    """Extractor -> distinct option dicts of every v1.0.16 model."""

    sets: dict[str, list[dict[str, Any]]] = {}
    for path in model_files(root):
        model = json.loads(path.read_text(encoding="utf-8"))["model_dict"]
        for name, opts in zip(model["feature_names"], model["feature_opts_dicts"], strict=True):
            if name not in MODEL_FEATURE_EXTRACTOR:
                raise ValueError(
                    f"{path}: feature {name} has no extractor in MODEL_FEATURE_EXTRACTOR"
                )
            bucket = sets.setdefault(MODEL_FEATURE_EXTRACTOR[name], [])
            if opts not in bucket:
                bucket.append(opts)
    return sets


VIEW_DIST = "adm_norm_view_dist"
VIEW_DIST_EXTRA = "adm_norm_view_dist_extra"


def view_distance_variants(option_sets: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """One second-distance variant per pair of option dicts that differ only in
    the viewing distance (ADR-2795): the first dict, with the second's distance
    as ``adm_norm_view_dist_extra``. Ordered as the model option sets are."""

    variants: list[dict[str, Any]] = []
    for i, first in enumerate(option_sets):
        for second in option_sets[i + 1 :]:
            if VIEW_DIST not in first or VIEW_DIST not in second:
                continue
            rest_first = {k: v for k, v in first.items() if k != VIEW_DIST}
            rest_second = {k: v for k, v in second.items() if k != VIEW_DIST}
            if rest_first != rest_second or first[VIEW_DIST] == second[VIEW_DIST]:
                continue
            variants.append({**first, VIEW_DIST_EXTRA: second[VIEW_DIST]})
    return variants


def option_value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def feature_arg(c_name: str, opts: dict[str, Any]) -> str:
    if not opts:
        return c_name
    return c_name + "=" + ":".join(f"{k}={option_value(v)}" for k, v in opts.items())


def build_cells(args: argparse.Namespace, root: Path) -> list[Cell]:
    fixtures = [FIXTURES[k] for k in args.fixtures]
    cells: list[Cell] = []
    option_sets = model_option_sets(root)
    for c_name in args.features:
        model_sets = [o for o in option_sets.get(c_name, []) if o]
        variants: list[dict[str, Any]] = [{}, *model_sets, *view_distance_variants(model_sets)]
        for i, opts in enumerate(variants):
            for fx in fixtures:
                for t in args.threads:
                    label = f"{c_name}#{i} {fx.key} t{t}"
                    cells.append(Cell(label, fx, feature_arg(c_name, opts), None, c_name, t))
    if args.models:
        for path in model_files(root):
            for fx in fixtures:
                for t in args.threads:
                    cells.append(
                        Cell(f"model {path.stem} {fx.key} t{t}", fx, None, str(path), None, t)
                    )
    return cells


def frame_limit(args: argparse.Namespace, fx: Fixture) -> int | None:
    if fx.key == "bbb4k" and args.bbb_frames:
        return int(args.bbb_frames)
    return int(args.frames) if args.frames else None


def vmaf_command(args: argparse.Namespace, cell: Cell, root: Path, out: Path) -> list[str]:
    fx = cell.fixture
    cmd = [
        str(args.vmaf),
        "--reference",
        str(root / fx.ref),
        "--distorted",
        str(root / fx.dist),
        "--width",
        str(fx.width),
        "--height",
        str(fx.height),
        "--pixel_format",
        fx.pix_fmt,
        "--bitdepth",
        str(fx.bitdepth),
        "--precision",
        "max",
        "--json",
        "--output",
        str(out),
    ]
    if cell.model_path:
        cmd += ["--model", f"path={cell.model_path}"]
    else:
        cmd += ["--feature", str(cell.feature_arg), "--no_prediction"]
    cmd += ["--threads", str(cell.threads)]
    limit = frame_limit(args, fx)
    if limit:
        cmd += ["--frame_cnt", str(limit)]
    return cmd


def run_side(
    args: argparse.Namespace, cell: Cell, root: Path, impl: str, out: Path
) -> tuple[int, str]:
    """Run one side; return its exit status and, on failure, the error text."""

    env = dict(os.environ)
    env["VMAF_FEATURE_IMPL"] = impl
    proc = run_command(
        vmaf_command(args, cell, root, out),
        allowed_executables=(str(args.vmaf),),
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=RUN_TIMEOUT_SECONDS,
        max_output_bytes=16 * 1_048_576,
    )
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout)[-2000:]
        return proc.returncode, f"{impl}: vmaf exited {proc.returncode}: {tail}"
    return 0, ""


def same_double(a: Any, b: Any) -> bool:
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return type(a) is type(b) and a == b


def ulp_distance(a: float, b: float) -> int:
    def key(x: float) -> int:
        i = int(struct.unpack("<q", struct.pack("<d", x))[0])
        return i if i >= 0 else -(1 << 63) - i

    return abs(key(a) - key(b))


def describe(frame: int, metric: str, a: Any, b: Any) -> str:
    text = f"frame {frame} {metric}: C {a!r} Rust {b!r}"
    if isinstance(a, float) and isinstance(b, float) and math.isfinite(a) and math.isfinite(b):
        text = f"frame {frame} {metric}: C {a:.17g} Rust {b:.17g} ({ulp_distance(a, b)} ulp)"
    return text


def diff_runs(c_frames: list[dict[str, Any]], r_frames: list[dict[str, Any]]) -> list[str]:
    if len(c_frames) != len(r_frames):
        return [f"frame count differs: C {len(c_frames)} Rust {len(r_frames)}"]
    diffs: list[str] = []
    for i, (fc, fr) in enumerate(zip(c_frames, r_frames, strict=True)):
        mc, mr = fc.get("metrics", {}), fr.get("metrics", {})
        if set(mc) != set(mr):
            diffs.append(
                f"frame {i}: metric names differ: only C {sorted(set(mc) - set(mr))}, "
                f"only Rust {sorted(set(mr) - set(mc))}"
            )
            continue
        diffs += [describe(i, m, mc[m], mr[m]) for m in sorted(mc) if not same_double(mc[m], mr[m])]
    return diffs


def extractors_used(path: Path) -> list[str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [str(e.get("extractor")) for e in payload.get("feature_backends", [])]


def receipt_errors(cell: Cell, twins: dict[str, str], c_out: Path, r_out: Path) -> list[str]:
    """The receipt must show which implementation ran on each side."""

    c_used, r_used = extractors_used(c_out), extractors_used(r_out)
    errors = [f"C run used Rust twins {n}" for n in c_used if n.endswith("_rust")]
    expected = [cell.c_name] if cell.c_name else [c for c in twins if c in c_used]
    for c_name in expected:
        rust_name = twins.get(str(c_name))
        if not rust_name:
            errors.append(f"no Rust twin of {c_name} is registered")
        elif rust_name not in r_used:
            errors.append(f"Rust run did not use {rust_name} (used {r_used})")
    return errors


def refusal_outcome(cell: Cell, c_rc: int, c_err: str, r_rc: int, r_err: str) -> Outcome:
    """Both sides refusing the input with the same status is parity; anything else is not."""

    if c_rc and c_rc == r_rc:
        return Outcome(cell, "refused", detail=[f"both refuse (exit {c_rc}): {c_err[-300:]}"])
    return Outcome(cell, "error", detail=[e for e in (c_err, r_err) if e])


def compare_cell(
    args: argparse.Namespace, cell: Cell, root: Path, twins: dict[str, str]
) -> Outcome:
    fx = cell.fixture
    if not (root / fx.ref).is_file() or not (root / fx.dist).is_file():
        status = "skipped" if args.skip_missing else "error"
        return Outcome(cell, status, detail=[f"fixture {fx.key} missing ({fx.ref}, {fx.dist})"])
    with tempfile.TemporaryDirectory(prefix="rust-twin-diff-") as tmp:
        c_out, r_out = Path(tmp) / "c.json", Path(tmp) / "rust.json"
        c_rc, c_err = run_side(args, cell, root, "c", c_out)
        r_rc, r_err = run_side(args, cell, root, "rust", r_out)
        if c_rc or r_rc:
            return refusal_outcome(cell, c_rc, c_err, r_rc, r_err)
        c_frames, r_frames = load_frames(c_out), load_frames(r_out)
        problems = receipt_errors(cell, twins, c_out, r_out) + diff_runs(c_frames, r_frames)
    status = "mismatch" if problems else "equal"
    return Outcome(cell, status, frames=len(c_frames), detail=problems[:MAX_REPORTED_DIFFS])


def report(outcomes: list[Outcome]) -> None:
    for o in outcomes:
        frames = f"{o.frames} frames" if o.frames else ""
        print(f"{o.status.upper():9} {o.cell.label:40} {frames}")
        for line in o.detail:
            print(f"          {line}")


def exit_status(args: argparse.Namespace, outcomes: list[Outcome]) -> int:
    statuses = {o.status for o in outcomes}
    if "error" in statuses:
        return 2
    if "mismatch" in statuses:
        return 1
    if outcomes and statuses == {"skipped"} and args.skip_missing_exit is not None:
        return int(args.skip_missing_exit)
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--vmaf", type=Path, required=True, help="vmaf binary of a Rust build")
    ap.add_argument("--repo-root", type=Path, default=REPO)
    ap.add_argument(
        "--feature",
        action="append",
        default=[],
        dest="features",
        help="C extractor name (repeatable)",
    )
    ap.add_argument("--all-twins", action="store_true", help="every twin the lane crates register")
    ap.add_argument("--models", action="store_true", help="also score every vmaf_v1.0.16 model")
    ap.add_argument(
        "--fixtures",
        default="netflix,checker1,checker10,sparks10,bbb4k",
        type=lambda s: [k for k in s.split(",") if k],
        help=f"of {sorted(FIXTURES)}",
    )
    ap.add_argument("--frames", type=int, default=None, help="limit every fixture to N frames")
    ap.add_argument("--bbb-frames", type=int, default=None, help="limit bbb4k to N frames")
    ap.add_argument(
        "--threads",
        default="0,1,4",
        type=lambda s: [int(t) for t in s.split(",") if t],
        help="vmaf --threads values, one cell each (0 = serial; 1+ = the thread pool, "
        "where non-temporal flushes run on the shared context)",
    )
    ap.add_argument("--skip-missing", action="store_true")
    ap.add_argument("--skip-missing-exit", type=int, default=None)
    args = ap.parse_args(argv)
    if args.skip_missing_exit is not None:
        args.skip_missing = True
    unknown = [k for k in args.fixtures if k not in FIXTURES]
    if unknown:
        ap.error(f"unknown fixtures {unknown}")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.vmaf = args.vmaf.resolve()
    root = args.repo_root.resolve()
    twins = discover_twins(root)
    if args.all_twins:
        args.features = sorted(set(args.features) | set(twins))
    if not args.features and not args.models:
        print(
            "rust_twin_diff: nothing to compare (--feature, --all-twins or --models)",
            file=sys.stderr,
        )
        return 2
    outcomes = [compare_cell(args, cell, root, twins) for cell in build_cells(args, root)]
    report(outcomes)
    return exit_status(args, outcomes)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

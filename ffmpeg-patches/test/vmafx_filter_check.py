#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Checks of the FFmpeg `vmafx` filter against the `vmaf` CLI (RC4 WP9).

Subcommands, each exiting 0 on success, 1 on a failure and 77 when a
fixture is missing:

- ``parity``: the filter and the CLI score the same pair; every per-frame
  metric, every pooled and aggregate value of the two JSON reports must be
  the same double (both written at ``%.17g``). ``--backend cuda`` uploads
  both inputs with ``hwupload`` to one CUDA device and compares with ``vmaf --backend
  cuda`` of the same build: the filter imports the CUDA frames without a
  copy and scores on the CUDA twins.
- ``windows``: ``n_stats`` / ``n_stats_frames`` windows written to
  ``stats_path`` equal the CLI's per-frame scores pooled over the same
  frames with the engine's arithmetic (scripts/ci/vmafx_window_pooling.py);
  the last window is marked partial when the stream ends inside it.
- ``provenance``: the report carries the provenance record and the log its
  init line.
- ``refusal``: a CPU-only extractor on CUDA frames fails the graph with the
  import rule's message naming the backend, the input and the extractor,
  and no `VMAF score` line follows (D8).

Pairs: the Netflix 576x324 pair, both 1080p checkerboard pairs, the 4K BBB
pair (first ``--frames-4k`` frames).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "ci"))
from vmafx_window_pooling import pooled_by_name  # noqa: E402 -- path set above

SKIP = 77


@dataclass(frozen=True)
class Pair:
    name: str
    ref: str
    dist: str
    width: int
    height: int
    frames: int  # 0: every frame


PAIRS = {
    "golden": Pair("golden", "src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, 0),
    "checkerboard-1": Pair(
        "checkerboard-1",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        0,
    ),
    "checkerboard-10": Pair(
        "checkerboard-10",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        0,
    ),
    "bbb-4k": Pair(
        "bbb-4k", "bbb/ref_3840x2160_200f.yuv", "bbb/dis_3840x2160_200f.yuv", 3840, 2160, 0
    ),
}


def fixture(args: argparse.Namespace, name: str) -> Path:
    if name.startswith("bbb/"):
        return Path(args.bbb) / name[len("bbb/") :]
    return Path(args.yuv) / name


def run(
    cmd: list[str], env: dict[str, str], check: bool = True
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(  # noqa: S603 -- binaries under test, argument list, no shell
        cmd, env=env, capture_output=True, text=True, timeout=1800, check=False
    )
    if check and result.returncode != 0:
        sys.stderr.write(result.stdout[-4000:] + result.stderr[-4000:])
        raise SystemExit(f"{Path(cmd[0]).name} exited {result.returncode}")
    return result


def environment(args: argparse.Namespace) -> dict[str, str]:
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = args.libdir + ":" + env.get("LD_LIBRARY_PATH", "")
    return env


def frame_limit(args: argparse.Namespace, pair: Pair) -> int:
    return args.frames_4k if pair.name == "bbb-4k" else pair.frames


def cli_report(args: argparse.Namespace, pair: Pair, out: Path, backend: str) -> dict:
    cmd = [args.vmaf, "-r", str(fixture(args, pair.ref)), "-d", str(fixture(args, pair.dist))]
    cmd += ["-w", str(pair.width), "-h", str(pair.height), "-p", "420", "-b", "8"]
    cmd += ["--precision", "max", "--json", "-o", str(out), "-q", "--backend", backend]
    cmd += ["--model", args.model] if args.model else []
    limit = frame_limit(args, pair)
    cmd += ["--frame_cnt", str(limit)] if limit else []
    run(cmd, environment(args))
    return json.loads(out.read_text(encoding="utf-8"))


def ffmpeg_inputs(args: argparse.Namespace, pair: Pair) -> list[str]:
    size = f"{pair.width}x{pair.height}"
    raw = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", "24", "-i"]
    return [*raw, str(fixture(args, pair.dist)), *raw, str(fixture(args, pair.ref))]


def filter_graph(backend: str, options: str, limit: int) -> str:
    """Both inputs cut to `limit` frames (0: all), uploaded for CUDA."""
    cut = f"trim=end_frame={limit}," if limit else ""
    up = "hwupload," if backend == "cuda" else ""
    chains = f"[0:v]{cut}{up}null[d];[1:v]{cut}{up}null[r]"
    return f"{chains};[d][r]vmafx={options}"


def run_filter(
    args: argparse.Namespace, pair: Pair, backend: str, options: str, check: bool = True
) -> subprocess.CompletedProcess[str]:
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "info"]
    cmd += ["-init_hw_device", "cuda=cu:0", "-filter_hw_device", "cu"] if backend == "cuda" else []
    cmd += ffmpeg_inputs(args, pair)
    model = f"model={args.model}:" if args.model else ""
    graph = filter_graph(backend, model + options, frame_limit(args, pair))
    cmd += ["-lavfi", graph, "-f", "null", "-"]
    return run(cmd, environment(args), check)


def escape(path: Path) -> str:
    """A path as a filter option value (`:` and `\\` escaped)."""
    return str(path).replace("\\", "\\\\").replace(":", "\\:")


def compare(cli: dict, filt: dict) -> tuple[int, int, float, list[str]]:
    """Values compared, identical, max abs difference, mismatching keys."""
    total = same = 0
    worst = 0.0
    bad: list[str] = []
    if len(cli["frames"]) != len(filt["frames"]):
        return 0, 0, float("inf"), ["frame count"]
    for a, b in zip(cli["frames"], filt["frames"], strict=True):
        for key, value in a["metrics"].items():
            total += 1
            other = b["metrics"].get(key)
            if other == value:
                same += 1
            else:
                bad.append(f"frame {a['frameNum']} {key}")
                worst = max(worst, abs(other - value)) if other is not None else float("inf")
    for section in ("pooled_metrics", "aggregate_metrics"):
        total += 1
        if cli.get(section) == filt.get(section):
            same += 1
        else:
            bad.append(section)
    return total, same, worst, bad


def cmd_parity(args: argparse.Namespace) -> int:
    rows = []
    failed = False
    for name in args.pairs.split(","):
        pair = PAIRS[name]
        if not fixture(args, pair.ref).is_file() or not fixture(args, pair.dist).is_file():
            print(f"skip {name}: fixtures missing")
            return SKIP
        with tempfile.TemporaryDirectory() as tmp:
            cli = cli_report(args, pair, Path(tmp) / "cli.json", args.backend)
            report = Path(tmp) / "filter.json"
            run_filter(args, pair, args.backend, f"log_path={escape(report)}:score_fmt=%.17g")
            filt = json.loads(report.read_text(encoding="utf-8"))
        total, same, worst, bad = compare(cli, filt)
        failed |= bool(bad) or total == 0
        rows.append(
            f"| {name} | {args.backend} | {len(cli['frames'])} | {total} | {same} | {worst:g} |"
        )
        for item in bad[:5]:
            print(f"MISMATCH {name}: {item}")
    print("| pair | backend | frames | values | identical | max abs diff |")
    print("| --- | --- | --- | --- | --- | --- |")
    print("\n".join(rows))
    return 1 if failed else 0


def check_windows(lines: list[dict], frames: list[dict], model: str) -> tuple[int, list[str]]:
    checked = 0
    bad: list[str] = []
    for window in lines:
        first, last = window["first"], window["last"]
        scores = [frame["metrics"][model] for frame in frames[first : last + 1]]
        if len(scores) != window["n_frames"]:
            bad.append(
                f"window {window['window']}: {len(scores)} frames, {window['n_frames']} listed"
            )
            continue
        expected = pooled_by_name(scores)
        for pool, value in window[model].items():
            checked += 1
            if value != expected[pool]:
                bad.append(f"window {window['window']} {pool}: {value!r} != {expected[pool]!r}")
    return checked, bad


def cmd_windows(args: argparse.Namespace) -> int:
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    pools = "min+max+mean+harmonic_mean+median+perc5+perc10+perc20"
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        cli = cli_report(args, pair, Path(tmp) / "cli.json", args.backend)
        for spec, expect_partial in (("n_stats_frames=10", True), ("n_stats=0.5", True)):
            stats = Path(tmp) / "stats.ndjson"
            options = f"{spec}:pool={pools}:stats_out=log+file:stats_path={escape(stats)}"
            run_filter(args, pair, args.backend, options + ":score_fmt=%.17g")
            lines = [json.loads(x) for x in stats.read_text(encoding="utf-8").splitlines()]
            checked, bad = check_windows(lines, cli["frames"], "vmaf")
            partial = [w["window"] for w in lines if w["partial"]]
            covered = sum(w["n_frames"] for w in lines) == len(cli["frames"])
            ok = not bad and checked == 8 * len(lines) and covered
            ok &= partial == ([lines[-1]["window"]] if expect_partial else [])
            failed |= not ok
            print(
                f"{spec}: {len(lines)} windows, {checked} pooled values equal the CLI's frames"
                f" ({len(bad)} differ), partial windows {partial}, every frame in one window: {covered}"
            )
            for item in bad[:5]:
                print(f"MISMATCH {item}")
    return 1 if failed else 0


def cmd_provenance(args: argparse.Namespace) -> int:
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    with tempfile.TemporaryDirectory() as tmp:
        report = Path(tmp) / "filter.json"
        result = run_filter(args, pair, args.backend, f"log_path={escape(report)}")
        record = json.loads(report.read_text(encoding="utf-8")).get("provenance", {})
    logged = "vmafx provenance: {" in result.stderr
    fields = all(k in record for k in ("build_id", "models", "features", "digest"))
    model_hash = bool(record.get("models")) and all(m.get("sha256") for m in record["models"])
    print(
        f"provenance in the report: {bool(record)} (build_id, models, features, digest: {fields};"
        f" model hashes: {model_hash}); logged at init: {logged};"
        f" backend {record.get('active_backend')}"
    )
    return 0 if record and fields and model_hash and logged else 1


def cmd_refusal(args: argparse.Namespace) -> int:
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    result = run_filter(args, pair, "cuda", "feature=delta_e_itp", check=False)
    text = result.stderr
    named = "delta_e_itp (cpu" in text and any(
        f"{pad}: backend cuda" in text for pad in ("main", "reference")
    )
    no_score = "VMAF score" not in text
    print(
        f"exit {result.returncode}; backend, input and extractor named: {named};"
        f" no score line: {no_score}"
    )
    lines = [x for x in text.splitlines() if "delta_e_itp" in x]
    print(lines[0][:400] if lines else text[-400:])
    return 0 if result.returncode != 0 and named and no_score else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("parity", "windows", "provenance", "refusal"))
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--vmaf", required=True)
    parser.add_argument("--libdir", required=True, help="directory of libvmafx.so.1")
    parser.add_argument("--yuv", default=str(ROOT / "python" / "test" / "resource" / "yuv"))
    parser.add_argument("--bbb", default=str(ROOT / "testdata" / "bbb"))
    parser.add_argument("--backend", default="cpu", choices=("cpu", "cuda"))
    parser.add_argument("--pairs", default="golden,checkerboard-1,checkerboard-10,bbb-4k")
    parser.add_argument("--frames-4k", type=int, default=30)
    parser.add_argument("--model", default="", help="a model spec; empty: the library default")
    args = parser.parse_args()
    return {
        "parity": cmd_parity,
        "windows": cmd_windows,
        "provenance": cmd_provenance,
        "refusal": cmd_refusal,
    }[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())

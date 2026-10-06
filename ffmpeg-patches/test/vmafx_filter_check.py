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
  copy and scores on the CUDA twins. ``--backend cuda-host`` feeds software
  frames to ``backend=cuda``: the context on the GPU uploads them.
- ``windows``: ``n_stats`` / ``n_stats_frames`` windows written to
  ``stats_path`` equal the CLI's per-frame scores pooled over the same
  frames with the engine's arithmetic (scripts/ci/vmafx_window_pooling.py);
  the last window is marked partial when the stream ends inside it.
- ``provenance``: the report carries the provenance record and the log its
  init line.
- ``refusal``: a CPU-only extractor on CUDA frames fails the graph with the
  import rule's message naming the backend, the input and the extractor,
  and no `VMAF score` line follows (D8).
- ``legacy``: ``vmafx_pre`` writes the same bytes as ``vmaf_pre`` (8-bit and
  10-bit planes through the blur fixture ``pre_blur_4x4.onnx``, which changes
  every sample) and ``vmafx_tune`` logs the same recommendation as
  ``libvmaf_tune`` (model pinned to ``vmaf_v0.6.1``, two targets). Needs an
  FFmpeg configured with both ``--enable-libvmafx`` and ``--enable-libvmaf``.
- ``pool``: both inputs uploaded to a VAAPI device into fixed frame pools;
  vmafx downloads them (``import=host``) and with ``metadata=1`` holds the
  main frames until their scores are final. A pool one frame smaller than
  ``vmafx_context_max_in_flight() + 1`` is refused by name before any frame;
  a pool of exactly that size scores the pair as the CLI does.

Pairs: the Netflix 576x324 pair, both 1080p checkerboard pairs, the 4K BBB
pair (first ``--frames-4k`` frames).
"""

from __future__ import annotations

import argparse
import json
import os
import re
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


def cli_backend(args: argparse.Namespace) -> str:
    """The CLI backend a filter run is compared with (cuda-host scores on CUDA)."""
    return "cpu" if args.backend == "cpu" else "cuda"


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
    """Both inputs cut to `limit` frames (0: all), uploaded for CUDA; software
    frames to a CUDA context for cuda-host."""
    cut = f"trim=end_frame={limit}," if limit else ""
    up = "hwupload," if backend == "cuda" else ""
    chains = f"[0:v]{cut}{up}null[d];[1:v]{cut}{up}null[r]"
    selector = "backend=cuda:" if backend == "cuda-host" else ""
    return f"{chains};[d][r]vmafx={selector}{options}"


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
            cli = cli_report(args, pair, Path(tmp) / "cli.json", cli_backend(args))
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
        cli = cli_report(args, pair, Path(tmp) / "cli.json", cli_backend(args))
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


def has_filter(args: argparse.Namespace, name: str) -> bool:
    out = run([args.ffmpeg, "-hide_banner", "-h", f"filter={name}"], environment(args), False)
    return f"Filter {name}" in out.stdout


def legacy_pre(args: argparse.Namespace, tmp: Path, name: str, fmt: str) -> bytes:
    """`name` over the golden distorted video cut to 4x4 planes of `fmt`; "" filters nothing."""
    model = Path(__file__).resolve().parent / "pre_blur_4x4.onnx"
    chain = f"scale=4:4:flags=neighbor,format={fmt}" + (f",{name}=model={model}" if name else "")
    out = tmp / f"{name or 'none'}-{fmt}.yuv"
    raw = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "576x324", "-r", "24"]
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-loglevel", "error", *raw]
    cmd += ["-i", str(fixture(args, PAIRS["golden"].dist)), "-vf", chain, "-f", "rawvideo"]
    run([*cmd, str(out)], environment(args))
    return out.read_bytes()


def legacy_tune(args: argparse.Namespace, name: str, target: float) -> str:
    options = f"model=version=vmaf_v0.6.1:recommend_target_vmaf={target}"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", *ffmpeg_inputs(args, PAIRS["golden"])]
    cmd += ["-lavfi", f"[0:v][1:v]{name}={options}", "-f", "null", "-"]
    found = re.search(r"recommended_crf=.*", run(cmd, environment(args)).stderr)
    return found.group(0) if found else "(no recommendation)"


def cmd_legacy(args: argparse.Namespace) -> int:
    if not fixture(args, PAIRS["golden"].dist).is_file():
        return SKIP
    if not all(has_filter(args, f) for f in ("vmaf_pre", "libvmaf_tune")):
        print("skip: this FFmpeg has no vmaf_pre / libvmaf_tune (configure --enable-libvmaf)")
        return SKIP
    ok = True
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        for fmt in ("yuv420p", "yuv420p10le"):
            old, new = (
                legacy_pre(args, tmp, "vmaf_pre", fmt),
                legacy_pre(args, tmp, "vmafx_pre", fmt),
            )
            filtered = new != legacy_pre(args, tmp, "", fmt)
            ok &= old == new and filtered
            print(
                f"vmafx_pre {fmt}: {len(new)} bytes, equal to vmaf_pre: {old == new}, "
                f"differs from the input: {filtered}"
            )
    for target in (95.0, 80.0):
        old, new = (
            legacy_tune(args, "libvmaf_tune", target),
            legacy_tune(args, "vmafx_tune", target),
        )
        ok &= old == new and old.startswith("recommended_crf=")
        print(f"target {target}: libvmaf_tune {old} | vmafx_tune {new}")
    return 0 if ok else 1


def pool_run(
    args: argparse.Namespace, pair: Pair, report: Path, pool: int, import_mode: str = "host"
):
    """Both inputs in VAAPI pools of `pool` frames (hwupload allocates 2 + extra_hw_frames)."""
    up = f"format=nv12,hwupload=extra_hw_frames={pool - 2}"
    options = f"import={import_mode}:metadata=1:threads=4:log_path={escape(report)}"
    options += ":score_fmt=%.17g"
    graph = f"[0:v]{up}[d];[1:v]{up}[r];[d][r]vmafx={options},hwdownload,format=nv12"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "verbose"]
    cmd += ["-init_hw_device", f"vaapi=va:{args.vaapi_device}", "-filter_hw_device", "va"]
    cmd += [*ffmpeg_inputs(args, pair), "-lavfi", graph, "-f", "null", "-"]
    return run(cmd, environment(args), check=False)


def cmd_pool(args: argparse.Namespace) -> int:
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    if not Path(args.vaapi_device).exists():
        print(f"skip: {args.vaapi_device} missing")
        return SKIP
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        probe = pool_run(args, pair, tmp / "probe.json", 2)
        held = re.search(r"holds up to (\d+) hardware frames", probe.stderr)
        if held is None:
            print(probe.stderr[-2000:])
            return 1
        need = int(held.group(1))
        small = pool_run(args, pair, tmp / "small.json", need - 1)
        refused = small.returncode != 0 and (
            f"frame pool of the main input has {need - 1} frames" in small.stderr
            and "VMAF score" not in small.stderr
        )
        direct = pool_run(args, pair, tmp / "direct.json", need, "auto")
        named = direct.returncode != 0 and "cannot score vaapi frames" in direct.stderr
        enough = pool_run(args, pair, tmp / "filter.json", need)
        if enough.returncode != 0:
            print(enough.stderr[-2000:])
            return 1
        filt = json.loads((tmp / "filter.json").read_text(encoding="utf-8"))
        cli = cli_report(args, pair, tmp / "cli.json", "cpu")
    total, same, worst, bad = compare(cli, filt)
    print(f"held frames per input: {need}; a pool of {need - 1} refused by name: {refused}")
    print(f"VAAPI frames without import=host refused by name: {named}")
    print(
        f"| pool of {need} (VAAPI, import=host, metadata=1, threads=4) | {len(cli['frames'])} "
        f"| {total} | {same} | {worst:g} |"
    )
    for item in bad[:5]:
        print(f"MISMATCH {item}")
    return 0 if refused and named and not bad and total else 1


def e2e_command(args: argparse.Namespace, tmp: Path, pair: Pair) -> list[str]:
    """Encode with NVENC, decode the encoder's output with NVDEC in a loopback
    decoder (CUDA frames), score them against the uploaded reference, write
    the decoded frames for the file run (#2138)."""
    report, stats, decoded = tmp / "filter.json", tmp / "stats.ndjson", tmp / "decoded.yuv"
    options = (
        f"log_path={escape(report)}:score_fmt=%.17g:n_stats_frames=10:pool=min+mean:"
        f"stats_out=log+file:stats_path={escape(stats)}"
    )
    # Without -hwaccel before -dec the decoder returns system memory: the
    # reference stays in it too and the filter scores host frames.
    hw = not args.no_loopback_hwaccel
    graph = (
        f"[0:v]{'hwupload' if hw else 'null'}[r];[dec:0]split=2[d][dd];[d][r]vmafx={options}[o];"
        f"[dd]{'hwdownload,format=nv12,' if hw else ''}format=yuv420p[raw]"
    )
    size = f"{pair.width}x{pair.height}"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "info"]
    cmd += ["-init_hw_device", "cuda=cu:0", "-filter_hw_device", "cu"]
    cmd += ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", "24"]
    cmd += ["-i", str(fixture(args, pair.ref))]
    cmd += ["-map", "0:v", "-c:v", "h264_nvenc", "-preset", "p4", "-rc", "constqp", "-qp", "32"]
    cmd += ["-f", "h264", str(tmp / "encoded.h264")]
    if not args.no_loopback_hwaccel:
        cmd += ["-hwaccel", "cuda", "-hwaccel_output_format", "cuda"]
    cmd += ["-dec", "0:0", "-filter_complex", graph]
    # passthrough: a raw file must hold the decoded frames, none duplicated
    cmd += ["-map", "[o]", "-f", "null", "-", "-map", "[raw]", "-fps_mode", "passthrough"]
    cmd += ["-f", "rawvideo", str(decoded)]
    return cmd


def e2e_file_report(args: argparse.Namespace, tmp: Path, pair: Pair) -> dict:
    decoded = Pair(pair.name, pair.ref, str(tmp / "decoded.yuv"), pair.width, pair.height, 0)
    out = tmp / "cli.json"
    cmd = [args.vmaf, "-r", str(fixture(args, pair.ref)), "-d", decoded.dist]
    cmd += ["-w", str(pair.width), "-h", str(pair.height), "-p", "420", "-b", "8"]
    cmd += ["--precision", "max", "--json", "-o", str(out), "-q", "--backend", "cuda"]
    run(cmd, environment(args))
    return json.loads(out.read_text(encoding="utf-8"))


def cmd_e2e(args: argparse.Namespace) -> int:
    """#2138 on CUDA: NVENC -> loopback NVDEC -> vmafx, against the same frames from files."""
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        result = run(e2e_command(args, tmp, pair), environment(args))
        paths = [x for x in result.stderr.splitlines() if "vmafx frames:" in x]
        filt = json.loads((tmp / "filter.json").read_text(encoding="utf-8"))
        cli = e2e_file_report(args, tmp, pair)
        lines = [json.loads(x) for x in (tmp / "stats.ndjson").read_text().splitlines()]
    total, same, worst, bad = compare(cli, filt)
    checked, wbad = check_windows(lines, cli["frames"], "vmaf")
    path = paths[-1].split("vmafx frames:")[-1].strip() if paths else "(no line)"
    on_device = f"{len(cli['frames'])} imported on the device, 0 host, 0 downloaded" in path
    print(f"frame paths: {path}")
    print(
        f"| e2e nvenc->nvdec->vmafx | cuda | {len(cli['frames'])} | {total} | {same} | {worst:g} |"
    )
    print(f"windows: {len(lines)}, {checked} pooled values equal the file run ({len(wbad)} differ)")
    for item in (bad + wbad)[:5]:
        print(f"MISMATCH {item}")
    return 0 if on_device and not bad and not wbad and total else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "command",
        choices=("parity", "windows", "provenance", "refusal", "pool", "legacy", "e2e"),
    )
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--vmaf", required=True)
    parser.add_argument("--libdir", required=True, help="directory of libvmafx.so.1")
    parser.add_argument("--yuv", default=str(ROOT / "python" / "test" / "resource" / "yuv"))
    parser.add_argument("--bbb", default=str(ROOT / "testdata" / "bbb"))
    parser.add_argument("--backend", default="cpu", choices=("cpu", "cuda", "cuda-host"))
    parser.add_argument("--pairs", default="golden,checkerboard-1,checkerboard-10,bbb-4k")
    parser.add_argument("--frames-4k", type=int, default=30)
    parser.add_argument("--model", default="", help="a model spec; empty: the library default")
    parser.add_argument(
        "--no-loopback-hwaccel",
        action="store_true",
        help="e2e without -hwaccel before -dec (the decoder then returns system memory)",
    )
    parser.add_argument("--vaapi-device", default="/dev/dri/renderD128", help="pool: a VAAPI node")
    args = parser.parse_args()
    return {
        "parity": cmd_parity,
        "windows": cmd_windows,
        "provenance": cmd_provenance,
        "refusal": cmd_refusal,
        "pool": cmd_pool,
        "legacy": cmd_legacy,
        "e2e": cmd_e2e,
    }[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())

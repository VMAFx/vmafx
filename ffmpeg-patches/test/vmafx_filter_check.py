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
- ``layouts``: the 4:2:2 / 4:4:4 semi-planar, packed and MSB-aligned layouts
  (NV16, P210, Y210, Y212, YUYV422, NV24, P410, XV30, XV36, VUYX, YUV444P10MSB,
  YUV444P12MSB) score as the CLI scores the planar file they were converted
  from; ``--backend cuda`` uploads them and imports them on the device.
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

- ``vulkan``: the reference encoded on the ``--vendor``'s GPU, decoded by
  FFmpeg's Vulkan decoder on the same GPU (``-hwaccel vulkan``) and scored
  by vmafx against the reference uploaded to the Vulkan device; the filter
  copies each frame on the GPU into exportable per-plane images and the
  device of that GPU (CUDA, SYCL, HIP) imports them. Every value must equal
  the CLI's on the downloaded decoded frames, and every frame of both inputs
  must be imported on the device. ``--libplacebo`` puts FFmpeg's
  ``libplacebo`` filter between the decoder and vmafx (its output frames are
  scored the same way).

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


# (FFmpeg layout, planar layout of the CLI file, CLI -p, CLI -b)
LAYOUTS = (
    ("nv16", "yuv422p", "422", 8),
    ("yuyv422", "yuv422p", "422", 8),
    ("p210le", "yuv422p10le", "422", 10),
    ("y210le", "yuv422p10le", "422", 10),
    ("y212le", "yuv422p12le", "422", 12),
    ("nv24", "yuv444p", "444", 8),
    ("vuyx", "yuv444p", "444", 8),
    ("p410le", "yuv444p10le", "444", 10),
    ("xv30le", "yuv444p10le", "444", 10),
    ("xv36le", "yuv444p12le", "444", 12),
    ("yuv444p10msble", "yuv444p10le", "444", 10),
    ("yuv444p12msble", "yuv444p12le", "444", 12),
)


# hwupload carries Y212 to CUDA as P212, a layout the library does not import
# (it has P210 and P216): the filter must refuse it by name.
CUDA_REFUSED = {"y212le": "frames of layout p212le cannot be scored"}


def planar_file(args: argparse.Namespace, src: Path, out: Path, fmt: str) -> None:
    raw = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "576x324", "-r", "24", "-i", str(src)]
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-loglevel", "error", *raw]
    run([*cmd, "-vf", f"format={fmt}", "-f", "rawvideo", str(out)], environment(args))


def cmd_layouts(args: argparse.Namespace) -> int:
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    up = "hwupload," if args.backend == "cuda" else ""
    hw = (
        ["-init_hw_device", "cuda=cu:0", "-filter_hw_device", "cu"]
        if args.backend == "cuda"
        else []
    )
    failed = False
    print("| layout | from | backend | values | identical | max abs diff |")
    print("| --- | --- | --- | --- | --- | --- |")
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        for fmt, planar, sub, bpc in LAYOUTS:
            ref, dist = tmp / "ref.yuv", tmp / "dist.yuv"
            planar_file(args, fixture(args, pair.ref), ref, planar)
            planar_file(args, fixture(args, pair.dist), dist, planar)
            cli_out = tmp / "cli.json"
            cmd = [args.vmaf, "-r", str(ref), "-d", str(dist), "-w", "576", "-h", "324"]
            cmd += ["-p", sub, "-b", str(bpc), "--precision", "max", "--json", "-o", str(cli_out)]
            run([*cmd, "-q", "--backend", cli_backend(args)], environment(args))
            report = tmp / "filter.json"
            raw = ["-f", "rawvideo", "-pix_fmt", planar, "-s", "576x324", "-r", "24", "-i"]
            graph = (
                f"[0:v]format={fmt},{up}null[d];[1:v]format={fmt},{up}null[r];"
                f"[d][r]vmafx=log_path={escape(report)}:score_fmt=%.17g"
            )
            ff = [args.ffmpeg, "-hide_banner", "-nostdin", *hw, *raw, str(dist), *raw, str(ref)]
            result = run([*ff, "-lavfi", graph, "-f", "null", "-"], environment(args), False)
            refusal = CUDA_REFUSED.get(fmt) if args.backend == "cuda" else None
            if refusal:
                named = result.returncode != 0 and refusal in result.stderr
                failed |= not named
                print(f"| {fmt} | {planar} | {args.backend} | refused by name: {named} | | |")
                continue
            if result.returncode != 0:
                failed = True
                print(f"| {fmt} | {planar} | {args.backend} | filter failed | | |")
                print(result.stderr[-600:])
                continue
            cli = json.loads(cli_out.read_text(encoding="utf-8"))
            filt = json.loads(report.read_text(encoding="utf-8"))
            total, same, worst, bad = compare(cli, filt)
            failed |= bool(bad) or total == 0
            print(f"| {fmt} | {planar} | {args.backend} | {total} | {same} | {worst:g} |")
    return 1 if failed else 0


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


@dataclass(frozen=True)
class Vendor:
    """How one GPU vendor encodes, decodes in a loopback decoder and hands the
    frames to vmafx; `backend` is the CLI backend of the file run."""

    name: str
    backend: str
    devices: tuple[str, ...]
    encode: tuple[str, ...]
    hwaccel: tuple[str, ...]
    ref_chain: str  # the reference's way to the device
    dist_chain: str  # the decoded frames' way to the filter
    raw_chain: str  # the decoded frames' way to the raw file
    driver: str = ""  # LIBVA_DRIVER_NAME


def vendor(args: argparse.Namespace) -> Vendor:
    node = args.render_node
    drm = (f"drm=dr:{node}", "vaapi=va@dr")
    vaapi = ("-hwaccel", "vaapi", "-hwaccel_device", "va", "-hwaccel_output_format", "vaapi")
    to_drm = "hwmap=derive_device=drm,format=drm_prime"
    profiles = {
        "nvidia": Vendor(
            "nvenc->nvdec->vmafx",
            "cuda",
            ("cuda=cu:0",),
            ("-c:v", "h264_nvenc", "-preset", "p4", "-rc", "constqp", "-qp", "32"),
            ("-hwaccel", "cuda", "-hwaccel_output_format", "cuda"),
            "hwupload",
            "null",
            "hwdownload,format=nv12,",
        ),
        "intel": Vendor(
            "vaapi->vaapi->drm->vmafx",
            "sycl",
            drm,
            ("-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-qp", "32"),
            vaapi,
            f"format=nv12,hwupload,{to_drm}",
            to_drm,
            "hwdownload,format=nv12,",
            "iHD",
        ),
        "amd": Vendor(
            "vaapi->vaapi->drm->vmafx",
            "hip",
            drm,
            ("-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-qp", "32"),
            vaapi,
            f"format=nv12,hwupload,{to_drm}",
            to_drm,
            "hwdownload,format=nv12,",
            "radeonsi",
        ),
    }
    return profiles[args.vendor]


# --e2e-format: (planar file format, CLI -p, CLI -b, device layout, encoder args).
# NVDEC returns 4:4:4 at 10 and 12 bits as MSB-aligned planes
# (yuv444p10msb / yuv444p12msb). NVENC on an RTX 4090 encodes 4:4:4 at 10 bits
# only (12-bit input comes back as a 10-bit stream, which the filter refuses
# against a 12-bit reference by name); the 12-bit MSB layout is covered by the
# `layouts` check on CUDA uploads.
E2E_FORMATS = {
    "420p8": ("yuv420p", "420", 8, "nv12", ()),
    "444p10": (
        "yuv444p10le", "444", 10, "yuv444p10msble",
        ("-c:v", "hevc_nvenc", "-profile:v", "rext", "-rc", "constqp", "-qp", "32"),
    ),
}  # fmt: skip


def e2e_source(args: argparse.Namespace, tmp: Path, pair: Pair) -> Path:
    """The reference in the planar format of --e2e-format."""
    planar = E2E_FORMATS[args.e2e_format][0]
    if planar == "yuv420p":
        return fixture(args, pair.ref)
    out = tmp / f"ref.{planar}"
    planar_file(args, fixture(args, pair.ref), out, planar)
    return out


def e2e_command(args: argparse.Namespace, v: Vendor, tmp: Path, pair: Pair) -> list[str]:
    """Encode on the GPU, decode the encoder's output on the GPU in a loopback
    decoder, score it against the reference on the device, and write the
    decoded frames for the file run (#2138)."""
    report, stats, decoded = tmp / "filter.json", tmp / "stats.ndjson", tmp / "decoded.yuv"
    options = (
        f"log_path={escape(report)}:score_fmt=%.17g:n_stats_frames=10:pool=min+mean:"
        f"stats_out=log+file:stats_path={escape(stats)}"
    )
    # Without -hwaccel before -dec the decoder returns system memory: the
    # reference stays in it too and the filter scores host frames.
    hw = not args.no_loopback_hwaccel
    planar, _, _, layout, encode = E2E_FORMATS[args.e2e_format]
    ref_chain = v.ref_chain if planar == "yuv420p" else f"format={layout},hwupload"
    raw_chain = v.raw_chain.replace("format=nv12", f"format={layout}")
    graph = (
        f"[0:v]{ref_chain if hw else 'null'}[r];[dec:0]split=2[dv][dd];"
        f"[dv]{v.dist_chain if hw else 'null'}[d];[d][r]vmafx={options}[o];"
        f"[dd]{raw_chain if hw else ''}format={planar}[raw]"
    )
    size = f"{pair.width}x{pair.height}"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "info"]
    for device in v.devices:
        cmd += ["-init_hw_device", device]
    cmd += ["-filter_hw_device", v.devices[-1].split("=", 1)[1].split(":")[0].split("@")[0]]
    cmd += ["-f", "rawvideo", "-pix_fmt", planar, "-s", size, "-r", "24"]
    cmd += ["-i", str(e2e_source(args, tmp, pair))]
    if encode:
        cmd += ["-map", "0:v", "-vf", f"format={layout}", *encode, "-f", "hevc"]
        cmd += [str(tmp / "encoded.hevc")]
    else:
        cmd += ["-map", "0:v", *v.encode, "-f", "h264", str(tmp / "encoded.h264")]
    if hw:
        cmd += list(v.hwaccel)
    cmd += ["-dec", "0:0", "-filter_complex", graph]
    # passthrough: a raw file must hold the decoded frames, none duplicated
    cmd += ["-map", "[o]", "-f", "null", "-", "-map", "[raw]", "-fps_mode", "passthrough"]
    cmd += ["-f", "rawvideo", str(decoded)]
    return cmd


def e2e_file_report(args: argparse.Namespace, v: Vendor, tmp: Path, pair: Pair) -> dict:
    out = tmp / "cli.json"
    _, sub, bpc, _, _ = E2E_FORMATS[args.e2e_format]
    cmd = [args.vmaf, "-r", str(e2e_source(args, tmp, pair)), "-d", str(tmp / "decoded.yuv")]
    cmd += ["-w", str(pair.width), "-h", str(pair.height), "-p", sub, "-b", str(bpc)]
    cmd += ["--precision", "max", "--json", "-o", str(out), "-q", "--backend", v.backend]
    run(cmd, e2e_environment(args, v))
    return json.loads(out.read_text(encoding="utf-8"))


def e2e_environment(args: argparse.Namespace, v: Vendor) -> dict[str, str]:
    env = environment(args)
    if v.driver:
        env["LIBVA_DRIVER_NAME"] = v.driver
    return env


# The Vulkan device of a vendor's GPU, by FFmpeg's device-name match.
VULKAN_DEVICES = {"nvidia": "NVIDIA", "intel": "Intel", "amd": "RADV"}


def hw_device_flags(v: Vendor) -> list[str]:
    cmd: list[str] = []
    for device in v.devices:
        cmd += ["-init_hw_device", device]
    return [*cmd, "-filter_hw_device", v.devices[-1].split("=", 1)[1].split(":")[0].split("@")[0]]


def vulkan_encode(args: argparse.Namespace, v: Vendor, tmp: Path, pair: Pair) -> Path:
    """The reference encoded on the vendor's GPU, as `e2e` encodes it."""
    out = tmp / "encoded.h264"
    size = f"{pair.width}x{pair.height}"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y", *hw_device_flags(v)]
    cmd += ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", "24"]
    cmd += ["-i", str(fixture(args, pair.ref)), *v.encode, "-f", "h264", str(out)]
    run(cmd, e2e_environment(args, v))
    return out


def vulkan_command(args: argparse.Namespace, tmp: Path, pair: Pair, encoded: Path) -> list[str]:
    """Vulkan decode -> (libplacebo) -> vmafx against the uploaded reference,
    and the scored frames downloaded for the file run."""
    report, decoded = tmp / "filter.json", tmp / "decoded.yuv"
    placebo = "libplacebo=format=nv12," if args.libplacebo else ""
    graph = (
        f"[0:v]{placebo}split=2[dv][dd];[1:v]format=nv12,hwupload[r];"
        f"[dv][r]vmafx=log_path={escape(report)}:score_fmt=%.17g[o];"
        "[dd]hwdownload,format=nv12,format=yuv420p[raw]"
    )
    size = f"{pair.width}x{pair.height}"
    cmd = [args.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "info", "-y"]
    cmd += ["-init_hw_device", f"vulkan=vk:{VULKAN_DEVICES[args.vendor]}"]
    cmd += ["-filter_hw_device", "vk", "-hwaccel", "vulkan", "-hwaccel_device", "vk"]
    cmd += ["-hwaccel_output_format", "vulkan", "-i", str(encoded)]
    cmd += ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", "24"]
    cmd += ["-i", str(fixture(args, pair.ref)), "-filter_complex", graph]
    cmd += ["-map", "[o]", "-f", "null", "-", "-map", "[raw]", "-fps_mode", "passthrough"]
    return [*cmd, "-f", "rawvideo", str(decoded)]


def cmd_vulkan(args: argparse.Namespace) -> int:
    """Vulkan frames (ADR-2152) into vmafx on the device of their GPU."""
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    v = vendor(args)
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        encoded = vulkan_encode(args, v, tmp, pair)
        result = run(vulkan_command(args, tmp, pair, encoded), e2e_environment(args, v))
        paths = [x for x in result.stderr.splitlines() if "vmafx frames:" in x]
        filt = json.loads((tmp / "filter.json").read_text(encoding="utf-8"))
        cli = e2e_file_report(args, v, tmp, pair)
    total, same, worst, bad = compare(cli, filt)
    path = paths[-1].split("vmafx frames:")[-1].strip() if paths else "(no line)"
    frames = len(cli["frames"])
    expect = f"{2 * frames} imported on the device, 0 host, 0 downloaded"
    route = "vulkan->libplacebo->vmafx" if args.libplacebo else "vulkan->vmafx"
    print(f"frame paths: {path} (expected: {expect})")
    print(f"| {route} ({args.vendor}) | {v.backend} | {frames} | {total} | {same} | {worst:g} |")
    for item in bad[:5]:
        print(f"MISMATCH {item}")
    return 0 if expect == path and not bad and total else 1


def cmd_e2e(args: argparse.Namespace) -> int:
    """#2138: GPU encode -> loopback GPU decode -> vmafx on the device, against
    the same frames scored from files by the CLI on the same backend."""
    pair = PAIRS["golden"]
    if not fixture(args, pair.ref).is_file():
        return SKIP
    v = vendor(args)
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        result = run(e2e_command(args, v, tmp, pair), e2e_environment(args, v))
        paths = [x for x in result.stderr.splitlines() if "vmafx frames:" in x]
        filt = json.loads((tmp / "filter.json").read_text(encoding="utf-8"))
        cli = e2e_file_report(args, v, tmp, pair)
        lines = [json.loads(x) for x in (tmp / "stats.ndjson").read_text().splitlines()]
    total, same, worst, bad = compare(cli, filt)
    checked, wbad = check_windows(lines, cli["frames"], "vmaf")
    path = paths[-1].split("vmafx frames:")[-1].strip() if paths else "(no line)"
    frames = len(cli["frames"])
    # both inputs count: the decoded frames and the reference
    expect = (
        f"{2 * frames} imported on the device, 0 host, 0 downloaded"
        if not args.no_loopback_hwaccel
        else f"0 imported on the device, {2 * frames} host, 0 downloaded"
    )
    print(f"frame paths: {path} (expected: {expect})")
    print(
        f"| e2e {v.name} {args.e2e_format} | {v.backend} | {frames} | {total} | {same} | {worst:g} |"
    )
    print(f"windows: {len(lines)}, {checked} pooled values equal the file run ({len(wbad)} differ)")
    for item in (bad + wbad)[:5]:
        print(f"MISMATCH {item}")
    return 0 if expect == path and not bad and not wbad and total else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "command",
        choices=(
            "parity",
            "windows",
            "provenance",
            "refusal",
            "pool",
            "legacy",
            "layouts",
            "e2e",
            "vulkan",
        ),
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
    parser.add_argument(
        "--vendor", default="nvidia", choices=("nvidia", "intel", "amd"), help="e2e"
    )
    parser.add_argument("--render-node", default="/dev/dri/renderD128", help="e2e: the GPU's node")
    parser.add_argument("--e2e-format", default="420p8", choices=("420p8", "444p10"))
    parser.add_argument(
        "--libplacebo", action="store_true", help="vulkan: libplacebo between decoder and vmafx"
    )
    args = parser.parse_args()
    return {
        "parity": cmd_parity,
        "windows": cmd_windows,
        "provenance": cmd_provenance,
        "refusal": cmd_refusal,
        "pool": cmd_pool,
        "legacy": cmd_legacy,
        "layouts": cmd_layouts,
        "e2e": cmd_e2e,
        "vulkan": cmd_vulkan,
    }[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())

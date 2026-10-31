#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every vmaf_v1.0.16* and hdr_vmaf_v1.1.8* model runs wholly on a GPU backend.

Usage::

    test_gpu_v1_models_no_fallback.py --self-test
    test_gpu_v1_models_no_fallback.py --backend cuda|sycl|hip|metal

With ``--backend``, every built-in ``vmaf_v1.0.16*`` model (one per JSON file
under ``model/vmaf_v1.0.16*/``) and the CLI's default (a run without
``--model``) score each SDR fixture twice with the ``vmaf`` CLI of this build,
at ``--precision max``: once with ``--backend cpu`` and once with the device
backend. The SDR fixtures are the 576x324 `src01_hrc00` / `src01_hrc01` pair at
8, 10 and 12 bits 4:2:0 and 10 bits 4:2:2, and the first 16 frames of the
3840x2160 pair in `testdata/bbb`.

Every built-in ``hdr_vmaf_v1.1.8*`` model (under ``model/hdr_vmaf_v1.1.8*/``)
scores two HDR inputs twice (CPU and device): the 1-frame 576x324 10-bit 4:2:0
PQ dock pair (`ref_dock-na-HDR...` / `dis_dock-na-HDR...`) and the 16-frame
576x324 10-bit 4:2:2 tagged pair (`src01_hrc00...422p10le.yuv` /
`src01_hrc01...422p10le.yuv`), with color tags
(`--color_range_* limited --color_primaries_* bt2020 --color_trc_* smpte2084
--color_matrix_* bt2020nc`).

A run fails when the device run lists an extractor on any other backend in
``feature_backends`` (a CPU fallback), when its ``backend_used`` is not the
device, on any difference in a per-frame, pooled or aggregate metric, or on a
missing HDR fixture or zimg refusal. On CUDA runs, device free memory is
inspected before device runs and compared against the estimated lower bound.

Every device run also scores one known fallback: ``--feature
float_adm=adm_csf_mode=1``, an option value every ``float_adm`` twin
implements only at its default (``VMAF_OPT_FLAG_DEFAULT_ONLY``, ADR-1316). The
check must name ``float_adm`` as a fallback there, so the test fails when the
check stops seeing fallbacks.

``--self-test`` runs the comparison on synthetic receipts without a device.

Exit 77 (skip) when this build was configured without the backend, the CLI
refuses the backend (exit 100: no device), or an SDR fixture is absent; the
reason is printed. Device runs take the device's lock under ``VMAFX_LOCK_DIR``
(default ``~/.cache/vmafx-locks``) when that directory exists, with a 300 s
limit inside the lock.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import re
import shutil
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

YUV = ROOT / "python" / "test" / "resource" / "yuv"
UHD = ROOT / "testdata" / "bbb"
V1_MODEL_GLOB = "vmaf_v1.0.16*/vmaf_v1.0.16*.json"
HDR_MODEL_GLOB = "hdr_vmaf_v1.1.8*/hdr_vmaf_v1.1.8*.json"
MODEL_GLOB = V1_MODEL_GLOB
DEFAULT_LABEL = "default"
SKIP = 77
EXIT_BACKEND_REFUSED = 100
EXIT_OOM = 244
ERRNO_ENOMEM = -12
NEGATIVE_12 = re.compile(r"(?<![\w-])-12\b")
NEGATIVE_95 = re.compile(r"(?<![\w-])-95\b")
DEVICE_RUN_LIMIT_S = 300
WAIT_LIMIT_S = 3600
BACKEND_OPTION = {
    "cuda": "enable_cuda",
    "sycl": "enable_sycl",
    "hip": "enable_hipcc",
    "metal": "enable_metal",
}
LOCK_FILE = {
    "cuda": "cuda-4090.lock",
    "sycl": "sycl-a380.lock",
    "hip": "hip-gfx1036.lock",
    "metal": "metal.lock",
}
KNOWN_FALLBACK_FEATURE = "float_adm=adm_csf_mode=1"
KNOWN_FALLBACK_EXTRACTOR = "float_adm"
V1_MODEL_COUNT = 8
HDR_MODEL_COUNT = 8
TOTAL_MODEL_COUNT = 16


@dataclasses.dataclass(frozen=True)
class Fixture:
    """One reference / distorted pair and the geometry the CLI needs."""

    name: str
    ref: Path
    dis: Path
    width: int
    height: int
    pix_fmt: str
    bitdepth: int
    frame_cnt: int | None = None
    extra_args: tuple[str, ...] = ()


def src01_fixture(name: str, suffix: str, pix_fmt: str, bitdepth: int) -> Fixture:
    """The 576x324 src01 pair stored as `src01_hrc0N_576x324<suffix>`."""
    return Fixture(
        name=name,
        ref=YUV / f"src01_hrc00_576x324{suffix}",
        dis=YUV / f"src01_hrc01_576x324{suffix}",
        width=576,
        height=324,
        pix_fmt=pix_fmt,
        bitdepth=bitdepth,
    )


FIXTURES: tuple[Fixture, ...] = (
    src01_fixture("src01-420-8", ".yuv", "420", 8),
    src01_fixture("src01-420-10", ".yuv420p10le.yuv", "420", 10),
    src01_fixture("src01-420-12", ".yuv420p12le.yuv", "420", 12),
    src01_fixture("src01-422-10", ".yuv422p10le.yuv", "422", 10),
    Fixture(
        name="uhd-3840x2160-420-8",
        ref=UHD / "ref_3840x2160_200f.yuv",
        dis=UHD / "dis_3840x2160_200f.yuv",
        width=3840,
        height=2160,
        pix_fmt="420",
        bitdepth=8,
        frame_cnt=16,
    ),
)

HDR_COLOR_ARGS: tuple[str, ...] = (
    "--color_range_ref",
    "limited",
    "--color_primaries_ref",
    "bt2020",
    "--color_trc_ref",
    "smpte2084",
    "--color_matrix_ref",
    "bt2020nc",
    "--color_range_dist",
    "limited",
    "--color_primaries_dist",
    "bt2020",
    "--color_trc_dist",
    "smpte2084",
    "--color_matrix_dist",
    "bt2020nc",
)

HDR_FIXTURES: tuple[Fixture, ...] = (
    Fixture(
        name="dock-hdr-576x324-420-10",
        ref=YUV
        / "ref_dock-na-HDR_res2160_rate5p0_maxRate100000_bufSize200000_fps59p940_576x324_1frame.yuv",
        dis=YUV
        / "dis_dock-na-HDR_res1440_rate15p0_maxRate10000_bufSize20000_fps59p940_576x324_1frame.yuv",
        width=576,
        height=324,
        pix_fmt="420",
        bitdepth=10,
        frame_cnt=1,
        extra_args=HDR_COLOR_ARGS,
    ),
    Fixture(
        name="src01-tagged-576x324-422-10",
        ref=YUV / "src01_hrc00_576x324.yuv422p10le.yuv",
        dis=YUV / "src01_hrc01_576x324.yuv422p10le.yuv",
        width=576,
        height=324,
        pix_fmt="422",
        bitdepth=10,
        frame_cnt=16,
        extra_args=HDR_COLOR_ARGS,
    ),
)


# ---------------------------------------------------------------------------
# The comparison: pure functions over the CLI's JSON receipts.
# ---------------------------------------------------------------------------


def fallback_problems(receipt: dict[str, Any], backend: str) -> list[str]:
    """Why `receipt` is not a run done wholly on `backend`; empty when it is."""
    problems: list[str] = []
    used = receipt.get("backend_used")
    if used != backend:
        problems.append(f"backend_used is {used!r}, not {backend!r}")
    entries = receipt.get("feature_backends")
    if not isinstance(entries, list) or not entries:
        return [*problems, "feature_backends is missing or empty"]
    for entry in entries:
        on = entry.get("backend") if isinstance(entry, dict) else None
        if on != backend:
            name = entry.get("extractor") if isinstance(entry, dict) else entry
            problems.append(f"extractor {name!r} ran on {on!r}")
    return problems


def same_value(a: Any, b: Any) -> bool:
    """Equal values; two NaNs are the same value."""
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return type(a) is type(b) and a == b


def flat_values(where: str, value: Any) -> dict[str, Any]:
    """`value` as dotted keys, two mapping levels deep (pooled: metric -> statistic)."""
    if not isinstance(value, dict):
        return {where: value}
    flat: dict[str, Any] = {}
    for key, inner in value.items():
        if isinstance(inner, dict):
            flat |= {f"{where}.{key}.{stat}": leaf for stat, leaf in inner.items()}
        else:
            flat[f"{where}.{key}"] = inner
    return flat


def mapping_problems(where: str, cpu: Any, dev: Any) -> list[str]:
    """Differences between two metric mappings: keys on one side only, unequal values."""
    cpu_flat, dev_flat = flat_values(where, cpu), flat_values(where, dev)
    problems = [
        f"{key}: only the {'cpu' if key in cpu_flat else 'device'} run has it"
        for key in sorted(set(cpu_flat) ^ set(dev_flat))
    ]
    for key in sorted(set(cpu_flat) & set(dev_flat)):
        if not same_value(cpu_flat[key], dev_flat[key]):
            problems.append(f"{key}: cpu {cpu_flat[key]!r} != device {dev_flat[key]!r}")
    return problems


def score_problems(cpu: dict[str, Any], dev: dict[str, Any]) -> list[str]:
    """Every per-frame, pooled or aggregate value the two runs do not share."""
    cpu_frames = cpu.get("frames") or []
    dev_frames = dev.get("frames") or []
    if not cpu_frames or len(cpu_frames) != len(dev_frames):
        return [f"frame count: cpu {len(cpu_frames)}, device {len(dev_frames)}"]
    problems: list[str] = []
    for index, (a, b) in enumerate(zip(cpu_frames, dev_frames, strict=True)):
        problems += mapping_problems(f"frames[{index}]", a.get("metrics"), b.get("metrics"))
    for section in ("pooled_metrics", "aggregate_metrics"):
        problems += mapping_problems(section, cpu.get(section), dev.get(section))
    return problems


def run_problems(cpu: dict[str, Any], dev: dict[str, Any], backend: str) -> list[str]:
    """Fallbacks of the device run, then its score differences from the CPU run."""
    return fallback_problems(dev, backend) + score_problems(cpu, dev)


def run_error_problems(backend: str, code: int, err: str) -> list[str]:
    """Evaluate CLI exit code and output for known errors and fallbacks."""
    if code == 0:
        return []
    err_str = err.strip()
    if "built without zimg support" in err_str or NEGATIVE_95.search(err_str):
        return [f"{backend} run failed (built without zimg support, code {code}): {err_str[-400:]}"]
    if (
        code in (EXIT_OOM, ERRNO_ENOMEM)
        or NEGATIVE_12.search(err_str)
        or "CUDA_ERROR_OUT_OF_MEMORY" in err_str
    ):
        return [f"{backend} run exited {code} (-12 out of device memory): {err_str[-400:]}"]
    if code == EXIT_BACKEND_REFUSED and backend != "cpu":
        raise Refused(err_str)
    return [f"{backend} run exited {code}: {err_str[-400:]}"]


# ---------------------------------------------------------------------------
# Running the CLI.
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Runner:
    """The CLI of this build, and the lock wrapper of the device under test."""

    vmaf: Path
    backend: str
    workdir: Path
    prefix: tuple[str, ...]

    def argv(self, fixture: Fixture, model: str, backend: str, out: Path) -> list[str]:
        args = [str(self.vmaf), "-r", str(fixture.ref), "-d", str(fixture.dis)]
        args += ["-w", str(fixture.width), "-h", str(fixture.height)]
        args += ["-p", fixture.pix_fmt, "-b", str(fixture.bitdepth)]
        if fixture.frame_cnt is not None:
            args += ["--frame_cnt", str(fixture.frame_cnt)]
        if model != DEFAULT_LABEL:
            args += ["--model", f"version={model}"]
        if fixture.extra_args:
            args += list(fixture.extra_args)
        args += ["--precision", "max", "--json", "-o", str(out), "--backend", backend]
        return args

    def run(self, argv: list[str], *, on_device: bool) -> tuple[int, str]:
        full = [*self.prefix, *argv] if on_device else argv
        proc = run_command(
            full,
            allowed_executables=(full[0],),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=WAIT_LIMIT_S if on_device else DEVICE_RUN_LIMIT_S,
            max_output_bytes=4 * 1_048_576,
        )
        return proc.returncode, (proc.stderr or proc.stdout)


class Refused(Exception):
    """The CLI refused the device backend: nothing on this host to compare."""


def vram_precheck(backend: str, model: str, fixture: Fixture) -> None:
    """Print CUDA free memory vs estimated requirement before a device run."""
    if backend != "cuda":
        return
    smi = shutil.which("nvidia-smi")
    if not smi:
        return
    res = run_command(
        [smi, "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
        allowed_executables=(smi,),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=5,
    )
    if res.returncode != 0 or not res.stdout.strip():
        return
    try:
        free_mib = float(res.stdout.strip().splitlines()[0])
    except ValueError:
        return
    # Lower bound from a fresh-context probe: 8 pictures (ring buffer + pool)
    # x display width x height x 2 bytes (16-bit).
    is_hdr = "hdr_vmaf" in model
    disp_w, disp_h = (
        (3840, 2160)
        if "2160" in model
        else ((1920, 1080) if is_hdr else (fixture.width, fixture.height))
    )
    need_bytes = 8 * disp_w * disp_h * 2
    need_mib = need_bytes / (1024 * 1024)
    print(
        f"cuda VRAM pre-check: free {free_mib:.0f} MiB, estimated need >= {need_mib:.1f} MiB "
        f"({disp_w}x{disp_h}x2B x 8 frames lower bound from fresh-context probe)"
    )


def score(runner: Runner, fixture: Fixture, model: str, extra: Sequence[str] = ()) -> list[str]:
    """Problems of one model on one fixture, CPU run against device run."""
    missing = [str(p) for p in (fixture.ref, fixture.dis) if not p.is_file()]
    if missing:
        return [
            f"fixture files not found: {', '.join(missing)}; "
            "scripts/test/fetch-test-yuvs.sh fetches python/test/resource/yuv"
        ]
    receipts: dict[str, dict[str, Any]] = {}
    for backend in ("cpu", runner.backend):
        out = runner.workdir / f"{fixture.name}_{model}_{backend}.json"
        argv = [*runner.argv(fixture, model, backend, out), *extra]
        if backend != "cpu":
            vram_precheck(backend, model, fixture)
        code, err = runner.run(argv, on_device=backend != "cpu")
        problems = run_error_problems(backend, code, err)
        if problems:
            return problems
        receipts[backend] = json.loads(out.read_text(encoding="utf-8"))
    return run_problems(receipts["cpu"], receipts[runner.backend], runner.backend)


def known_fallback_seen(runner: Runner) -> str | None:
    """None when the check names the planted fallback, else why it does not."""
    fixture = FIXTURES[0]
    problems = score(runner, fixture, DEFAULT_LABEL, ("--feature", KNOWN_FALLBACK_FEATURE))
    named = [p for p in problems if f"extractor {KNOWN_FALLBACK_EXTRACTOR!r} ran on 'cpu'" in p]
    if named:
        return None
    return f"--feature {KNOWN_FALLBACK_FEATURE} was not reported as a fallback: {problems}"


def v1_model_versions() -> list[str]:
    """The built-in vmaf_v1.0.16* versions, one per model JSON in the tree."""
    return sorted(path.stem for path in (ROOT / "model").glob(V1_MODEL_GLOB))


def hdr_model_versions() -> list[str]:
    """The built-in hdr_vmaf_v1.1.8* versions, one per model JSON in the tree."""
    return sorted(path.stem for path in (ROOT / "model").glob(HDR_MODEL_GLOB))


def model_versions() -> list[str]:
    """All built-in model versions under test: v1.0.16* then hdr_v1.1.8*."""
    return v1_model_versions() + hdr_model_versions()


def lock_prefix(backend: str) -> tuple[str, ...]:
    """`flock <lock> timeout 300` when the lock directory exists, else `timeout 300`."""
    lock_dir = Path(os.environ.get("VMAFX_LOCK_DIR", Path.home() / ".cache" / "vmafx-locks"))
    flock = shutil.which("flock")
    timeout = shutil.which("timeout")
    limit = (timeout, str(DEVICE_RUN_LIMIT_S)) if timeout else ()
    lock_file = LOCK_FILE.get(backend, f"{backend}.lock")
    if flock and lock_dir.is_dir():
        return (flock, str(lock_dir / lock_file), *limit)
    return limit


# ---------------------------------------------------------------------------
# Skip decisions, made before any device is touched.
# ---------------------------------------------------------------------------


def find_cli() -> Path | None:
    """The CLI Meson passes in VMAF_CLI, or `build/tools/vmaf`."""
    configured = os.environ.get("VMAF_CLI")
    binary = Path(configured) if configured else ROOT / "build" / "tools" / "vmaf"
    return binary if binary.is_file() else None


def build_option(build_dir: Path, name: str) -> bool | str | None:
    """A Meson build option of `build_dir`, or None when Meson does not say."""
    options = build_dir / "meson-info" / "intro-buildoptions.json"
    try:
        entries = json.loads(options.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    for entry in entries if isinstance(entries, list) else []:
        if isinstance(entry, dict) and entry.get("name") == name:
            value = entry.get("value")
            if isinstance(value, (bool, str)):
                return value
    return None


def skip_reason(cli: Path | None, backend: str) -> str | None:
    """Why there is nothing to compare on this build and host, or None."""
    if cli is None:
        return "vmaf CLI not found (VMAF_CLI)"
    val = build_option(cli.parent.parent, BACKEND_OPTION[backend])
    if val is False or val == "disabled":
        return f"this build was configured without {BACKEND_OPTION[backend]}"
    if backend == "metal" and sys.platform != "darwin" and val == "auto":
        return f"this build was configured without {BACKEND_OPTION[backend]} (auto off darwin)"
    return None


def absent_fixtures() -> tuple[Fixture, ...]:
    """SDR pairs not on this host; an absent HDR pair fails its rows instead."""
    return tuple(f for f in FIXTURES if not (f.ref.is_file() and f.dis.is_file()))


def device_main(backend: str) -> int:
    """Score every model on every fixture on `backend`; 0 when all are the CPU's."""
    cli = find_cli()
    reason = skip_reason(cli, backend)
    if reason is not None or cli is None:
        sys.stderr.write(f"{reason}; skipping ({SKIP})\n")
        return SKIP
    v1_versions = v1_model_versions()
    hdr_versions = hdr_model_versions()
    if not v1_versions and not hdr_versions:
        sys.stderr.write("no model matches model globs\n")
        return 1
    v1_models = [*v1_versions, DEFAULT_LABEL]
    with tempfile.TemporaryDirectory(prefix=f"models-{backend}-") as tmp:
        runner = Runner(cli, backend, Path(tmp), lock_prefix(backend))
        try:
            return report(runner, v1_models, hdr_versions)
        except Refused as refused:
            sys.stderr.write(f"--backend {backend} refused: {refused}; skipping ({SKIP})\n")
            return SKIP


def planned_runs(
    v1_models: Sequence[str], hdr_models: Sequence[str], absent: Sequence[Fixture]
) -> list[tuple[str, Fixture]]:
    """The rows to score: an absent SDR pair never takes an HDR row with it."""
    runs = [(m, f) for m in v1_models for f in FIXTURES if f not in absent]
    runs += [(m, f) for m in hdr_models for f in HDR_FIXTURES]
    return runs


def final_status(failed: int, missed: str | None, absent: Sequence[Fixture]) -> int:
    """A failure outranks a skip; rows not run are never reported as passing."""
    if failed or missed:
        return 1
    return SKIP if absent else 0


def report(runner: Runner, v1_models: list[str], hdr_models: list[str]) -> int:
    """Run every (model, fixture) pair and the known fallback; print the table."""
    failed = 0
    absent = absent_fixtures()
    runs = planned_runs(v1_models, hdr_models, absent)
    for model, fixture in runs:
        problems = score(runner, fixture, model)
        verdict = "ok" if not problems else f"FAIL ({len(problems)})"
        print(f"{runner.backend:<5} {model:<30} {fixture.name:<26} {verdict}")
        for problem in problems[:20]:
            print(f"    {problem}")
        failed += bool(problems)
    missed = known_fallback_seen(runner)
    print(f"{runner.backend:<5} known fallback {KNOWN_FALLBACK_FEATURE}: {missed or 'reported'}")
    total = len(runs)
    print(f"{runner.backend}: {total - failed} of {total} runs wholly on the device and equal")
    if absent:
        names = ", ".join(f.name for f in absent)
        print(f"{runner.backend}: SDR rows not run, fixtures not found: {names}")
    return final_status(failed, missed, absent)


# ---------------------------------------------------------------------------
# Device-free self-test of the comparison.
# ---------------------------------------------------------------------------


def receipt(backend: str, entries: list[tuple[str, str]], value: float) -> dict[str, Any]:
    """A synthetic CLI receipt: one frame, one pooled metric."""
    return {
        "backend_used": backend,
        "feature_backends": [{"extractor": e, "backend": b} for e, b in entries],
        "frames": [{"frameNum": 0, "metrics": {"vmaf": value, "adm2": 0.5}}],
        "pooled_metrics": {"vmaf": {"mean": value}},
        "aggregate_metrics": {},
    }


def check_zimg_refusal_fails() -> bool:
    """True when zimg refusal produces a failure and never a Refused skip."""
    try:
        problems = run_error_problems(
            "cuda",
            EXIT_BACKEND_REFUSED,
            "libvmaf was built without zimg support (configure with -Denable_zimg=true)",
        )
        return bool(problems) and "built without zimg support" in problems[0]
    except Refused:
        return False


def hdr_self_test_cases() -> list[tuple[str, bool]]:
    """Synthetic cases for HDR models, zimg refusal and device OOM."""
    hdr_cpu = receipt("cpu", [("adm", "cpu"), ("vif", "cpu")], 78.5)
    hdr_dev = receipt("cuda", [("adm_cuda", "cuda"), ("vif_cuda", "cuda")], 78.5)
    hdr_diff = receipt("cuda", [("adm_cuda", "cuda"), ("vif_cuda", "cuda")], 78.2)
    oom_err = run_error_problems("cuda", 244, "cuMemAllocPitch failed: CUDA_ERROR_OUT_OF_MEMORY")
    return [
        ("HDR row equal passes", run_problems(hdr_cpu, hdr_dev, "cuda") == []),
        ("one metric differs fails", bool(run_problems(hdr_cpu, hdr_diff, "cuda"))),
        (
            "backend_used wrong fails",
            fallback_problems(hdr_dev, "hip")[:1] == ["backend_used is 'cuda', not 'hip'"],
        ),
        ("zimg refusal text fails not skip", check_zimg_refusal_fails()),
        (
            "absent SDR pair keeps every HDR row",
            planned_runs(["sdr"], ["hdr"], FIXTURES)
            == [("hdr", fixture) for fixture in HDR_FIXTURES],
        ),
        ("rows not run skip, never pass", final_status(0, None, FIXTURES) == SKIP),
        ("failure outranks rows not run", final_status(1, None, FIXTURES) == 1),
        ("all rows run and equal pass", final_status(0, None, ()) == 0),
        (
            "fixture name with -12 is not memory",
            "-12 out of" not in run_error_problems("cuda", 1, "src01-420-12.yuv")[0],
        ),
        (
            "rc 244 with -12 fails",
            bool(oom_err) and "-12 out of device memory" in oom_err[0],
        ),
    ]


def v1_self_test_cases() -> list[tuple[str, bool]]:
    """Synthetic cases for SDR v1 models, fallbacks and float arithmetic."""
    cpu = receipt("cpu", [("adm", "cpu")], 90.0)
    on_dev = receipt("cuda", [("adm_cuda", "cuda"), ("motion_cuda", "cuda")], 90.0)
    fell_back = receipt("cuda", [("adm_cuda", "cuda"), ("float_adm", "cpu")], 90.0)
    one_ulp = receipt("cuda", [("adm_cuda", "cuda")], math.nextafter(90.0, 100.0))
    extra_key = receipt("cuda", [("adm_cuda", "cuda")], 90.0)
    extra_key["frames"][0]["metrics"]["motion2"] = 0.0
    no_entries = receipt("cuda", [], 90.0)
    nan_cpu = receipt("cpu", [], math.nan)
    nan_dev = receipt("cuda", [("adm_cuda", "cuda")], math.nan)
    short = receipt("cuda", [("adm_cuda", "cuda")], 90.0) | {"frames": []}
    return [
        ("identical device run passes", run_problems(cpu, on_dev, "cuda") == []),
        (
            "cpu entry is named",
            run_problems(cpu, fell_back, "cuda") == ["extractor 'float_adm' ran on 'cpu'"],
        ),
        (
            "one ulp is a difference",
            [p.split(":")[0] for p in run_problems(cpu, one_ulp, "cuda")]
            == ["frames[0].vmaf", "pooled_metrics.vmaf.mean"],
        ),
        (
            "key on one side only",
            run_problems(cpu, extra_key, "cuda")
            == ["frames[0].motion2: only the device run has it"],
        ),
        (
            "empty feature_backends fails",
            run_problems(cpu, no_entries, "cuda") == ["feature_backends is missing or empty"],
        ),
        (
            "wrong backend_used fails",
            fallback_problems(on_dev, "hip")[:1] == ["backend_used is 'cuda', not 'hip'"],
        ),
        ("NaN equals NaN", run_problems(nan_cpu, nan_dev, "cuda") == []),
        (
            "frame count differs",
            run_problems(cpu, short, "cuda") == ["frame count: cpu 1, device 0"],
        ),
        ("int is not float", not same_value(1, 1.0)),
        (
            "feature_backends with CPU fails",
            run_problems(cpu, fell_back, "cuda") == ["extractor 'float_adm' ran on 'cpu'"],
        ),
    ]


def self_test_cases() -> list[tuple[str, bool]]:
    """(case, passed) pairs: positive, negative and boundary receipts."""
    return v1_self_test_cases() + hdr_self_test_cases()


def self_test() -> int:
    """0 when every synthetic case gives the expected verdict."""
    failed = [name for name, passed in self_test_cases() if not passed]
    for name in failed:
        print(f"self-test FAIL: {name}")
    if len(v1_model_versions()) != V1_MODEL_COUNT:
        failed.append(f"model/{V1_MODEL_GLOB} names {len(v1_model_versions())} models")
        print(f"self-test FAIL: {failed[-1]}")
    if len(hdr_model_versions()) != HDR_MODEL_COUNT:
        failed.append(f"model/{HDR_MODEL_GLOB} names {len(hdr_model_versions())} models")
        print(f"self-test FAIL: {failed[-1]}")
    total_checks = len(self_test_cases()) + 2
    passed_count = total_checks - len(failed)
    print(f"self-test: {passed_count} passed, {len(failed)} failed")
    return 1 if failed else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--backend", choices=sorted(BACKEND_OPTION))
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    return self_test() if args.self_test else device_main(args.backend)


if __name__ == "__main__":
    sys.exit(main())

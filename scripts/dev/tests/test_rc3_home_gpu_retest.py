# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Positive, negative and boundary controls for the RC3 home GPU retest kit.

Nothing here needs a GPU, a build or the fixtures: the helper is fed small JSON
files, and the shell script runs with ``--list``, ``--dry-run``, bad arguments,
and against a fake ``vmaf`` that writes the JSON a real run would.
"""

from __future__ import annotations

import contextlib
import fcntl
import importlib.util
import io
import json
import os
import re
import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/dev/rc3-home-gpu-retest.sh"
STATE = ROOT / "docs/state.md"
SPEC = importlib.util.spec_from_file_location(
    "rc3_retest_helpers", ROOT / "scripts/dev/rc3_retest_helpers.py"
)
assert SPEC is not None and SPEC.loader is not None
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)
ROW_ID = re.compile(r"^T-[A-Z0-9.-]+-\d{4}-\d{2}-\d{2}$")
SCRIPT_TIMEOUT_SECONDS = 120


def run_helper(*argv: str) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = HELPERS.main(list(argv))
    return rc, out.getvalue().strip()


def run_script(*argv: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 - the repository's own script, test-owned arguments
        [str(SCRIPT), *argv],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
        timeout=SCRIPT_TIMEOUT_SECONDS,
        env=env,
    )


# Stands in for tools/vmaf: parses the arguments the kit passes, logs the
# device it was pinned to, and writes one frame of psnr_hvs JSON.
FAKE_VMAF = """#!/bin/sh
out=
backend=cpu
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift 2 ;;
    --backend) backend=$2; shift 2 ;;
    *) shift ;;
  esac
done
echo "backend=$backend cuda=${CUDA_VISIBLE_DEVICES-unset}" >>"$FAKE_VMAF_LOG"
value=30.0
twin=psnr_hvs
if [ "$backend" != cpu ]; then
  value=${FAKE_GPU_VALUE:-30.0}
  twin=${FAKE_TWIN:-psnr_hvs_$backend}
fi
printf '{"frames": [{"frameNum": 0, "metrics": {"psnr_hvs": %s}}], "backend_used": "%s",\
 "feature_backends": [{"extractor": "%s", "backend": "%s"}]}\n' \
  "$value" "$backend" "$twin" "$backend" >"$out"
"""


def listed_entries() -> list[tuple[str, str]]:
    result = run_script("--list")
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    entries: list[tuple[str, str]] = []
    for line in result.stdout.splitlines():
        if line.strip():
            parts = line.split()
            entries.append((parts[0], parts[1]))
    return entries


class HelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, name: str, payload: object, prefix: str = "") -> str:
        path = self.dir / name
        path.write_text(prefix + json.dumps(payload), encoding="utf-8")
        return str(path)

    def frames(self, name: str, *rows: dict[str, float | None]) -> str:
        return self.write(name, {"frames": [{"metrics": row} for row in rows]})

    def test_identical_frames_pass_at_zero(self) -> None:
        a = self.frames("a.json", {"m": 1.0}, {"m": 2.0})
        b = self.frames("b.json", {"m": 1.0}, {"m": 2.0})
        self.assertEqual(run_helper("compare", a, b, "0"), (0, "max 0, 2/2 frames identical"))

    def test_difference_beyond_bound_fails_and_names_the_metric(self) -> None:
        a = self.frames("a.json", {"m": 1.0, "n": 5.0})
        b = self.frames("b.json", {"m": 1.0, "n": 5.25})
        rc, text = run_helper("compare", a, b, "0.1")
        self.assertEqual(rc, 1)
        self.assertEqual(text, "max 0.25 (n), 0/1 frames identical")

    def test_difference_at_the_bound_passes(self) -> None:
        a = self.frames("a.json", {"m": 1.0})
        b = self.frames("b.json", {"m": 1.5})
        self.assertEqual(run_helper("compare", a, b, "0.5")[0], 0)

    def test_listed_keys_limit_the_comparison(self) -> None:
        a = self.frames("a.json", {"m": 1.0, "n": 0.0})
        b = self.frames("b.json", {"m": 1.0, "n": 9.0})
        self.assertEqual(run_helper("compare", a, b, "0", "m")[0], 0)

    def test_missing_key_fails_even_within_bound(self) -> None:
        a = self.frames("a.json", {"integer_aim": 1.0, "integer_adm2": 0.9})
        b = self.frames("b.json", {"integer_adm2": 0.9})
        rc, text = run_helper("compare", a, b, "1", "integer_aim,integer_adm2")
        self.assertEqual(rc, 1)
        self.assertIn("missing integer_aim", text)

    def test_null_compares_as_zero_like_the_rows(self) -> None:
        a = self.frames("a.json", {"m": None})
        b = self.frames("b.json", {"m": 0.0})
        self.assertEqual(run_helper("compare", a, b, "0")[0], 0)

    def test_nan_is_never_within_a_bound(self) -> None:
        a = self.frames("a.json", {"m": 1.0})
        path = self.dir / "b.json"
        path.write_text('{"frames": [{"metrics": {"m": NaN}}]}', encoding="utf-8")
        rc, text = run_helper("compare", a, str(path), "1e300")
        self.assertEqual(rc, 1)
        self.assertIn("max inf", text)

    def test_frame_count_mismatch_is_an_error(self) -> None:
        a = self.frames("a.json", {"m": 1.0}, {"m": 1.0})
        b = self.frames("b.json", {"m": 1.0})
        self.assertEqual(run_helper("compare", a, b, "0")[0], 2)

    def test_unreadable_file_is_an_error(self) -> None:
        rc, text = run_helper("compare", str(self.dir / "absent.json"), "x", "0")
        self.assertEqual(rc, 2)
        self.assertIn("FileNotFoundError", text)

    def test_json_after_banner_text_is_found(self) -> None:
        a = self.write("a.json", {"frames": [{"metrics": {"m": 1.0}}]}, prefix="VMAF 1.0\n")
        self.assertEqual(run_helper("compare", a, a, "0")[0], 0)

    def test_twin_on_backend_passes(self) -> None:
        path = self.write(
            "b.json", {"feature_backends": [{"extractor": "psnr_hvs_cuda", "backend": "cuda"}]}
        )
        self.assertEqual(run_helper("backends", path, "cuda", "psnr_hvs_cuda")[0], 0)

    def test_cpu_fallback_fails_the_twin_check(self) -> None:
        path = self.write(
            "b.json", {"feature_backends": [{"extractor": "float_ssim", "backend": "cpu"}]}
        )
        rc, text = run_helper("backends", path, "cuda", "float_ssim_cuda")
        self.assertEqual(rc, 1)
        self.assertIn("lacks float_ssim_cuda", text)

    def test_image_needs_sycl_and_the_bound(self) -> None:
        cpu = self.write("c.json", {"pooled_metrics": {"vmaf": {"mean": 82.816058}}})
        sycl = self.write(
            "s.json",
            {"backend_used": "sycl", "pooled_metrics": {"vmaf": {"mean": 82.816059}}},
        )
        fallback = self.write(
            "f.json",
            {"backend_used": "cpu", "pooled_metrics": {"vmaf": {"mean": 82.816058}}},
        )
        self.assertEqual(run_helper("image", cpu, sycl, "5e-5")[0], 0)
        self.assertEqual(run_helper("image", cpu, sycl, "1e-7")[0], 1)
        self.assertEqual(run_helper("image", cpu, fallback, "5e-5")[0], 1)

    def test_msframe_is_the_median_of_pair_differences(self) -> None:
        # (short, long) pairs: 20 frames apart, 100 / 300 / 200 ms apart.
        samples = ["1000000", "101000000", "0", "300000000", "5", "200000005"]
        self.assertEqual(run_helper("msframe", "2", "22", *samples), (0, "10.00"))

    def test_msframe_rejects_an_odd_sample_count(self) -> None:
        self.assertEqual(run_helper("msframe", "2", "22", "1", "2", "3")[0], 2)

    def test_msframe_rejects_long_not_above_short(self) -> None:
        self.assertEqual(run_helper("msframe", "22", "22", "1", "2")[0], 2)

    def test_speedsum_keeps_the_worst_output_timing_and_errors(self) -> None:
        path = self.dir / "sgp.txt"
        path.write_text(
            "576x324 speed_chroma_u: bit-identical 48/48, max abs diff 0.000e+00\n"
            "576x324 speed_chroma_v: bit-identical 40/48, max abs diff 3.000e-06\n"
            "576x324 speed_chroma: cpu16 1.20 ms/frame, cuda 0.40 ms/frame\n"
            "speed_gpu_parity: Command failed with exit code 1\n",
            encoding="utf-8",
        )
        rc, text = run_helper("speedsum", str(path))
        self.assertEqual(rc, 0)
        self.assertIn("576x324: max 3e-06 (speed_chroma_v), 40/48 frames identical", text)
        self.assertIn("ms/frame 576x324 speed_chroma cuda 0.40 / cpu16 1.20", text)
        self.assertIn("error: Command failed with exit code 1", text)

    def test_buildopts_reads_the_meson_options(self) -> None:
        info = self.dir / "meson-info"
        info.mkdir()
        options = [
            {"name": "enable_hip", "value": True},
            {"name": "enable_hipcc", "value": False},
            {"name": "enable_cuda", "value": True},
        ]
        (info / "intro-buildoptions.json").write_text(json.dumps(options), encoding="utf-8")
        self.assertEqual(run_helper("buildopts", str(self.dir), "cuda")[0], 0)
        self.assertEqual(run_helper("buildopts", str(self.dir), "hip")[0], 1)
        self.assertEqual(run_helper("buildopts", str(self.dir), "sycl")[0], 1)

    def test_table_escapes_pipes(self) -> None:
        path = self.dir / "summary.tsv"
        path.write_text("T-X-2026-09-30\tcuda\tFAIL\ta | b\n", encoding="utf-8")
        rc, text = run_helper("table", str(path))
        self.assertEqual(rc, 0)
        self.assertIn("| `T-X-2026-09-30` | cuda | FAIL | a \\| b |", text)

    def test_unknown_subcommand_is_a_usage_error(self) -> None:
        self.assertEqual(run_helper("nope")[0], 2)
        self.assertEqual(run_helper()[0], 2)


class ScriptTests(unittest.TestCase):
    def test_every_entry_names_a_state_row(self) -> None:
        state = STATE.read_text(encoding="utf-8")
        entries = listed_entries()
        self.assertGreater(len(entries), 0)
        for row, backend in entries:
            with self.subTest(row=row, backend=backend):
                self.assertRegex(row, ROW_ID)
                self.assertIn(backend, ("cuda", "hip", "sycl"))
                self.assertIn(f"| **{row}**", state)

    def test_backend_and_row_filters_narrow_the_list(self) -> None:
        cuda = run_script("--list", "--backend", "cuda")
        self.assertEqual(cuda.returncode, 0)
        self.assertTrue(cuda.stdout.strip())
        self.assertTrue(all(line.split()[1] == "cuda" for line in cuda.stdout.splitlines()))
        row = listed_entries()[0][0]
        only = run_script("--list", "--only", row)
        self.assertEqual({line.split()[0] for line in only.stdout.splitlines()}, {row})

    def test_bad_arguments_exit_2(self) -> None:
        for argv in (
            ("--backend", "metal"),
            ("--only", "T-NOT-A-ROW-2026-09-30"),
            ("--reps", "0"),
            ("--threads", "x"),
            ("--cuda-device", "-1"),
            ("--baseline", "/nonexistent/rc3-baseline"),
            ("--build-dir",),
            ("--frobnicate",),
        ):
            with self.subTest(argv=argv):
                self.assertEqual(run_script(*argv).returncode, 2)

    def test_help_prints_the_usage(self) -> None:
        result = run_script("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--baseline DIR", result.stdout)

    def test_dry_run_prints_the_rows_commands_without_devices(self) -> None:
        row = "T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29"
        with TemporaryDirectory() as out:
            result = run_script(
                "--dry-run",
                "--out",
                out,
                "--only",
                row,
                "--backend",
                "cuda",
                "--cuda-device",
                "0",
                "--build-dir",
                "cuda=/nonexistent/build-cuda",
            )
            summary = (Path(out) / "summary.tsv").read_text(encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CUDA_VISIBLE_DEVICES=0 /nonexistent/build-cuda/tools/vmaf", result.stderr)
        self.assertIn(
            "--backend cuda --no_prediction --feature motion --precision=max", result.stderr
        )
        self.assertIn("--backend cpu --no_prediction --feature motion", result.stderr)
        self.assertEqual(summary.split("\t")[:3], [row, "cuda", "DRY-RUN"])


class FakeDeviceRunTests(unittest.TestCase):
    """The real (not --dry-run) path, with a fake vmaf and fake fixtures."""

    ROW = "T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29"

    def setUp(self) -> None:
        self.tmp = TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        tools = self.dir / "build/tools"
        tools.mkdir(parents=True)
        vmaf = tools / "vmaf"
        vmaf.write_text(FAKE_VMAF, encoding="utf-8")
        vmaf.chmod(0o755)
        info = self.dir / "build/meson-info"
        info.mkdir()
        (info / "intro-buildoptions.json").write_text(
            json.dumps([{"name": "enable_cuda", "value": True}]), encoding="utf-8"
        )
        fixtures = self.dir / "fixtures"
        fixtures.mkdir()
        for name in (
            "src01_hrc00_576x324.yuv",
            "src01_hrc01_576x324.yuv",
            "ref_3840x2160_200f.yuv",
            "dis_3840x2160_200f.yuv",
        ):
            (fixtures / name).touch()
        self.log = self.dir / "vmaf.log"
        self.env = dict(os.environ, FAKE_VMAF_LOG=str(self.log))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def kit(self, out: str, *extra: str, **env: str) -> subprocess.CompletedProcess[str]:
        return run_script(
            "--backend",
            "cuda",
            "--only",
            self.ROW,
            "--cuda-device",
            "7",
            "--build-dir",
            f"cuda={self.dir / 'build'}",
            "--netflix-dir",
            str(self.dir / "fixtures"),
            "--bbb-dir",
            str(self.dir / "fixtures"),
            "--lock-dir",
            str(self.dir / "locks"),
            "--reps",
            "1",
            "--out",
            str(self.dir / out),
            *extra,
            env=dict(self.env, **env),
        )

    def summary(self, out: str) -> list[str]:
        return (self.dir / out / "summary.tsv").read_text(encoding="utf-8").split("\t")

    def test_matching_twin_passes_pins_the_device_and_releases_the_lock(self) -> None:
        result = self.kit("run")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row, backend, status, notes = self.summary("run")
        self.assertEqual((row, backend, status), (self.ROW, "cuda", "PASS"))
        self.assertIn("576x324 cuda vs cpu: max 0, 1/1 frames identical", notes)
        self.assertIn(
            "feature_backends", (self.dir / "run" / self.ROW / "cuda/log.txt").read_text()
        )
        self.assertIn("ms/frame 3840x2160 (2 vs 22 frames, median of 1)", notes)
        runs = self.log.read_text(encoding="utf-8").splitlines()
        self.assertIn("backend=cuda cuda=7", runs)
        self.assertTrue(all(line.startswith("backend=c") for line in runs))
        # Two parity runs plus four timed runs on each side.
        self.assertEqual(runs.count("backend=cuda cuda=7"), 6)
        with (self.dir / "locks/cuda-4090.lock").open("a") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_twin_beyond_the_rows_bound_fails(self) -> None:
        result = self.kit("run", "--no-timing", FAKE_GPU_VALUE="30.001")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        _, _, status, notes = self.summary("run")
        self.assertEqual(status, "FAIL")
        self.assertIn("max 0.001 (psnr_hvs), 0/1 frames identical (bound 5e-4)", notes)

    def test_cpu_fallback_fails_the_twin_check(self) -> None:
        result = self.kit("run", "--no-timing", FAKE_TWIN="psnr_hvs")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("feature_backends lacks psnr_hvs_cuda", self.summary("run")[3])

    def test_baseline_difference_fails_a_row_that_needs_identical_output(self) -> None:
        self.assertEqual(self.kit("before", "--no-timing").returncode, 0)
        result = self.kit(
            "after", "--no-timing", "--baseline", str(self.dir / "before"), FAKE_GPU_VALUE="30.0001"
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(
            f"{self.ROW}/cuda/nf-cuda.json vs baseline: max 0.0001", self.summary("after")[3]
        )

    def test_missing_fixture_is_an_error(self) -> None:
        (self.dir / "fixtures/ref_3840x2160_200f.yuv").unlink()
        result = self.kit("run", "--no-timing")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.summary("run")[2], "ERROR")
        self.assertFalse(self.log.exists())

    def test_build_without_the_backend_is_skipped(self) -> None:
        (self.dir / "build/meson-info/intro-buildoptions.json").write_text(
            json.dumps([{"name": "enable_cuda", "value": False}]), encoding="utf-8"
        )
        result = self.kit("run", "--no-timing")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.summary("run")[2], "SKIP")


if __name__ == "__main__":
    unittest.main()

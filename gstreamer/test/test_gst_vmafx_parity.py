#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The vmafx GStreamer element scores what the vmaf CLI scores (RC4 WP9, #2236).

Builds nothing. Environment (set by run.sh):

* ``GST_VMAFX_PLUGIN_DIR``  directory holding libgstvmafx.so (CPU build);
* ``VMAFX_LIB_DIR``         directory holding the libvmafx it links;
* ``VMAF_CLI``              the vmaf CLI of the same library build;
* ``VMAFX_YUV_DIR``         python/test/resource/yuv (fixtures; tests skip without);
* ``VMAFX_BBB_DIR``         testdata/bbb;
* ``GST_VMAFX_CUDA_PLUGIN_DIR``, ``VMAFX_CUDA_LIB_DIR``, ``VMAF_CUDA_CLI``  the CUDA build
  (the CUDA tests skip without them and run under the device lock);
* ``GST_VMAFX_REPORT``      file the parity table is appended to.

Every per-frame metric and every pooled value of the element's report equals the CLI's at
``--precision max``; windows equal the engine's pooling of the CLI's per-frame scores over the same
ranges (scripts/ci/vmafx_window_pooling.py, one implementation).
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "ci"))
import vmafx_window_pooling as pooling  # noqa: E402

MODEL = "vmaf_v0.6.1"
POOL_ALL = "min+max+mean+harmonic_mean+median+perc5+perc10+perc20"
POOL_CLI = "min+max+mean+harmonic_mean"
CUDA_LOCK = Path.home() / ".cache" / "vmafx-locks" / "cuda-4090.lock"
BBB_FRAMES = 20

PAIRS = [
    ("golden", "src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None),
    (
        "checker-1px",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_1_0.yuv",
        1920,
        1080,
        None,
    ),
    (
        "checker-10px",
        "checkerboard_1920_1080_10_3_0_0.yuv",
        "checkerboard_1920_1080_10_3_10_0.yuv",
        1920,
        1080,
        None,
    ),
    ("bbb-4k-20f", "ref_3840x2160_200f.yuv", "dis_3840x2160_200f.yuv", 3840, 2160, BBB_FRAMES),
]
TABLE: list[str] = []


def env_path(name: str) -> Path | None:
    value = os.environ.get(name)
    return Path(value) if value else None


def run(cmd, env=None, timeout=600):
    return subprocess.run(  # noqa: S603 -- the test's own build outputs, argument list, no shell
        [str(c) for c in cmd],
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def gst_env(plugin_dir: Path, lib_dir: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["GST_PLUGIN_PATH"] = str(plugin_dir)
    env["LD_LIBRARY_PATH"] = f"{lib_dir}:{env.get('LD_LIBRARY_PATH', '')}"
    env["GST_REGISTRY"] = str(Path(tempfile.gettempdir()) / f"gst-vmafx-test-{os.getuid()}.bin")
    return env


def raw_branch(path: Path, w: int, h: int, pad: str, fmt: str = "i420", extra: str = "") -> str:
    return (
        f"filesrc location={path} ! rawvideoparse format={fmt} width={w} height={h} "
        f"framerate=24/1 {extra} ! v.{pad}"
    )


def gst_launch(env, ref, dist, w, h, props: str, messages=False, extra=("", ""), timeout=600):
    """One scoring pipeline; returns the CompletedProcess."""
    pipeline = (
        f"{raw_branch(ref, w, h, 'reference', extra=extra[0])} "
        f"{raw_branch(dist, w, h, 'distorted', extra=extra[1])} "
        f"vmafx name=v {props} ! fakesink"
    )
    cmd = ["gst-launch-1.0", "-m" if messages else "-q", *pipeline.split()]
    return run(cmd, env, timeout)


def cli_run(cli, ref, dist, w, h, out, backend=None):
    cmd = [cli, "-r", ref, "-d", dist, "-w", w, "-h", h, "-p", "420", "-b", "8"]
    cmd += ["--model", f"version={MODEL}", "--precision", "max", "--json", "-o", out, "-q"]
    if backend:
        cmd += ["--backend", backend]
    result = run(cmd)
    if result.returncode != 0:
        raise AssertionError(f"vmaf CLI failed: {result.stdout}{result.stderr}")
    return json.loads(Path(out).read_text(encoding="utf-8"))


def truncated(src: Path, dst: Path, w: int, h: int, frames: int) -> Path:
    size = w * h * 3 // 2 * frames
    with src.open("rb") as fin, dst.open("wb") as fout:
        fout.write(fin.read(size))
    return dst


def compare(element: dict, cli: dict, pooled_keys=("min", "max", "mean", "harmonic_mean")):
    """(metrics compared, max abs diff, identical) of frames and pooled values."""
    compared, worst, same = 0, 0.0, True
    assert len(element["frames"]) == len(cli["frames"]), "frame counts differ"
    for ef, cf in zip(element["frames"], cli["frames"], strict=True):
        assert ef["metrics"].keys() == cf["metrics"].keys(), "metric names differ"
        for key, value in cf["metrics"].items():
            compared += 1
            diff = abs(ef["metrics"][key] - value)
            worst, same = max(worst, diff), same and ef["metrics"][key] == value
    for key, methods in cli["pooled_metrics"].items():
        for method in pooled_keys:
            compared += 1
            ev = element["pooled_metrics"][key][method]
            worst, same = max(worst, abs(ev - methods[method])), same and ev == methods[method]
    return compared, worst, same


class Fixture(unittest.TestCase):
    plugin_dir = env_path("GST_VMAFX_PLUGIN_DIR")
    lib_dir = env_path("VMAFX_LIB_DIR")
    cli = env_path("VMAF_CLI")
    yuv = env_path("VMAFX_YUV_DIR")
    bbb = env_path("VMAFX_BBB_DIR")

    @classmethod
    def setUpClass(cls):
        if not (cls.plugin_dir and cls.lib_dir and cls.cli):
            raise unittest.SkipTest("GST_VMAFX_PLUGIN_DIR, VMAFX_LIB_DIR, VMAF_CLI not set")
        cls.env = gst_env(cls.plugin_dir, cls.lib_dir)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def pair(self, ref: str, dist: str, w: int, h: int, frames):
        base = self.bbb if ref.startswith("ref_") else self.yuv
        if base is None or not (base / ref).is_file() or not (base / dist).is_file():
            self.skipTest(f"fixture {ref} / {dist} missing")
        if frames:
            return (
                truncated(base / ref, self.dir / f"r-{ref}", w, h, frames),
                truncated(base / dist, self.dir / f"d-{dist}", w, h, frames),
            )
        return base / ref, base / dist


class ParityCpu(Fixture):
    def test_reports_equal_the_cli(self):
        for name, ref, dist, w, h, frames in PAIRS:
            with self.subTest(pair=name):
                r, d = self.pair(ref, dist, w, h, frames)
                out = self.dir / f"gst-{name}.json"
                props = f"model=version={MODEL} log-path={out} score-fmt=%.17g pool={POOL_CLI}"
                done = gst_launch(self.env, r, d, w, h, props)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                cli = cli_run(self.cli, r, d, w, h, self.dir / f"cli-{name}.json")
                element = json.loads(out.read_text(encoding="utf-8"))
                n, worst, same = compare(element, cli)
                TABLE.append(
                    f"| {name} | cpu | {len(cli['frames'])} | {n} | {'yes' if same else 'NO'} | "
                    f"{worst:.3g} |"
                )
                self.assertTrue(same, f"{name}: max abs diff {worst}")
                self.assertIn("provenance", element)

    def test_comparison_refuses_a_lossy_report(self):
        """Negative case: the same run at %.6f is not the CLI's %.17g, and compare() says so."""
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "lossy.json"
        props = f"model=version={MODEL} log-path={out} score-fmt=%.6f pool={POOL_CLI}"
        done = gst_launch(self.env, r, d, 576, 324, props)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        cli = cli_run(self.cli, r, d, 576, 324, self.dir / "cli-lossy.json")
        _, worst, same = compare(json.loads(out.read_text(encoding="utf-8")), cli)
        self.assertFalse(same)
        self.assertGreater(worst, 0.0)

    def test_nv12_input_equals_planar(self):
        """NV12 is the same pixels interleaved: the imported frames score as the planar ones."""
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "nv12.json"
        nv12 = "! videoconvert ! video/x-raw,format=NV12"
        props = f"model=version={MODEL} log-path={out} score-fmt=%.17g pool={POOL_CLI}"
        done = gst_launch(self.env, r, d, 576, 324, props, extra=(nv12, nv12))
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        cli = cli_run(self.cli, r, d, 576, 324, self.dir / "cli-nv12.json")
        n, worst, same = compare(json.loads(out.read_text(encoding="utf-8")), cli)
        TABLE.append(f"| golden-nv12 | cpu | 48 | {n} | {'yes' if same else 'NO'} | {worst:.3g} |")
        self.assertTrue(same, f"max abs diff {worst}")

    def test_provenance_message_and_report(self):
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "prov.json"
        done = gst_launch(self.env, r, d, 576, 324, f"log-path={out}", messages=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("vmafx-provenance", done.stdout)
        self.assertIn("vmafx-summary", done.stdout)
        report = json.loads(out.read_text(encoding="utf-8"))
        self.assertIsInstance(report["provenance"], dict)

    def test_frame_messages(self):
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        props = f"model=version={MODEL} metadata=true score-fmt=%.17g"
        done = gst_launch(self.env, r, d, 576, 324, props, messages=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(re.findall(r"vmafx-frame, index=\(guint64\)\d+", done.stdout)), 48)


class Locale(Fixture):
    def test_decimal_comma_locale(self):
        """gst-launch sets the user's locale; the library parses option numbers in the C locale."""
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "locale.json"
        env = dict(self.env, LC_ALL="de_DE.UTF-8", LANG="de_DE.UTF-8")
        done = gst_launch(env, r, d, 576, 324, f"model=version=vmaf_v1.0.16_3d0h log-path={out}")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertTrue(out.is_file())


class Windows(Fixture):
    def run_windows(self, knob: str):
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        stats = self.dir / f"stats-{knob.split('=', 1)[0]}.ndjson"
        props = (
            f"model=version={MODEL} score-fmt=%.17g pool={POOL_ALL} {knob} "
            f"stats-out=file+log+metadata stats-path={stats}"
        )
        done = gst_launch(self.env, r, d, 576, 324, props, messages=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        cli = cli_run(self.cli, r, d, 576, 324, self.dir / "cli-win.json")
        lines = [json.loads(x) for x in stats.read_text(encoding="utf-8").splitlines()]
        return cli, lines, done.stdout

    def check(self, cli, windows, groups):
        """`groups`: the frame index ranges the windows must cover, in order."""
        scores = [f["metrics"]["vmaf"] for f in cli["frames"]]
        self.assertEqual(len(windows), len(groups))
        for k, (w, (first, last)) in enumerate(zip(windows, groups, strict=True)):
            part = scores[first : last + 1]
            self.assertEqual(w["window"], k)
            self.assertEqual(w["n_frames"], len(part))
            self.assertEqual(w["n_scored"], len(part))
            self.assertEqual(w["partial"], k == len(groups) - 1 and self.tail_partial)
            self.assertEqual(w["vmaf"], pooling.pooled_by_name(part), f"window {k}")
        self.assertEqual(sum(w["n_frames"] for w in windows), len(scores))

    tail_partial = False

    def test_frame_windows_with_partial_tail(self):
        cli, windows, out = self.run_windows("n-stats-frames=10")
        self.tail_partial = True
        self.check(cli, windows, [(0, 9), (10, 19), (20, 29), (30, 39), (40, 47)])
        self.assertTrue(windows[-1]["partial"])
        self.assertEqual(len(re.findall(r"vmafx-window,", out)), len(windows))

    def test_time_windows(self):
        cli, windows, _ = self.run_windows("n-stats=0.5")
        # rawvideoparse stamps frame i with i * floor(1e9 / 24) ns, so frame 12 (0.5 s) is stamped
        # 499999992 ns and falls into the first window: the clock cuts on the stamps, not the rate.
        buckets: dict[int, list[int]] = {}
        for i in range(48):
            buckets.setdefault(i * (10**9 // 24) // (5 * 10**8), []).append(i)
        groups = [(v[0], v[-1]) for _, v in sorted(buckets.items())]
        self.tail_partial = True
        self.check(cli, windows, groups)
        self.assertAlmostEqual(windows[1]["start"], 0.5)
        self.assertAlmostEqual(windows[1]["end"], 1.0)

    def test_default_model_windows(self):
        """Windows over the default model in a decimal-comma locale: its feature names carry a
        fractional option, which the library formats in the C locale
        (T-OPTION-NUMBERS-CALLER-LOCALE-2026-10-06)."""
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        env = dict(self.env, LC_ALL="de_DE.UTF-8", LANG="de_DE.UTF-8")
        done = gst_launch(env, r, d, 576, 324, "n-stats-frames=8")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


class Refusals(Fixture):
    def launch(self, props, fmt="i420"):
        r = self.yuv / "src01_hrc00_576x324.yuv" if self.yuv else None
        if r is None or not r.is_file():
            self.skipTest("golden fixture missing")
        d = self.yuv / "src01_hrc01_576x324.yuv"
        return gst_launch(self.env, r, d, 576, 324, props, extra=("", ""))

    def test_backend_without_import_lane(self):
        done = self.launch("backend=sycl")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("backend sycl", done.stdout + done.stderr)

    def test_layout_refused_at_caps(self):
        done = self.launch("import=device")
        self.assertNotEqual(done.returncode, 0)
        text = done.stdout + done.stderr
        self.assertIn("format I420", text)
        self.assertIn("import=device", text)

    def test_gl_memory_refused_by_name(self):
        """GLMemory (nvh264dec's second output, VA decoders') fails at caps, never downloads."""
        branch = (
            "videotestsrc num-buffers=2 ! video/x-raw,format=NV12,width=320,height=240 "
            "! glupload ! video/x-raw(memory:GLMemory) ! v.{pad}"
        )
        pipeline = f"{branch.format(pad='distorted')} {branch.format(pad='reference')} vmafx name=v ! fakesink"
        done = run(["gst-launch-1.0", "-q", *pipeline.split()], self.env)
        text = done.stdout + done.stderr
        if "no element" in text or "Kein solches" in text:
            self.skipTest("glupload not available")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("format NV12 in GLMemory", text)

    def test_invalid_pool_value(self):
        text = self.launch("backend=cpu pool=nonsense").stderr
        self.assertIn("pool", text)

    def test_invalid_score_format(self):
        done = self.launch("score-fmt=%d")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("score-fmt", done.stdout + done.stderr)


class Cuda(Fixture):
    plugin_dir = env_path("GST_VMAFX_CUDA_PLUGIN_DIR")
    lib_dir = env_path("VMAFX_CUDA_LIB_DIR")
    cli = env_path("VMAF_CUDA_CLI")

    def locked(self, cmd, env=None):
        wrapped = ["flock", str(CUDA_LOCK), "timeout", "300", *[str(c) for c in cmd]]
        return run(wrapped, env, timeout=420)

    def element_cuda(self, r, d, w, h, out):
        pipeline = (
            f"filesrc location={r} ! rawvideoparse format=i420 width={w} height={h} "
            f"framerate=24/1 ! cudaupload ! video/x-raw(memory:CUDAMemory) ! v.reference "
            f"filesrc location={d} ! rawvideoparse format=i420 width={w} height={h} "
            f"framerate=24/1 ! cudaupload ! video/x-raw(memory:CUDAMemory) ! v.distorted "
            f"vmafx name=v backend=cuda model=version={MODEL} log-path={out} "
            f"score-fmt=%.17g pool={POOL_CLI} ! fakesink"
        )
        return self.locked(["gst-launch-1.0", "-m", *pipeline.split()], self.env)

    def test_nv12_cuda_memory(self):
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "cuda-nv12.json"
        nv12 = (
            "! videoconvert ! video/x-raw,format=NV12 ! cudaupload ! video/x-raw(memory:CUDAMemory)"
        )
        pipeline = (
            f"{raw_branch(r, 576, 324, 'reference', extra=nv12)} "
            f"{raw_branch(d, 576, 324, 'distorted', extra=nv12)} "
            f"vmafx name=v backend=cuda model=version={MODEL} log-path={out} "
            f"score-fmt=%.17g pool={POOL_CLI} ! fakesink"
        )
        done = self.locked(["gst-launch-1.0", "-m", *pipeline.split()], self.env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertRegex(done.stdout, r"host-copy-frames=\(guint64\)0")
        cmd = [self.cli, "-r", r, "-d", d, "-w", 576, "-h", 324, "-p", "420", "-b", "8",
               "--model", f"version={MODEL}", "--precision", "max", "--json",
               "-o", self.dir / "clicuda-nv12.json", "-q", "--backend", "cuda"]  # fmt: skip
        self.assertEqual(self.locked(cmd).returncode, 0)
        cli = json.loads((self.dir / "clicuda-nv12.json").read_text(encoding="utf-8"))
        n, worst, same = compare(json.loads(out.read_text(encoding="utf-8")), cli)
        TABLE.append(f"| golden-nv12 | cuda | 48 | {n} | {'yes' if same else 'NO'} | {worst:.3g} |")
        self.assertTrue(same, f"max abs diff {worst}")

    def test_system_memory_on_cuda_backend(self):
        """backend=cuda with system-memory frames: the CUDA context uploads them."""
        r, d = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out = self.dir / "cuda-sysmem.json"
        pipeline = (
            f"{raw_branch(r, 576, 324, 'reference')} {raw_branch(d, 576, 324, 'distorted')} "
            f"vmafx name=v backend=cuda model=version={MODEL} log-path={out} "
            f"score-fmt=%.17g pool={POOL_CLI} ! fakesink"
        )
        done = self.locked(["gst-launch-1.0", "-m", *pipeline.split()], self.env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        cmd = [self.cli, "-r", r, "-d", d, "-w", 576, "-h", 324, "-p", "420", "-b", "8",
               "--model", f"version={MODEL}", "--precision", "max", "--json",
               "-o", self.dir / "clicuda-sysmem.json", "-q", "--backend", "cuda"]  # fmt: skip
        self.assertEqual(self.locked(cmd).returncode, 0)
        report = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(report["provenance"]["active_backend"], "cuda")
        cli = json.loads((self.dir / "clicuda-sysmem.json").read_text(encoding="utf-8"))
        n, worst, same = compare(report, cli)
        TABLE.append(
            f"| golden-sysmem | cuda | 48 | {n} | {'yes' if same else 'NO'} | {worst:.3g} |"
        )
        self.assertTrue(same, f"max abs diff {worst}")

    def encode_time_pipeline(
        self, ref: Path, out: Path, stats: Path, ref_nv12: Path, dec_nv12: Path
    ):
        cuda = "video/x-raw(memory:CUDAMemory)"
        pipeline = (
            f"filesrc location={ref} ! rawvideoparse format=i420 width=576 height=324 "
            f"framerate=24/1 ! videoconvert ! video/x-raw,format=NV12 ! tee name=t "
            f"t. ! queue ! filesink location={ref_nv12} "
            f"t. ! queue ! cudaupload ! {cuda} ! tee name=u "
            f"u. ! queue ! v.reference "
            f"u. ! queue ! nvh264enc bitrate=2000 ! h264parse ! nvh264dec ! {cuda} ! tee name=d "
            f"d. ! queue ! v.distorted "
            f"d. ! queue ! cudadownload ! videoconvert ! video/x-raw,format=NV12,width=576,height=324 ! filesink location={dec_nv12} "
            f"vmafx name=v backend=cuda model=version={MODEL} log-path={out} score-fmt=%.17g "
            f"pool={POOL_ALL} n-stats-frames=8 stats-out=file stats-path={stats} ! fakesink"
        )
        return self.locked(["gst-launch-1.0", "-m", *pipeline.split()], self.env)

    def test_encode_time_scoring_equals_files(self):
        """tee -> nvh264enc -> nvh264dec (CUDAMemory) -> distorted, reference from the tee."""
        r, _ = self.pair("src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, None)
        out, stats = self.dir / "enc.json", self.dir / "enc.ndjson"
        ref_nv12, dec_nv12 = self.dir / "ref.nv12", self.dir / "dec.nv12"
        done = self.encode_time_pipeline(r, out, stats, ref_nv12, dec_nv12)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertRegex(done.stdout, r"host-copy-frames=\(guint64\)0")
        # the same frames scored from files, through the same element and backend
        out2, stats2 = self.dir / "files.json", self.dir / "files.ndjson"
        cuda = "video/x-raw(memory:CUDAMemory)"

        def branch(path, pad):
            return (
                f"filesrc location={path} ! rawvideoparse format=nv12 width=576 height=324 "
                f"framerate=24/1 ! cudaupload ! {cuda} ! v.{pad}"
            )

        pipeline = (
            f"{branch(ref_nv12, 'reference')} {branch(dec_nv12, 'distorted')} "
            f"vmafx name=v backend=cuda model=version={MODEL} log-path={out2} score-fmt=%.17g "
            f"pool={POOL_ALL} n-stats-frames=8 stats-out=file stats-path={stats2} ! fakesink"
        )
        done2 = self.locked(["gst-launch-1.0", "-q", *pipeline.split()], self.env)
        self.assertEqual(done2.returncode, 0, done2.stdout + done2.stderr)
        live = json.loads(out.read_text(encoding="utf-8"))
        files = json.loads(out2.read_text(encoding="utf-8"))
        self.assertEqual(len(live["frames"]), 48)
        n, worst, same = compare(live, files)
        TABLE.append(
            f"| encode-time h264 | cuda vs files | 48 | {n} | {'yes' if same else 'NO'} | {worst:.3g} |"
        )
        self.assertTrue(same, f"max abs diff {worst}")
        # nvh264enc stamps from 1 h: the windows' times differ by that offset, nothing else does
        win = [json.loads(x) for x in stats.read_text(encoding="utf-8").splitlines()]
        win2 = [json.loads(x) for x in stats2.read_text(encoding="utf-8").splitlines()]
        for a, b in zip(win, win2, strict=True):
            self.assertAlmostEqual(a["end"] - a["start"], b["end"] - b["start"], places=6)
            for key in ("start", "end"):
                del a[key], b[key]
            self.assertEqual(a, b)
        self.assertEqual(len(stats.read_text(encoding="utf-8").splitlines()), 6)

    def test_cuda_memory_equals_cuda_cli(self):
        for name, ref, dist, w, h, frames in PAIRS:
            with self.subTest(pair=name):
                r, d = self.pair(ref, dist, w, h, frames)
                out = self.dir / f"cuda-{name}.json"
                done = self.element_cuda(r, d, w, h, out)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertRegex(done.stdout, r"host-copy-frames=\(guint64\)0")
                cmd = [
                    self.cli, "-r", r, "-d", d, "-w", w, "-h", h, "-p", "420", "-b", "8",
                    "--model", f"version={MODEL}", "--precision", "max", "--json",
                    "-o", self.dir / f"clicuda-{name}.json", "-q", "--backend", "cuda",
                ]  # fmt: skip
                cli_done = self.locked(cmd)
                self.assertEqual(cli_done.returncode, 0, cli_done.stdout + cli_done.stderr)
                cli = json.loads((self.dir / f"clicuda-{name}.json").read_text(encoding="utf-8"))
                element = json.loads(out.read_text(encoding="utf-8"))
                n, worst, same = compare(element, cli)
                TABLE.append(
                    f"| {name} | cuda | {len(cli['frames'])} | {n} | {'yes' if same else 'NO'} | "
                    f"{worst:.3g} |"
                )
                self.assertTrue(same, f"{name}: max abs diff {worst}")
                self.cpu_cli_diff(name, element, r, d, w, h)

    def cpu_cli_diff(self, name, element, r, d, w, h):
        """The CUDA element against the CPU CLI: exact where the twin is declared exact."""
        cpu_cli = Fixture.cli
        if cpu_cli is None:
            return
        cpu_cli = Path(os.environ["VMAF_CLI"])
        cpu = cli_run(cpu_cli, r, d, w, h, self.dir / f"cpu-{name}.json")
        exact, worst_exact, worst_other, bad = declared_exact_cuda(), 0.0, 0.0, []
        for ef, cf in zip(element["frames"], cpu["frames"], strict=True):
            for key, value in cf["metrics"].items():
                diff = abs(ef["metrics"][key] - value)
                if is_exact_metric(key, exact):
                    worst_exact = max(worst_exact, diff)
                    if diff:
                        bad.append(key)
                else:
                    worst_other = max(worst_other, diff)
        TABLE.append(
            f"| {name} | cuda vs cpu cli | {len(cpu['frames'])} | exact twins {worst_exact:.3g}, "
            f"others {worst_other:.3g} | {'yes' if not bad else 'NO'} | {worst_exact:.3g} |"
        )
        self.assertEqual(bad, [], f"{name}: declared-exact twins differ from the CPU")


def declared_exact_cuda() -> set[str]:
    """Feature names scripts/ci/exact_twins.d declares bit-identical on CUDA."""
    return {f.name[: -len(".cuda")] for f in (ROOT / "scripts/ci/exact_twins.d").glob("*.cuda")}


def is_exact_metric(key: str, exact: set[str]) -> bool:
    name = key.replace("VMAF_integer_feature_", "").replace("integer_", "")
    return any(name == n or name.startswith(n) for n in exact)


def tearDownModule():
    report = os.environ.get("GST_VMAFX_REPORT")
    text = "\n".join(
        ["| pair | backend | frames | values compared | identical | max abs diff |", *TABLE]
    )
    print("\n" + text)
    if report:
        with Path(report).open("a", encoding="utf-8") as fh:
            fh.write(text + "\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)

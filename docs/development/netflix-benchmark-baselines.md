<!-- markdownlint-disable MD013 -->

# Netflix benchmark baselines — how to reproduce them

This page documents the harness behind `testdata/netflix_benchmark_results.json`
(the fork's recorded per-backend score + throughput snapshot for the three
Netflix fixtures), how to re-run it, and what the last re-run measured.

It is **not** the Netflix golden-data gate. That gate is the Python
`assertAlmostEqual` suite invoked by `make test-netflix-golden`
([CLAUDE.md §8](../../CLAUDE.md)); its assertions are never edited.
`testdata/netflix_benchmark_results.json` is fork-added data governed by the
`/regen-snapshots` rule and by
[ADR-1192](../adr/1192-netflix-bench-snapshot-drift-not-regenerated.md).

## Which harness writes which file

There are three benchmark entry points under `testdata/` and they are easy to
confuse:

| Script | Path exercised | Fixtures | Writes |
| --- | --- | --- | --- |
| `testdata/benchmark_netflix.py` | FFmpeg `libvmaf` / `libvmaf_cuda` / `libvmaf_sycl` filters | src01 576x324 (48f), checkerboard 1080p mild + heavy (3f each) | `testdata/netflix_benchmark_results.json` |
| `testdata/bench_all.sh` | the `vmaf` **CLI** binary | src01 576x324, src01 1080p 5f, BBB 4K 200f | nothing (prints a report) |
| `testdata/bench_perf.py` | FFmpeg filters, portable/parameterised | BBB pairs | `testdata/perf_benchmark_results.json` (historical) |

`testdata/compare_combined.py` compares `scores_cpu_*.json` against
`scores_sycl_a380_*.json`. It does **not** read
`netflix_benchmark_results.json`, so it cannot be used to diff a fresh
`benchmark_netflix.py` run against the recorded snapshot — compare the JSON
directly (see [Diffing a fresh run](#diffing-a-fresh-run)).

## Prerequisites

`benchmark_netflix.py` drives an FFmpeg binary that links the fork's libvmaf and
carries all three filters. The supported way to get one is the
`vmaf-dev-mcp` container ([dev-mcp.md](dev-mcp.md)), which ships FFmpeg with
`libvmaf`, `libvmaf_cuda` and `libvmaf_sycl` already wired up. The YUV fixtures
live in `python/test/resource/yuv/` and are gitignored, so they exist only in a
full checkout — point `VMAF_YUVDIR` at that checkout when running from a
worktree.

To measure *current* libvmaf rather than the one baked into the image, build the
library out-of-source inside the container and put it ahead of the installed one
on `LD_LIBRARY_PATH`. FFmpeg resolves `libvmaf.so.3` at load time, so no FFmpeg
rebuild is needed as long as the public headers under `core/include/libvmaf/`
have not changed shape.

```bash
# 1. Build current master's libvmaf inside the container, out of source.
docker exec vmaf-dev-mcp bash -lc '
  set +u; source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1; set -u
  CC=icx CXX=icpx meson setup /tmp/bench-build /workspace/core \
      -Denable_cuda=true -Denable_sycl=true \
      -Denable_hip=false -Denable_metal=disabled -Denable_dnn=disabled \
      -Denable_mcp=false -Denable_tests=false \
      --buildtype=release -Db_lto=false -Dc_args=-march=native
  nice -n 10 ninja -C /tmp/bench-build -j 4'

# 2. Run the suite against it.
docker exec vmaf-dev-mcp bash -lc '
  set +u; source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1; set -u
  export LD_LIBRARY_PATH=/tmp/bench-build/src:$LD_LIBRARY_PATH
  export VMAF_FFMPEG=/usr/local/bin/ffmpeg
  export VMAF_YUVDIR=/workspace/python/test/resource/yuv
  export VMAF_SYCL_RENDER_NODE=$(ls /dev/dri/renderD*)   # see below
  mkdir -p /tmp/benchrun && cp /workspace/testdata/benchmark_netflix.py /tmp/benchrun/
  cd /tmp/benchrun && python3 benchmark_netflix.py'
```

The script writes its output JSON next to itself, so running the copy in
`/tmp/benchrun` keeps the tracked snapshot untouched. That is deliberate:
overwriting `testdata/netflix_benchmark_results.json` requires the
`/regen-snapshots` justification.

### Picking the SYCL render node

`benchmark_netflix.py` imports frames onto the Intel GPU through QSV/VA-API, so
it needs the render node that the **iHD** driver claims. Node numbering is not
stable across hosts or across PCI re-enumeration — on the `ryzen-4090-arc` bench
host the Arc A380 moved from `renderD130` to `renderD129`, and `renderD130` is
now the AMD iGPU (`radeonsi`), which makes the run fail with
`DRM_IOCTL_VERSION, unsupported drm device by media driver: amdg`. Find the
right node before running:

```bash
docker exec vmaf-dev-mcp bash -lc \
  'for n in /dev/dri/renderD*; do echo "== $n"; vainfo --display drm --device $n 2>&1 | grep -m1 "Driver version"; done'
```

Pick the node whose driver line reads `Intel iHD driver`, and pass it as
`VMAF_SYCL_RENDER_NODE` (or `--sycl-render-node`). `VMAF_FFMPEG` overrides the
FFmpeg binary; both defaults are host-specific and are expected to be set.

## Diffing a fresh run

```bash
python3 - <<'PY'
import json
rec = json.load(open("testdata/netflix_benchmark_results.json"))
new = json.load(open("/tmp/benchrun/netflix_benchmark_results.json"))
for fixture in rec:
    for backend in rec[fixture]:
        r, n = rec[fixture][backend], new.get(fixture, {}).get(backend)
        if not n or "frames" not in n:
            print(f"{fixture:22s} {backend:5s} MISSING in fresh run"); continue
        md = max(abs(a - b) for a, b in zip(r["frames"], n["frames"]))
        print(f"{fixture:22s} {backend:5s} pooled {r['pooled']:.6f} -> {n['pooled']:.6f} "
              f"(delta {n['pooled']-r['pooled']:+.2e}, max per-frame {md:.2e})")
PY
```

Any non-zero score delta is a **correctness** finding: open a `docs/state.md`
row, do not regenerate the snapshot. A throughput delta is a performance
observation and never justifies a rewrite on its own.

## Last re-run: 2026-09-06, commit `cd52f2670`

Host `ryzen-4090-arc`: AMD Ryzen 9 9950X3D (32 threads), 60 GiB RAM,
Linux 7.2.3-1-cachyos, NVIDIA RTX 4090 (driver 610.57.04), Intel Arc A380.
Toolchain inside `vmaf-dev-mcp` (Ubuntu 26.04): Intel oneAPI DPC++ 2026.1.1,
CUDA 13.3.73, FFmpeg n9.0.1-17-gfde3691, libvmaf built
`--buildtype=release -Db_lto=false -Dc_args=-march=native`.
The workstation was **not** idle: 1-minute load average 7–19 throughout, with a
container image build and other agent sessions running. Every figure below is
from a command run on that host; timing figures are medians of 5 repetitions
with the min/max spread quoted, and are not a substitute for a quiet-host
baseline.

### Reading the harness's own PASS/DIFF column

`benchmark_netflix.py` prints each pooled score against the Netflix reference
value lifted from `python/test/quality_runner_test.py`
(`src01_576x324` = 76.66890519623612). All three backends have always shown
`DIFF` on that row — the recorded snapshot's own CPU value, 76.667828, is
1.07e-3 below the reference too. The FFmpeg filter path and the Python harness
disagree by that much on this fixture; that gap is older than the snapshot and
is not what this page is about. The two checkerboard rows read `OK`. Compare
against the **snapshot**, not against that column, when asking whether master
has drifted.

### Scores versus the recorded snapshot

CUDA is quoted at its *modal* value; see the non-determinism note below.

| Fixture | Backend | Recorded pooled | 2026-09-06 pooled | Pooled delta | Max per-frame delta |
| --- | --- | --- | --- | --- | --- |
| `src01_576x324` | cpu | 76.667828 | 76.667831 | +2.83e-06 | 1.70e-05 |
| `src01_576x324` | cuda | 76.668903 | 76.667830 | -1.07e-03 | 2.85e-03 |
| `src01_576x324` | sycl | 76.669148 | 76.667746 | -1.40e-03 | 1.54e-02 |
| `checker_1080p_mild` | cpu | 35.068672 | 35.068671 | -6.67e-07 | 1.30e-05 |
| `checker_1080p_mild` | cuda | 35.068669 | 35.068667 | -2.33e-06 | 5.00e-06 |
| `checker_1080p_mild` | sycl | 35.068664 | 35.068628 | -3.60e-05 | 3.90e-05 |
| `checker_1080p_heavy` | cpu | 7.985899 | 7.985899 | 0 | 1.00e-06 |
| `checker_1080p_heavy` | cuda | 7.985899 | 7.985899 | 0 | 1.00e-06 |
| `checker_1080p_heavy` | sycl | 7.985899 | 7.985899 | 0 | 0 |

**None of these deltas come from the 2026-09-06 GPU merges.** Rebuilding
`5a080300e` — the commit immediately before #1307, #1312 and #1324 — with the
same flags gives CUDA 76.667830 and SYCL 76.667745 on the 576x324 pair, i.e.
the same drift versus the recorded snapshot. The three merges did move CPU
per-frame values by up to ~8e-6 (the pooled value is unchanged at six decimals)
and shrank the SYCL `frames[].metrics` key set from 35 keys to 24; the CUDA key
set is 14 on both commits. The recorded snapshot dates from PR #309
(2026-05-02), so the drift accumulated across four months.

### Throughput (FFmpeg filter path, 576x324 48f and 1080p 3f)

Whole-process wall time including FFmpeg start-up, matching how
`benchmark_netflix.py` computes fps. Median of 5, load average 9.6–9.8 during
the sweep. These numbers are **observations, not a recorded baseline** — no
throughput baseline is written while the CUDA path is non-deterministic
(ADR-1192).

| Fixture | Backend | Wall ms (median) | Wall ms (min–max) | fps (median) |
| --- | --- | --- | --- | --- |
| src01 576x324, 48f | cpu | 98 | 95–106 | 489.8 |
| src01 576x324, 48f | cuda | 185 | 180–200 | 259.5 |
| src01 576x324, 48f | sycl | 250 | 237–326 | 192.0 |
| checkerboard 1080p, 3f | cpu | 82 | 74–103 | 36.6 |
| checkerboard 1080p, 3f | cuda | 171 | 168–180 | 17.5 |
| checkerboard 1080p, 3f | sycl | 160 | 152–204 | 18.8 |

The CPU-beats-GPU ordering at these sizes is expected: 48 frames of 576x324 and
3 frames of 1080p do not amortise upload and launch cost. See
[benchmarks.md](../benchmarks.md) for the 4K numbers where CUDA dominates.

### Blockers found by this run

Two GPU defects were reproduced. Both are tracked in
[`docs/state.md`](../state.md) and both were confirmed present on `5a080300e`
as well, i.e. they are pre-existing rather than introduced on 2026-09-06.

1. **`vmaf --threads N` aborts on every GPU backend.**
   `--gpumask=0` (CUDA) or `--sycl_device=0` (SYCL) combined with any
   `--threads` value emits `feature "VMAF_integer_feature_motion2_score"
   cannot be overwritten at index N` for every frame, then
   `libvmaf ERROR context could not be synchronized` / `problem flushing
   context`, and exits 234 with no output file. Dropping `--threads`
   entirely makes both backends succeed and score correctly (CUDA 76.667830,
   SYCL 76.667746 on the 576x324 pair, 10/10 runs each).
   `testdata/bench_all.sh` hard-codes `--threads 1`, so **every CUDA and SYCL
   row it has ever printed on this host was a masked failure**, reported as
   `SKIP (… backend likely unavailable)` because the script also discarded
   stderr. It now keeps stderr in `$VMAF_BENCH_OUTDIR/<row>.err` and prints
   `FAIL (vmaf exited 234: problem flushing context; see …)`. Its flag sets
   also dropped `--no_vulkan`, which current CLI builds reject as unrecognized
   (ADR-0726 removed the Vulkan backend); the flag was ignored rather than
   fatal, so it had gone unnoticed.

2. **The `libvmaf_cuda` FFmpeg filter is non-deterministic.**
   On the 576x324 48-frame pair, 10 of 40 runs on `cd52f2670` and 8 of 40 runs
   on `5a080300e` returned a pooled score other than 76.667830; the bad runs
   differ in one or two individual frames (e.g. frame 1 = 0.0, or frame 3 =
   50.834 where CPU says 81.925763), not in a global offset. CPU (10/10) and
   SYCL (10/10) through the same FFmpeg build are bit-stable, and CUDA through
   the `vmaf` CLI without a thread pool is bit-stable (10/10). The two failure
   rates are within binomial noise of each other, so the merges neither caused
   nor worsened it.

Regenerating the snapshot is blocked on both closing.

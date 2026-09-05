<!-- markdownlint-disable MD013 -->

# Research digest — Netflix benchmark re-run on `cd52f2670` (2026-09-06)

Epic #1245, items 1 and 5. Question asked: *do the recorded per-backend Netflix
benchmark scores still reproduce on current master, and what are the fresh
throughput baselines?*

Answer: **no, they do not reproduce**, and the throughput baselines were not
recorded because the score gate failed. Two pre-existing GPU defects were found
along the way. Everything below is from a command run on the `ryzen-4090-arc`
bench host on 2026-09-06; nothing is copied from an earlier report.

## Method

`testdata/netflix_benchmark_results.json` is written by
`testdata/benchmark_netflix.py`, which drives an FFmpeg build carrying the
fork's `libvmaf`, `libvmaf_cuda` and `libvmaf_sycl` filters — not by
`testdata/bench_all.sh` (CLI) and not by `testdata/compare_combined.py` (which
diffs `scores_cpu_*.json` against `scores_sycl_a380_*.json` and never reads the
Netflix snapshot at all). The `/run-netflix-bench` skill points at the latter
pair, so following it literally cannot produce a snapshot diff; the digest
records that as a skill/harness mismatch.

libvmaf was built out-of-source inside `vmaf-dev-mcp` at four commits
(`cd52f2670` = master, `fcae339b5` = #1312, `2c7dc01f0` = #1307,
`5a080300e` = the commit before all three 2026-09-06 GPU merges), each with
`CC=icx CXX=icpx … -Denable_cuda=true -Denable_sycl=true --buildtype=release
-Db_lto=false -Dc_args=-march=native`, and injected ahead of the image's
installed library via `LD_LIBRARY_PATH`. The container's public headers differ
from current master only in comments and two new macros, so the FFmpeg filters
link against each build unchanged.

Host: AMD Ryzen 9 9950X3D (32 threads), 60 GiB, Linux 7.2.3-1-cachyos, RTX 4090
(driver 610.57.04), Arc A380; container Ubuntu 26.04, oneAPI DPC++ 2026.1.1,
CUDA 13.3.73, FFmpeg n9.0.1-17-gfde3691. Load average 7–19 throughout (image
build plus other agent sessions); the timing sweep specifically ran at load
9.6–9.8.

## Finding 1 — every backend has drifted, and none of it is today's merges

576x324 src01 pair, pooled: CPU 76.667828 → 76.667831 (+2.83e-06), CUDA
76.668903 → 76.667830 (-1.07e-03), SYCL 76.669148 → 76.667746 (-1.40e-03).
The 1080p checkerboard pairs move by ≤3.6e-05 (SYCL mild) and are exact on the
heavy pair. Full table in
`docs/development/netflix-benchmark-baselines.md`.

The 2026-09-06 merges are not the cause: the `5a080300e` rebuild scores CUDA
76.667830 and SYCL 76.667745 on the same pair, i.e. the same drift versus a
snapshot last written by PR #309 on 2026-05-02. What the merges *did* change is
smaller than the drift — CPU per-frame values moved by up to ~8e-6 (pooled
unchanged at six decimals) and the SYCL `frames[].metrics` key set shrank from
35 to 24 keys.

Interpretation: over four months the CUDA and SYCL twins converged **towards**
the CPU reference (recorded CUDA sat 1.07e-3 above CPU; it now tracks CPU to
2e-5). The drift is therefore most likely accumulated GPU-parity work, not a
regression — but it is a score change against committed data, so it is filed as
a correctness finding and the snapshot is left alone (ADR-1192).

## Finding 2 — `--threads N` aborts every GPU backend

`vmaf --gpumask=0 --no_sycl --threads 1` and `vmaf --sycl_device=0 --no_cuda
--threads 1` both exit 234 with no output, after one
`cannot be overwritten at index N` warning pair per frame and
`context could not be synchronized` / `problem flushing context`. `--threads 4`
behaves the same. Without `--threads` both succeed and are bit-stable across 10
runs (CUDA 76.667830, SYCL 76.667746). Present identically on `5a080300e`.

This is why `testdata/bench_all.sh` — which hard-codes `--threads 1` — has been
printing `SKIP (… backend likely unavailable)` for its CUDA and SYCL rows: it
discarded stderr and reported the exit code as an absent device. The harness is
fixed in this PR to keep stderr and print `FAIL` with the real last line.

The failure shape (per-index duplicate feature writes plus a context-sync error,
only when a thread pool is present) is exactly what the statically-derived
`T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03` row predicts for a
drain batch keyed by OS thread. This run is the empirical reproducer that row
said was the natural first step of the fix PR. The mechanism is **not** proven
here — only the symptom, the thread-pool dependency and the pre-existence.

## Finding 3 — the `libvmaf_cuda` FFmpeg filter is non-deterministic

40 runs per commit on the 576x324 pair: `cd52f2670` returned a non-modal pooled
score in 10, `5a080300e` in 8. Eleven distinct wrong values were observed
between 73.625670 and 76.126911. Bad runs corrupt one or two individual frames
(frame 1 = 0.0 against CPU 82.639803; frame 3 = 50.834 against CPU 81.925763)
while every other frame stays within 2e-5 of CPU. CPU and SYCL through the same
FFmpeg build are bit-stable over 10 runs each, and CUDA through the CLI with no
thread pool is bit-stable over 10 runs, which localises the defect to the
threaded/asynchronous feed rather than to the kernels.

20 % versus 25 % over n=40 each is inside binomial noise (95 % intervals of
roughly 9–36 % and 13–41 %), so the honest statement is that the merges neither
caused nor measurably worsened it. An earlier n=10 sample on `5a080300e` came
back 0/10 clean and briefly looked like a bisect hit; the larger sample retired
that conclusion. Recorded here because the mistake is easy to repeat.

## Consequence for the epic

Item 1 (re-run the suite) is done. Item 5 (record the fresh timing baselines) is
deliberately **not** done: the epic gates it on the scores matching, and a
throughput table recorded beside a CUDA score that is wrong a quarter of the
time would be worse than no table. The measured throughput is published as an
observation only, in
`docs/development/netflix-benchmark-baselines.md`.

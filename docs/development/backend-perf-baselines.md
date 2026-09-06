<!-- markdownlint-disable MD013 -->
# Per-backend performance baselines

This page is the operational guide to the fork's per-backend throughput
baselines: what is measured, with which harness, and how to reproduce a run
that is comparable to the numbers in [`docs/benchmarks.md`](../benchmarks.md).

The methodology decision is recorded in
[ADR-1185](../adr/1185-backend-perf-baseline-methodology.md).

## What this measures, and what it does not

It measures **`vmaf` CLI wall-clock throughput** — decode-from-raw-YUV,
feature extraction, model prediction and JSON emit, end to end, for one
exclusive backend at a time.

It does **not** measure:

- **Numerical parity.** GPU backends are *not* bit-exact to the CPU. Parity is
  a separate gate — see [`cross-backend-gate.md`](cross-backend-gate.md) and
  the `/cross-backend-diff` skill. The pooled score is recorded here only as a
  sanity signal, never as a correctness assertion.
- **FFmpeg end-to-end cost.** That is `testdata/bench_perf.py` (see
  [`docs/benchmarks.md` §FFmpeg lavfi performance harness](../benchmarks.md#ffmpeg-lavfi-performance-harness)).

## The harness

`testdata/bench_backends.py`. It is the repetition-and-median companion to
`testdata/bench_all.sh`; `bench_all.sh` answers "do the backends still agree?"
with one timed run per cell, this one answers "how fast is each backend?".

Its rules, all of which exist because a benchmark on a shared workstation is
otherwise uninterpretable:

| Rule | Why |
| --- | --- |
| One discarded warmup run per cell | page cache, GPU context creation and SYCL kernel JIT all land on run 1 |
| `--runs` timed repetitions, default 3, reported as **median** | the mean is dragged by a single scheduler stall; the median is not |
| min/max spread reported alongside | a cell with 30 % spread is noise, not a measurement |
| 1-minute load average sampled before and after every cell | a number without its load is not reproducible |
| exactly one backend per run, via the exclusive `--backend` selector | `--no_cuda` / `--no_sycl` are *disable*-only flags and do not engage anything |
| `frames[0].metrics` key count recorded per cell | a GPU row whose key count equals the CPU row's is a silent CPU fallback |

That last row is the load-bearing one. See
[`core/AGENTS.md` §"Backend-engagement foot-guns"](../../core/AGENTS.md).

## Fixtures

| Tag | Source | Geometry | Frames |
| --- | --- | --- | --- |
| `src01_576x324` | `python/test/resource/yuv/src01_hrc0{0,1}_576x324.yuv` | 576×324, 8-bit | 48 |
| `checkerboard_1px` | `checkerboard_1920_1080_10_3_0_0.yuv` vs `..._1_0.yuv` | 1920×1080, 8-bit | 3 |
| `checkerboard_10px` | `checkerboard_1920_1080_10_3_0_0.yuv` vs `..._10_0.yuv` | 1920×1080, 8-bit | 3 |
| `bbb_4k_200f` | `testdata/bbb/{ref,dis}_3840x2160_200f.yuv` | 3840×2160, 8-bit | 200 |

The first three are the Netflix golden-gate pairs (CLAUDE.md §8) and are
already present in a full checkout. **They are gitignored**
(`.gitignore`: `python/test/resource/yuv/`), so a fresh `git worktree` does
*not* contain them — link or copy the directory from your main checkout before
benchmarking there:

```bash
ln -sfn /path/to/main/checkout/python/test/resource/yuv python/test/resource/yuv
```

The 4K pair is gitignored too and must be generated once (see below).

The two 3-frame checkerboard pairs are deliberately kept even though 3 frames
is far too short to amortise a GPU dispatch. They are the fork's *worst case*
for GPU offload, and the point of a baseline table is to show where offload
loses as well as where it wins.

## Models

Every fixture is run against two models:

- **`v0.6.1`** — `model/vmaf_v0.6.1.json`, the model every historical row in
  `docs/benchmarks.md` used. Kept so new numbers stay comparable to old ones.
- **`default`** — no `--model` flag at all, so libvmaf resolves
  `VMAF_DEFAULT_MODEL_VERSION` (`vmaf_v1.0.16_3d0h`, ADR-1169). This is what a
  caller who passes no model actually pays, and it is the figure that retrain
  planning needs.

## How to reproduce

```bash
# 1. Build. One build dir per backend combination — see ADR-1185 for why the
#    all-backends binary is not the right thing to benchmark.
source /opt/intel/oneapi/setvars.sh          # for icx/icpx + Arc visibility
CC=icx CXX=icpx meson setup core/build core \
    -Denable_cuda=true -Denable_sycl=true \
    -Db_lto=false --buildtype=release
ninja -C core/build

# 2. Generate the 4K pair once (gitignored, ~2.5 GB per file).
#    Any 4K source works; the fps figures are insensitive to the codec
#    parameters, only the pooled score moves.
mkdir -p testdata/bbb
SRC=/path/to/bbb_sunflower_2160p_30fps_normal.mp4
ffmpeg -y -ss 60 -i "$SRC" -frames:v 200 -pix_fmt yuv420p \
    -f rawvideo testdata/bbb/ref_3840x2160_200f.yuv
ffmpeg -y -ss 60 -i "$SRC" -frames:v 200 -c:v libx264 -crf 35 -preset veryfast \
    -pix_fmt yuv420p testdata/bbb/dis_3840x2160_200f.mp4
ffmpeg -y -i testdata/bbb/dis_3840x2160_200f.mp4 -frames:v 200 \
    -pix_fmt yuv420p -f rawvideo testdata/bbb/dis_3840x2160_200f.yuv

# 3. Run. Keep the machine as idle as you can and record the load either way.
python3 testdata/bench_backends.py \
    --vmaf-bin core/build/tools/vmaf \
    --backend cpu --backend cuda \
    --runs 3 --json-out /tmp/baselines.json
```

Useful flags: `--list`-style auditing via `--dry-run` (prints the exact
`vmaf` command lines without touching hardware), `--fixture <tag>` and
`--model <name>` to narrow a run, `--runs N` to trade wall time for tighter
spread.

## Reading the output

```text
    cpu    ...   613.71 fps  median 0.078s  spread 3.5%  pool 76.667831  keys=15  load=6.6
```

- `fps` — `nframes / median_time`. The headline figure.
- `spread` — `(max - min) / median`. Treat anything above ~10 % as noise-dominated
  and re-run on a quieter machine before quoting it.
- `pool` — pooled VMAF mean, a sanity signal only (see above).
- `keys` — `frames[0].metrics` key count, the backend-engagement check.
- `load` — 1-minute load average at the end of the cell.

Two rows are only comparable when their `load` values are comparable. A
difference smaller than the larger of the two `spread` values is not a
difference.

## Interpreting a GPU row that loses to CPU

Expected, and not a bug, whenever the frame count is small or the resolution is
low: the fixed per-run cost (context creation, module load, host↔device copies)
is not amortised. The fork's own numbers show CPU winning at 576×324 and at
1080p×3f, and CUDA winning decisively at 4K×200f. If a GPU row loses at 4K with
a few hundred frames, that *is* worth investigating — start with
`/profile-hotpath`.

## Known blockers

Backends that cannot currently produce a baseline row are tracked in
[`docs/state.md`](../state.md). A `BLOCKED` cell in `docs/benchmarks.md` means
the run aborts, not that it is slow.

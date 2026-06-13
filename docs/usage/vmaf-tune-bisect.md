<!-- markdownlint-disable MD060 -->
# vmaf-tune — target-VMAF bisect (Phase B)

The `vmaftune.bisect` module finds the **largest CRF whose actual
measured VMAF still meets a target floor**, given a (source, codec,
target VMAF) triple. Largest CRF = lowest bitrate at acceptable
quality — the cost-optimal point.

This is the production wiring that replaced the earlier placeholder
predicate used by the `compare`, `recommend-saliency`, `predict`,
`tune-per-shot`, and `ladder` subcommands. See
[ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) for the decision
and [Research-0090](../research/0090-vmaf-tune-phase-b-bisect-feasibility.md)
for the algorithmic feasibility digest.

## When to reach for it

| Use case | What to use |
|---|---|
| One source, one codec, one target VMAF — find the CRF | `vmaftune.bisect.bisect_target_vmaf` |
| Many codecs, same source + target — rank by bitrate | `vmaf-tune compare --width ... --height ...` or `vmaftune.compare.compare_codecs(predicate=make_bisect_predicate(...))` |
| Per-shot CRF tuning across a movie | `vmaftune.per_shot.tune_per_shot(predicate=...)` (Phase D) |
| Per-resolution × per-target ladder | `vmaftune.ladder.build_ladder(...)` (Phase E) |
| Sweeping the entire `(preset, CRF)` plane | `vmaftune.corpus.coarse_to_fine_search` ([ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md)) |
| Quick recommendation from an existing corpus | `vmaftune.recommend.pick_target_vmaf` |

The bisect is a **1-axis primitive** that other phases compose with.
It does not sweep presets; pin a preset up-front (or use the adapter's
mid-range default). It does not pre-screen for unreachable targets;
the bisect bails with a clear error when the curve never clears the
floor in the searched window.

## Algorithm in one paragraph

Integer binary search over the CRF window
`crf_range or adapter.quality_range`. At each step, encode at the
midpoint CRF, score with libvmaf. Measured VMAF >= target → narrow
upward (try harder compression); else narrow downward. Midpoint
rounds toward the lower-quality (higher-CRF) end so the "best so far"
record is always a CRF we measured, not one we extrapolated to. Stops
when the window collapses to a single CRF or after `max_iterations`.

The bisect assumes monotone-decreasing VMAF in CRF. Two non-adjacent
samples that violate this contract (rising VMAF for rising CRF) abort
the call with a clear error rather than falling back to a different
search strategy.

## Quick start — single-codec

```python
from pathlib import Path
from vmaftune.bisect import bisect_target_vmaf

result = bisect_target_vmaf(
    Path("ref.yuv"),
    "libx264",
    target_vmaf=92.0,
    width=1920,
    height=1080,
    pix_fmt="yuv420p",
    framerate=24.0,
    duration_s=10.0,
    crf_range=(15, 40),       # default: adapter.quality_range
    max_iterations=8,
    preset="medium",          # default: adapter mid-range preset
)
if result.ok:
    print(
        f"best CRF {result.best_crf} → "
        f"VMAF {result.measured_vmaf:.2f} @ {result.bitrate_kbps:.0f} kbps "
        f"({result.n_iterations} encodes)"
    )
else:
    print(f"bisect failed: {result.error}")
```

The encode + score subprocesses go through the same seams Phase A
already uses (`encode.run_encode`, `score.run_score`), so all the
ffmpeg / vmaf binary settings (`ffmpeg_bin`, `vmaf_bin`,
`score_backend` per [ADR-0299](../adr/0299-vmaf-tune-gpu-score.md))
flow through verbatim.

## Quick start — multi-codec compare

CLI:

```shell
vmaf-tune compare \
    --src ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --sample-clip-seconds 4 \
    --target-vmaf 92 \
    --encoders libx264,libx265,libsvtav1 \
    --crf-min 15 --crf-max 40 \
    --format markdown
```

Python API:

```python
from pathlib import Path
from vmaftune.bisect import make_bisect_predicate
from vmaftune.compare import compare_codecs, emit_report

predicate = make_bisect_predicate(
    target_vmaf=92.0,
    width=1920,
    height=1080,
    framerate=24.0,
    duration_s=10.0,
    sample_clip_seconds=4.0,
    crf_range=(15, 40),
    max_iterations=8,
)

report = compare_codecs(
    src=Path("ref.yuv"),
    target_vmaf=92.0,
    encoders=("libx264", "libx265", "libsvtav1"),
    predicate=predicate,
)
print(emit_report(report, format="markdown"))
```

The predicate is bound once with the source geometry; `compare_codecs`
dispatches per-codec via the adapter registry and ranks the results
by ascending bitrate.

## Output schema — `BisectResult`

| Field | Type | Notes |
|---|---|---|
| `codec` | `str` | The codec name passed in |
| `best_crf` | `int` | Largest CRF whose VMAF >= target. `-1` on failure. |
| `measured_vmaf` | `float` | The actual libvmaf score at `best_crf`. NaN on failure. |
| `bitrate_kbps` | `float` | File-size-derived against `duration_s`. `0.0` if `duration_s <= 0`. |
| `encode_time_ms` | `float` | Last (best) encode wall time. |
| `n_iterations` | `int` | Number of encode+score round-trips actually run. |
| `encoder_version` | `str` | Parsed from ffmpeg stderr (e.g. `"libx264-164"`). |
| `ok` | `bool` | `False` on unreachable target / monotonicity violation / encode failure. |
| `error` | `str` | Human-readable error string. Empty on success. |

`BisectResult.to_recommend_result()` projects onto
`compare.RecommendResult` for downstream consumers that already speak
the comparison schema.

## Knobs

| Argument | Default | Notes |
|---|---|---|
| `crf_range` | `adapter.quality_range` | Inclusive `(lo, hi)`; widening past the adapter's range is allowed. |
| `max_iterations` | `8` | Hard cap; binary search asymptote is `ceil(log2(range))`. |
| `sample_clip_seconds` | `0.0` | `0.0` scores the full source. Positive values shorter than `duration_s` encode the centre window, score the matching `frame_skip_ref` / `frame_cnt` window, and normalise bitrate against the sample duration (ADR-0301). |
| `preset` | adapter mid-range (`"medium"` for x264/x265/svtav1) | Forwarded verbatim to the adapter. |
| `vmaf_model` | `"vmaf_v0.6.1"` | Same vocabulary as `score.py`; HDR / 4K models per ADR-0289 / ADR-0295. |
| `score_backend` | `None` | `"cpu"` / `"cuda"` / `"sycl"` / `"vulkan"` per ADR-0299. |
| `encode_runner` / `score_runner` / `decode_runner` | `subprocess.run` | Test seams; production callers leave `None`. `decode_runner` falls back to `encode_runner` when both are ffmpeg invocations. |
| `workdir` | `tempfile.TemporaryDirectory()` | Per-iteration encoded output + the cached decoded-reference sidecar go here; cleaned at exit. |

## Container vs raw YUV sources

`src` may be **either** a raw planar `.yuv` (the legacy contract) **or
any FFmpeg-readable container** (`.mp4`, `.mkv`, `.mov`, …). The
bisect autodetects the shape via the file suffix and does two extra
things for container sources (ADR-0497):

1. **Encode** — the encoder ffmpeg invocation omits the
   `-f rawvideo -pix_fmt … -s … -r …` input flags so ffmpeg
   autodetects the container; otherwise it would mis-interpret the
   demuxed bytes as raw YUV and produce zero frames.
2. **Decode-then-score** — every per-CRF encode is a `.mkv`
   container; the libvmaf CLI only accepts raw `.yuv` / `.y4m`, so
   the bisect decodes the encoded artefact to a raw YUV sidecar
   before invoking the vmaf binary. The reference is also decoded
   once per bisect and cached across iterations.

These steps add one ffmpeg call per encode + one per bisect to the
critical path; both are negligible against the encoder runtime on
sample clips longer than a second.

## Error modes

| Error | Cause | Recovery |
|---|---|---|
| `"unknown codec: ..."` | `codec` not registered in `codec_adapters` | Register the adapter or pick a known codec |
| `"invalid crf_range: lo > hi"` | Inverted window | Pass a valid `(lo, hi)` |
| `"adapter rejected (preset=..., crf=...)"` | Out-of-range crf or unknown preset | Use a valid preset + clip CRF to `quality_range` |
| `"encode failed at CRF N"` | ffmpeg exit non-zero | Inspect stderr; fix the source, codec args, or workdir |
| `"score failed at CRF N"` | vmaf exit non-zero or out-of-range score | Inspect vmaf binary + model; check pix_fmt match |
| `"target VMAF X unreachable in CRF window"` | Curve never clears target | Lower target, or widen `crf_range` toward `lo=0` |
| `"monotonicity violation: VMAF rose from V1 at CRF C1 to V2 at CRF C2"` | Pathological codec / corrupt content | Inspect the encode at the offending CRFs; do **not** fall back to a non-bisect strategy |

## What it does NOT do (yet)

- **No cache**: every call re-encodes; integrating the
  [ADR-0298](../adr/0298-vmaf-tune-cache.md) cache key fields is a
  one-call insertion.
- **No standalone `bisect` CLI subcommand**: the primitive is exposed
  through `vmaf-tune compare` for multi-codec ranking and through the
  Python API for custom orchestration. `tune-per-shot` and `ladder`
  can bind the same predicate from Python.

## See also

- [ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) — decision +
  alternatives matrix.
- [Research-0090](../research/0090-vmaf-tune-phase-b-bisect-feasibility.md)
  — algorithmic feasibility digest.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) —
  vmaf-tune umbrella spec.
- [`tools/vmaf-tune/AGENTS.md`](../../tools/vmaf-tune/AGENTS.md) —
  rebase-sensitive invariants for the harness.

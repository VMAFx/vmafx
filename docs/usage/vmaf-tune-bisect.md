<!-- markdownlint-disable MD013 MD060 -->
# vmaf-tune — target-VMAF bisect (Phase B)

The `vmaftune.bisect` module finds the largest CRF whose measured VMAF still
meets a target floor, given a (source, codec, target VMAF) triple. The largest
CRF is the lowest bitrate at acceptable quality, so it is the cost-optimal
point.

It is a library primitive. There is no standalone `bisect` subcommand: the
CLI reaches it through [`vmaf-tune compare`](vmaf-tune-compare.md), and Python
callers use `bisect_target_vmaf` directly.

Phase B replaced the earlier placeholder predicate used by `compare`,
`recommend-saliency`, `predict`, `tune-per-shot` and `ladder`. See
[ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) for the decision and
[Research-0090](../research/0090-vmaf-tune-phase-b-bisect-feasibility.md) for
the algorithmic feasibility digest.

## When to use it

| Use case | What to use |
| --- | --- |
| One source, one codec, one target VMAF: find the CRF | `vmaftune.bisect.bisect_target_vmaf` |
| Many codecs, same source and target: rank by bitrate | `vmaf-tune compare --width ... --height ...` or `vmaftune.compare.compare_codecs(predicate=make_bisect_predicate(...))` |
| Per-shot CRF tuning across a movie | `vmaftune.per_shot.tune_per_shot(predicate=...)` (Phase D) |
| Per-resolution by per-target ladder | `vmaftune.ladder.build_ladder(...)` (Phase E, see [`vmaf-tune-ladder.md`](vmaf-tune-ladder.md)) |
| Sweep of the whole `(preset, CRF)` plane | `vmaftune.corpus.coarse_to_fine_search` ([coarse-to-fine](vmaf-tune-coarse-to-fine.md), [ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md)) |
| Quick recommendation from an existing corpus | `vmaftune.recommend.pick_target_vmaf` ([`recommend`](vmaf-tune-recommend.md)) |

The bisect is a one-axis primitive. It does not sweep presets: pin a preset
up front or use the adapter's mid-range default. It does not pre-screen for
unreachable targets; it stops with a clear error when the curve never clears
the floor inside the searched window.

## Quick start: single codec

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
    crf_range=(15, 40),       # default: the encoder's absolute CRF range
    max_iterations=8,
    preset="medium",          # default: adapter mid-range preset
)
if result.ok:
    print(
        f"best CRF {result.best_crf} -> "
        f"VMAF {result.measured_vmaf:.2f} @ {result.bitrate_kbps:.0f} kbps "
        f"({result.n_iterations} encodes)"
    )
else:
    print(f"bisect failed: {result.error}")
```

Encode and score subprocesses go through the same seams Phase A uses
(`encode.run_encode`, `score.run_score`), so the `ffmpeg_bin`, `vmaf_bin` and
`score_backend` settings apply unchanged.

## Quick start: multi-codec compare

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

The predicate is bound once with the source geometry. `compare_codecs`
dispatches per codec through the adapter registry and ranks the results by
ascending bitrate.

## Algorithm

The bisect is an integer binary search over the CRF window:

1. Encode at the midpoint CRF and score it with libvmaf.
2. If the measured VMAF is at least the target, record the CRF as the best so
   far and narrow upward (try harder compression). Otherwise narrow downward.
3. Round the midpoint toward the lower-quality (higher-CRF) end, so the best
   record is always a CRF that was measured, never an extrapolation.
4. Stop when the window collapses or after `max_iterations`.

The search assumes VMAF does not rise as CRF rises. Two samples that violate
this by more than 0.5 VMAF abort the call with a `monotonicity violation`
error instead of falling back to a different strategy.

## High-VMAF contract (ADR-0538)

With `crf_range` left at its default (and no `--crf-min` / `--crf-max` in the
CLI), `bisect_target_vmaf` searches the encoder's absolute CRF range, not the
adapter's perceptually informative `quality_range`:

| Codec | Absolute CRF range | Informative `quality_range` (legacy default) |
| --- | --- | --- |
| `libx264` | `0..51` | `0..51` (already maximal) |
| `libx265` | `0..51` | `15..40` |
| `libvpx-vp9` | `0..63` | `0..63` (already maximal) |
| `libaom-av1` | `0..63` | `0..63` (already maximal) |
| `libsvtav1` | `0..63` | `20..50` |
| Other (`*_nvenc`, `*_qsv`, `*_amf`, `*_videotoolbox`, `libvvenc`) | the adapter's `crf_min` / `crf_max`, else its `quality_range` | as declared by the adapter |

The contract guarantees:

1. **The search starts at the encoder's accepted floor.** For `libx264`,
   `libx265`, `libvpx-vp9`, `libaom-av1` and `libsvtav1` the lowest probe is
   CRF 0 (lossless), so any reasonable source reaches VMAF 98 or more there
   and high targets are reachable.
2. **`max_iterations` covers the widest window.** A window of 52 CRFs
   (`0..51`) needs at most 6 encodes and a window of 64 (`0..63`) at most 7;
   the default of 8 covers both. Programmatic callers aiming at VMAF 97 or
   above should keep at least 7.
3. **Overshoot at the floor is fine.** If the codec already exceeds the target
   at CRF 0 (say VMAF 99.5 against a target of 96 on a low-distortion
   source), the search narrows toward higher CRFs, returns the highest CRF
   that still clears the target, and sets `ok=True`. The achieved VMAF is
   at least the target by construction.
4. **The narrow window is bypassed for CRF validation, not for presets.**
   Preset names are still checked against the adapter's whitelist. Pass
   `--crf-min` / `--crf-max` (or `crf_range`) to restore the narrow-window
   behaviour.

## Knobs

| Argument | Default | Meaning |
| --- | --- | --- |
| `crf_range` | encoder absolute range (table above) | Inclusive `(lo, hi)`. Widening past the adapter's range is allowed. |
| `max_iterations` | `8` | Hard cap on encode-and-score round trips. |
| `sample_clip_seconds` | `0.0` | `0.0` scores the full source. A positive value shorter than `duration_s` encodes the centre window and scores the matching frame window; bitrate is normalised against the sample duration (ADR-0301). |
| `preset` | adapter mid-range (`medium` when offered) | Forwarded to the adapter. |
| `vmaf_model` | `vmaf_v1.0.16_3d0h` | Same vocabulary as `score.py`; HDR and 4K models per ADR-0289 and ADR-0295. |
| `score_backend` | `None` | `None` emits no `--backend` flag, so libvmaf picks its default. A name (`auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`) is forwarded as `--backend NAME` ([ADR-0299](../adr/0299-vmaf-tune-gpu-score.md)). Vulkan was removed in ADR-0726. |
| `ffmpeg_bin`, `vmaf_bin` | `ffmpeg`, `vmaf` | Binaries to run. |
| `workdir` | temporary directory | Per-iteration encodes and the decoded-reference sidecar go here. `VMAFTUNE_WORKDIR` selects the parent when the argument is `None`. |
| `decode_semaphore` | module default (`Semaphore(1)`) | Caps concurrent reference decodes across threads ([ADR-0577](../adr/0577-vmaftune-bisect-concurrency-cap-and-aggressive-cleanup.md)). |
| `nr_proxy_backend` | `None` | NR pre-scoring hook for [`--fast-nr`](vmaf-tune-fast-nr.md). |

The `encode_runner`, `score_runner` and `decode_runner` arguments are test
seams that default to `subprocess.run`. Production callers leave them `None`.

## Output: `BisectResult`

| Field | Type | Notes |
| --- | --- | --- |
| `codec` | `str` | The codec name passed in. |
| `best_crf` | `int` | Largest CRF whose VMAF meets the target; `-1` on failure. |
| `measured_vmaf` | `float` | The libvmaf score at `best_crf`; NaN on failure. |
| `bitrate_kbps` | `float` | File-size-derived against `duration_s`; `0.0` if `duration_s <= 0`. |
| `encode_time_ms` | `float` | Wall time of the last (best) encode. |
| `n_iterations` | `int` | Encode-and-score round trips actually run. |
| `encoder_version` | `str` | Parsed from ffmpeg stderr, for example `libx264-164`. |
| `ok` | `bool` | `False` on unreachable target, monotonicity violation or encode failure. |
| `error` | `str` | Human-readable error; empty on success. |
| `samples` | tuple of `BisectSample` | Every successful probe (`crf`, `bitrate_kbps`, `vmaf_score`, `encode_time_ms`), so a rate-quality chart plots the measured curve (ADR-0534). |
| `fr_calls_total`, `fr_calls_saved` | `int` | Full-reference scoring calls made and skipped by NR pre-scoring; both `0` without `--fast-nr`. |

`BisectResult.to_recommend_result()` projects onto `compare.RecommendResult`
for consumers that already speak the comparison schema.

## Container and raw YUV sources

`src` may be a raw planar `.yuv` or any FFmpeg-readable container (`.mp4`,
`.mkv`, `.mov`, and so on). The bisect picks the shape from the file suffix
and, for container sources (ADR-0497):

1. **Encode:** omits the `-f rawvideo -pix_fmt ... -s ... -r ...` input flags
   so ffmpeg detects the container. Otherwise it would read demuxed bytes as
   raw YUV and produce zero frames.
2. **Decode, then score:** every per-CRF encode is a `.mkv`, and the libvmaf
   CLI only accepts raw `.yuv` or `.y4m`, so the bisect decodes the encoded
   file to a raw YUV sidecar before calling the vmaf binary. The reference is
   decoded once per bisect and reused across iterations.

These steps add one ffmpeg call per encode and one per bisect. Both are
negligible against the encoder runtime on sample clips longer than a second.

## Error modes

| Error | Cause | Recovery |
| --- | --- | --- |
| `unknown codec: ...` | `codec` is not registered in `codec_adapters` | Register the adapter or pick a known codec. |
| `invalid crf_range: lo=N > hi=M` | Inverted window | Pass a valid `(lo, hi)`. |
| `max_iterations must be >= 1` | Zero or negative cap | Pass a positive value. |
| `adapter rejected (preset=..., crf=...)` | CRF outside the adapter's accepted range, or an unknown preset | Use a valid preset and clip the CRF. |
| `encode failed at CRF N` | ffmpeg exited non-zero | Inspect stderr; fix the source, codec arguments or workdir. |
| `score failed at CRF N` | vmaf exited non-zero or returned an out-of-range score | Inspect the vmaf binary and model; check that `pix_fmt` matches. |
| `target VMAF X unreachable in CRF window [lo, hi] after N iterations ...` | The curve never clears the target | Lower the target, or widen `crf_range` toward `lo=0`. |
| `monotonicity violation: VMAF rose from V1 at CRF C1 to V2 at CRF C2` | Pathological codec or corrupt content | Inspect the encodes at those CRFs. Do not fall back to a non-bisect strategy. |
| Disk-space error naming the codec, target and iteration | Estimated decoded-YUV size exceeds free space in the workdir | Point `workdir` or `VMAFTUNE_WORKDIR` at a larger volume. |

## Not yet supported

!!! note "Not yet"
    - **No cache.** Every call re-encodes. Using the
      [ADR-0298](../adr/0298-vmaf-tune-cache.md) cache-key fields is a
      one-call insertion.
    - **No standalone `bisect` subcommand.** The primitive is exposed through
      `vmaf-tune compare` for multi-codec ranking and through the Python API
      for custom orchestration. `tune-per-shot` and `ladder` can bind the same
      predicate from Python.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — overview of every subcommand.
- [`vmaf-tune-compare.md`](vmaf-tune-compare.md) — multi-codec ranking built on
  this bisect, including the `--crf-min` / `--crf-max` flags.
- [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md) — NR early elimination inside
  the bisect.
- [ADR-0326](../adr/0326-vmaf-tune-phase-b-bisect.md) — decision and
  alternatives matrix.
- [ADR-0538](../adr/0538-premium-vmaf-target-defaults-and-bisect.md) — the
  high-VMAF contract.
- [Research-0090](../research/0090-vmaf-tune-phase-b-bisect-feasibility.md)
  — algorithmic feasibility digest.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — `vmaf-tune`
  umbrella spec.
- [`tools/vmaf-tune/AGENTS.md`](../../tools/vmaf-tune/AGENTS.md) —
  rebase-sensitive invariants for the harness.

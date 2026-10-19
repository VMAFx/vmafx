<!-- markdownlint-disable MD013 MD060 -->
# `vmaf-tune recommend` — pick a CRF for a quality or bitrate target

`vmaf-tune recommend` returns one (preset, CRF) pair for a target. It works in
two modes:

- **Corpus mode** (`--from-corpus`): select a row from an existing corpus JSONL.
  No encode and no scoring run.
- **Live mode** (`--source`): run the [coarse-to-fine
  search](vmaf-tune-coarse-to-fine.md),
  write every visited point to `--output`, and report the winner.

The base predicate is the point-estimate recipe from
[Research-0061](../research/0061-vmaf-tune-capability-audit.md) (Bucket 5). The
optional [uncertainty-aware extension](#uncertainty-aware-extension)
([ADR-0393](../adr/0393-fr-regressor-v2-probabilistic.md)) changes only how
much of the corpus is scanned.

## Quick start

### Corpus mode

1. Build a corpus once with the [`corpus` subcommand](vmaf-tune-corpus.md):

    ```shell
    vmaf-tune corpus --source ref.yuv --width 1920 --height 1080 \
        --framerate 24 --duration 10 \
        --preset medium --crf 18 --crf 22 --crf 26 --crf 30 --crf 34 \
        --output corpus.jsonl
    ```

2. Ask for the lowest-bitrate encode whose VMAF reaches 92:

    ```shell
    vmaf-tune recommend --from-corpus corpus.jsonl --target-vmaf 92.0
    ```

3. Or ask for the row whose bitrate is closest to 5 Mbps:

    ```shell
    vmaf-tune recommend --from-corpus corpus.jsonl --target-bitrate 5000
    ```

### Live mode

```shell
vmaf-tune recommend \
    --source ref.yuv --width 1920 --height 1080 \
    --framerate 24 --duration 10 \
    --preset medium \
    --target-vmaf 92
# stdout: src=ref.yuv preset=medium crf=27 vmaf=92.341 (visited 15 encodes)
```

Live mode needs `--source`, `--width`, `--height`, `--preset` and
`--target-vmaf`; it has no `--crf` flag because the search generates the CRF
axis. `--target-bitrate` is corpus mode only.

## Predicates

| Flag | Row chosen |
| --- | --- |
| `--target-vmaf T` | The row with the lowest `bitrate_kbps` among those whose `vmaf_score >= T` (ties go to the higher VMAF, then the lower CRF). When no row clears `T`, the highest-VMAF row, annotated `(UNMET)`. |
| `--target-bitrate KBPS` | The row whose `bitrate_kbps` is closest (absolute distance) to `KBPS`. Ties go to the smaller CRF, which is the higher quality. |

Passing both targets is an error (exit code 2). In corpus mode, rows with a
non-zero `exit_status` and rows with a missing or non-finite `vmaf_score` are
ignored.

!!! warning "Corpus mode filters on `--encoder` and `--preset`"
    `--encoder` defaults to `libx264`, so a corpus of another encoder yields
    "no eligible rows" until you pass `--encoder NAME`. `--preset` filters only
    when given; with several `--preset` flags only the first is used. This lets
    one mixed-codec corpus serve several runs without splitting it into
    per-codec files.

## Flags

### Selection

| Flag | Default | Meaning |
| --- | --- | --- |
| `--from-corpus JSONL` | none | Select from this corpus instead of running encodes. `--source`, `--width`, `--height` and `--preset` become optional. |
| `--target-vmaf T` | none | Lowest-bitrate predicate. Required in live mode. |
| `--target-bitrate KBPS` | none | Closest-bitrate predicate (corpus mode only). |
| `--json` | off | Print the winning corpus row as one JSON object instead of the summary line. |

### Live search

| Flag | Default | Meaning |
| --- | --- | --- |
| `--source PATH` | none | Source clip; repeat the flag for several sources. |
| `--width`, `--height` | none | Source geometry, required in live mode. |
| `--pix-fmt` | `yuv420p` | Source pixel format. |
| `--framerate` | `24.0` | Source frame rate. |
| `--duration S` | `0.0` | Analysed window in seconds (`0` = whole source). |
| `--encoder NAME` | `libx264` | Codec adapter. Also the row filter in corpus mode. |
| `--preset NAME` | none | Preset to search; repeat for several. Required in live mode. |
| `--coarse-to-fine`, `--coarse-step`, `--fine-radius`, `--fine-step` | search always on; `10`, `5`, `1` | See [`vmaf-tune-coarse-to-fine.md`](vmaf-tune-coarse-to-fine.md). |
| `--output PATH` | `corpus.jsonl` | JSONL destination for the visited points. |
| `--encode-dir PATH` | `.workingdir/cache/vmafx-tune/encodes` | Scratch directory for encodes. |
| `--keep-encodes` | off | Keep the encoded files after scoring. |
| `--vmaf-model NAME` | per encode height | libvmaf model of the live search; without it the model follows the encode height ([resolution-aware](vmaf-tune-resolution-aware.md)). |
| `--neg` | off | Use the VMAF NEG model variant (see [VMAF NEG](../metrics/vmaf-neg.md)). |
| `--ffmpeg-bin`, `--vmaf-bin` | `ffmpeg`, `vmaf` | Binaries to run. |
| `--score-backend` | `auto` | `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`. `auto` prefers `cuda`, then `sycl`, `hip`, `metal`, `cpu`; the chosen backend is printed on stderr. |
| `--no-source-hash` | off | Skip hashing the source for the corpus row. |
| `--two-pass` | off | Two-pass encode for codecs that support it (see [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md)). |

### Uncertainty

| Flag | Default | Meaning |
| --- | --- | --- |
| `--with-uncertainty` | off | Use the interval-aware predicate. Needs `--target-vmaf`; with `--target-bitrate` it prints a notice and falls back to the point estimate. |
| `--uncertainty-sidecar PATH` | none | Calibration sidecar JSON. Falls back, with a WARNING, to the [Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md) floor (tight 2.0, wide 5.0 VMAF) when absent or unreadable. |

## Output

Corpus mode prints one summary line (a trailing `[UNMET]` replaces `[OK]` when
no row reaches the target):

```text
crf=22  vmaf=95.000  kbps=5000  predicate=target_vmaf>=92.0  [OK]
```

Live mode prints the winner of each source and preset, using the smallest
passing CRF:

```text
src=ref.yuv preset=medium crf=27 vmaf=92.341 (visited 15 encodes)
```

| Case | Exit code |
| --- | --- |
| Row selected (including an honest closest miss in corpus mode, tagged `[UNMET]`) | `0` |
| Live mode where no CRF reaches the target (stderr names the target and the `--output` file) | `1` |
| Usage error: missing corpus file, both targets, no target, missing live-mode flag, no eligible rows | `2` |

## Uncertainty-aware extension

The point-estimate recipe treats every row's VMAF as exact. In practice the
predictor's residuals carry a distribution, and the
[conformal-prediction surface](../ai/conformal-vqa.md) wraps each prediction in
a `(low, high)` interval whose width is the predictor's local confidence.
`--with-uncertainty` uses that width to decide how much to scan.

Each row is classed by the width of its interval:

| Band | Condition | Action |
| --- | --- | --- |
| TIGHT | `width <= tight_max` and `low >= target` | Stop at the first qualifying row in file order: an `O(k)` scan. |
| WIDE | At least one row has `width >= wide_min` | Scan every row and tag the result `(UNCERTAIN)`. |
| MIDDLE or NaN | Otherwise | Defer to the native point-estimate predicate. |

When every visited row's interval lies below the target (`high < target`), the
result is tagged `UNMET, interval-excluded` and returns the highest-VMAF row.

Rows without a `vmaf_interval` object (`low` and `high`) have a NaN width and
land in MIDDLE, so a corpus built by plain `corpus` runs behaves exactly like
the point-estimate recipe. This keeps pre-uncertainty behaviour for callers who
upgrade the binary but keep an uncalibrated corpus.

### Calibration sidecar

The defaults are `tight_interval_max_width = 2.0` and
`wide_interval_min_width = 5.0` VMAF, from
[Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md),
defined by `vmaftune.uncertainty.ConfidenceThresholds`. The sidecar schema
matches the `auto` driver's loader, so one file drives `auto`, `recommend` and
[`ladder`](vmaf-tune-ladder.md):

```json
{
  "tight_interval_max_width": 1.6,
  "wide_interval_min_width": 4.2
}
```

Extra keys are ignored.

### Worked example

The example assumes the visited rows carry a `vmaf_interval` object. No
`vmaf-tune` subcommand writes that object into corpus rows (their VMAF is
measured, not predicted), so rows from a plain `corpus` or live run land in
MIDDLE and the pick is the point-estimate one. The command says so: stderr
reads `vmaf-tune recommend: --with-uncertainty: no row carries a vmaf_interval
...` and the result line ends with `uncertainty=unavailable`. Supply intervals
from a calibrated predictor pipeline, or use the library override shown below.

```text
$ vmaf-tune recommend --source ref.yuv --width 1920 --height 1080 \
    --encoder libx264 --preset medium --target-vmaf 93.0 \
    --coarse-to-fine --with-uncertainty \
    --uncertainty-sidecar calibration.json \
    --output corpus.jsonl

vmaf-tune: scoring backend = cpu
src=ref.yuv preset=medium crf=20 vmaf=94.250 \
    decision=tight rows_examined=2/15 (all 15 encoded) \
    predicate=target_vmaf>=93.0 (TIGHT, low=93.420)
```

- `decision=tight`: the interval at CRF 20 has `width=0.6 <= tight_max=2.0`
  and `low=93.42 >= target=93.0`, so the search stopped there.
- `rows_examined=2/15 (all 15 encoded)`: the pick examined two of the 15
  rows the coarse-to-fine sweep produced. Every encode has already run when
  the pick starts, so the short-circuit saves no encode. With `--from-corpus`
  the line carries `rows_examined=N/M` alone (no encodes run there).
- `predicate=...(TIGHT, low=93.420)`: the predicate that fired, with the lower
  bound that promoted the row.

If the predictor's intervals were all `width >= 5.0`, the output would read:

```text
src=ref.yuv preset=medium crf=20 vmaf=94.250 \
    decision=wide rows_examined=15/15 (all 15 encoded) \
    predicate=target_vmaf>=93.0 (UNCERTAIN)
```

### Library entry point

```python
from vmaftune.recommend import (
    UncertaintyAwareRequest,
    pick_target_vmaf_with_uncertainty,
)
from vmaftune.uncertainty import (
    ConfidenceDecision,
    ConfidenceThresholds,
    load_confidence_thresholds,
)

thresholds = load_confidence_thresholds("calibration.json")
req = UncertaintyAwareRequest(
    target_vmaf=93.0,
    thresholds=thresholds,
    encoder="libx264",
    preset="medium",
)
result = pick_target_vmaf_with_uncertainty(rows, req)
assert result.decision is not ConfidenceDecision.WIDE  # confident pick
```

Per-call interval overrides go through `sample_uncertainty`, which is useful
when the deep-ensemble and conformal pipeline produces intervals out of band:

```python
overrides = {
    20: (94.0, 93.5, 94.5),  # (point, low, high) at CRF 20
    23: (91.0, 90.0, 92.0),
}
req = UncertaintyAwareRequest(
    target_vmaf=93.0,
    sample_uncertainty=overrides,
)
```

!!! note "What the extension does not change"
    The recipe changes the search cost and which qualifying row is picked from
    an equivalence class. It does not change the Netflix golden-data
    assertions, the production-flip gate in `predictor_validate.py` (which
    decides what ships, not what is probed), or the point estimate itself:
    [`Predictor.predict_vmaf`](../api/predictor.md) returns the same scalar with
    or without the uncertainty wiring.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — overview of every subcommand.
- [`vmaf-tune-coarse-to-fine.md`](vmaf-tune-coarse-to-fine.md) — the live
  search, its flags, and the timing comparison against a full grid.
- [`vmaf-tune-corpus.md`](vmaf-tune-corpus.md) — builds the corpus and
  documents its row schema.
- [`vmaf-tune-benchmark.md`](vmaf-tune-benchmark.md) — ranks encoders from the
  same corpus.
- [`vmaf-tune-ladder.md`](vmaf-tune-ladder.md) — the ABR-ladder consumer of the
  same intervals.
- [`docs/ai/conformal-vqa.md`](../ai/conformal-vqa.md) — the conformal
  prediction surface (ADR-0393).
- [Research-0067](../research/0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
  — provenance of the threshold defaults.

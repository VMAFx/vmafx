<!-- markdownlint-disable MD013 MD060 -->
# vmaf-tune — Fast NR Pre-Scoring (`--fast-nr`)

`--fast-nr` enables **NR early-elimination** in the
[target-VMAF CRF bisect](vmaf-tune-bisect.md) (ADR-0615 / ADR-0624).  Instead of running a full-reference VMAF call at
every bisect midpoint, the cheap `nr_metric_v1` ONNX model scores the
distorted stream alone (~200 ms CPU, <50 ms GPU EP), maps that raw MOS-like
score into VMAF units with the sidecar calibration, and skips the expensive
FR call whenever the calibrated proxy is far from the target.  The final
accepted CRF always receives a full-reference confirmation call.

Typical wall-time reduction: **2–4×** on in-domain content that sits well
away from the target VMAF.

## Supported subcommands

| Subcommand | Flag supported |
|------------|---------------|
| `compare` | `--fast-nr` |
| `tune-per-shot` | `--fast-nr` |
| `corpus` | not applicable (no bisect loop) |
| `ladder` | not applicable (corpus-sampler mode) |

## Requirements

```text
pip install onnxruntime         # CPU only
pip install onnxruntime-gpu     # CUDA / ROCm EP (recommended on GPU hosts)
pip install numpy               # required by the NR frame extractor
```

The `nr_metric_v1.onnx` model is already in-tree at `model/tiny/`.  No
additional download is required.

## Quick start

```bash
# Compare the default CPU codecs (libx265, libsvtav1) with NR pre-scoring:
vmaf-tune compare \
    --src source.yuv --width 1920 --height 1080 \
    --framerate 24 --duration 10 \
    --target-vmaf 93 \
    --fast-nr

# Per-shot encode with NR pre-scoring:
vmaf-tune tune-per-shot \
    --src source.yuv --width 1920 --height 1080 \
    --framerate 24 \
    --target-vmaf 90 \
    --encoder libx265 \
    --fast-nr
```

The terminal will print the resolved δ_fast threshold at startup:

```text
vmaf-tune compare: --fast-nr enabled; δ_fast=8.0 VMAF (NR early-elimination)
```

And INFO-level logs report savings per bisect run:

```text
fast-nr: bisect done — FR calls 3 total, 4 saved (57%)
```

## The δ_fast threshold

`δ_fast` is the VMAF-unit half-width of the "uncertainty zone".  The raw
`nr_metric_v1` output is first mapped as
$\mathrm{NR\_VMAF} = \mathrm{calibration\_slope} \cdot \mathrm{NR}_{\mathrm{raw}} + \mathrm{calibration\_intercept}$.  When
$\lvert \mathrm{NR\_VMAF} - \mathrm{target} \rvert > \delta_{\mathrm{fast}}$ the FR call is skipped.  When within the zone
the FR call is paid.

The default value (8.0 VMAF) comes from the ADR-0615 design target and is
conservative enough to be safe on hosts that have not been calibrated. The
calibration script fits a tighter value from your own corpus.

### Calibrate on your corpus

1. Run the calibration script over a directory of reference YUVs:

   ```bash
   python ai/scripts/calibrate_nr_threshold.py \
       --corpus .corpus/netflix/ \
       --output model/tiny/nr_metric_v1.json \
       --nr-ep cpu
   ```

2. Read the Markdown report at
   `docs/ai/models/nr_metric_v1-calibration-<date>.md`. It states the fit
   quality, including a `WEAK` status when the fit is poor.
3. Run `vmaf-tune ... --fast-nr` as before. `NRProxyBackend` picks the new
   values up automatically on the next invocation.

A successful run writes these fields to `nr_metric_v1.json`:

- `calibration_slope` and `calibration_intercept`, the NR-to-VMAF mapping;
- `calibration_threshold`, the fitted `δ_fast`;
- `run_provenance` ([ADR-0661](../adr/0661-ai-run-manifest-provenance.md)),
  which traces the threshold back to the corpus directory, the
  `nr_metric_v1.onnx` input, the CRF grid, the CLI arguments and the report
  path that produced it.

!!! warning "Weak fits are not written"
    By default the script needs at least 10 samples and a PLCC of at least
    0.70 between the raw NR scores and the FR VMAF. A weaker fit still writes
    the Markdown report with a `WEAK` quality status, but it does not update
    `nr_metric_v1.json`, so `--fast-nr` never consumes a sidecar that cannot
    speed up the tune loop safely. Pass `--allow-weak-calibration` for
    diagnostic work only.

### Content-type considerations

Tune `δ_fast` per content type. `vmaf-tune` has no `--delta-fast` flag: edit
`calibration_threshold` in `model/tiny/nr_metric_v1.json`, or run
`calibrate_nr_threshold.py --delta-fast V` to write a forced value.

The default of 8.0 VMAF is a global threshold calibrated on mixed-content
Netflix clips.

- **Homogeneous content** (animation, screen capture): NR correlates better
  with FR, so a tighter value such as 5.0 can save more FR calls without a
  correctness risk.
- **High-motion sports content**: NR is less predictive, so a looser value
  such as 10 to 12 is safer.

## Calibration script

`ai/scripts/calibrate_nr_threshold.py` walks a YUV corpus at a CRF grid,
runs FR + NR scoring for each clip, fits
`vmaf_fr ≈ calibration_slope × nr_raw + calibration_intercept` via
least-squares, and sets `δ_fast = 2σ(residuals)`.

```text
usage: calibrate_nr_threshold.py [-h] [--corpus CORPUS] [--output JSON]
                                 [--onnx ONNX] [--crfs CRFS] [--codec CODEC]
                                 [--preset PRESET] [--width WIDTH]
                                 [--height HEIGHT] [--pix-fmt PIX_FMT]
                                 [--max-clips N] [--vmaf-bin VMAF_BIN]
                                 [--ffmpeg-bin FFMPEG_BIN] [--report-dir DIR]
                                 [--delta-fast VMAF] [--nr-ep {auto,cpu}]
                                 [--min-calibration-samples N]
                                 [--min-plcc R]
                                 [--allow-weak-calibration]
                                 [--dry-run] [-v]
```

Key options:

| Option | Default | Description |
|--------|---------|-------------|
| `--corpus PATH` | `.corpus/netflix/` | Directory of reference YUVs; if `PATH/ref/` exists, only that reference subdirectory is swept |
| `--output JSON` | `model/tiny/nr_metric_v1.json` | Sidecar JSON to update |
| `--crfs LIST` | `18,23,28,33,38` | CRF grid to sweep |
| `--max-clips N` | (all) | Limit clips for quick smoke runs |
| `--nr-ep auto\|cpu` | `auto` | `auto` tries CUDA/ROCm EP before CPU; `cpu` pins CPU inference for reproducible calibration or when CUDA is already busy |
| `--min-calibration-samples N` | `10` | Minimum fitted samples required before the JSON sidecar can be written |
| `--min-plcc R` | `0.70` | Minimum positive NR-vs-FR Pearson correlation required before the JSON sidecar can be written |
| `--allow-weak-calibration` | off | Diagnostic override that writes a weak sidecar with `calibration_quality_status = "weak"` |
| `--dry-run` | off | Compute δ_fast without writing the JSON |
| `--delta-fast VMAF` | (fit from data) | Force a specific δ_fast value |

Without a real corpus the script falls back to the YUVs under
`python/test/resource/yuv/` for a minimal smoke run. The in-tree Netflix
public corpus convention uses 1080p source filenames such as
`BigBuckBunny_25fps.yuv`; those names are recognised even though they do not
spell out `1920x1080`.

## FR call savings — example

On a 10-second 1080p source at `--target-vmaf 93` with a CRF window of
`[0, 63]` (the absolute range of `libsvtav1`) and `max_iterations=8`:

| Phase | CRF | NR score | δ_fast | Action |
|-------|-----|----------|--------|--------|
| iter 1 | 31 | 72.4 | 8.0 | NR_VMAF far below target → skip FR, lower CRF |
| iter 2 | 15 | 97.8 | 8.0 | NR_VMAF far above target → skip FR, raise CRF |
| iter 3 | 23 | 91.1 | 8.0 | NR_VMAF within zone → pay FR (92.8 measured) |
| iter 4 | 27 | 86.5 | 8.0 | NR_VMAF far below target → skip FR, lower CRF |
| iter 5 (final) | 25 | — | — | always FR: 93.1 measured ✓ |

Result: 5 bisect iterations, 2 FR calls (iterations 3 and 5), 3 FR calls
saved — **60% FR reduction**.

## Implementation details

- **Model**: `model/tiny/nr_metric_v1.onnx` — single-luma-frame MobileNet
  scoring from the middle frame of the distorted YUV.
- **Cache**: `NRProxyBackend` caches per `(path, width, height)` within a
  bisect run so repeated calls for the same CRF output are free.
- **GPU EP**: CUDA / ROCm execution provider is preferred when onnxruntime-gpu
  is installed; falls back to CPU EP silently.
- **Error handling**: any NR inference failure falls through to FR — correctness
  is never compromised.
- **Telemetry**: `BisectResult.fr_calls_total` and `.fr_calls_saved` carry
  the per-run savings count, exposed in INFO logs.

## See also

- [ADR-0615](../adr/0615-fast-nr-prescoring.md) — decision to implement NR
  early-elimination.
- [ADR-0624](../adr/0624-fast-nr-prescoring-impl.md) — implementation record.
- [Research-0611](../research/0611-fast-nr-prescoring-research.md) — design
  options and calibration plan.
- [`vmaf-tune compare`](vmaf-tune-compare.md) — compare subcommand.
- [`vmaf-tune tune-per-shot`](vmaf-tune-per-shot.md) — per-shot subcommand.
- [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md) — the bisect that `--fast-nr`
  accelerates.
- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.

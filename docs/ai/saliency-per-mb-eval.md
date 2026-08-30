# Saliency Per-Block Evaluation

`ai/scripts/eval_saliency_per_mb.py` evaluates saliency masks at the
granularity ROI encoders actually consume. Instead of reporting
full-resolution pixel IoU, it downsamples each mask to fixed-size
blocks, thresholds the block means, and reports macro / micro IoU over
paired predicted and ground-truth masks.

This is the ADR-0396 Phase-2 measurement harness for future
`video_saliency_student_v1` work. It is also useful for comparing
`saliency_student_v1` temporal aggregation modes because the output
matches the 16x16 macroblock or 64x64 CTU grids used by the tune ROI
paths.

## Inputs

The evaluator pairs files by stem across `--pred-dir` and `--gt-dir`.
Supported mask formats:

| Suffix | Shape | Notes |
| --- | --- | --- |
| `.npy` | 2-D numeric array | Values above 1 are normalised by the array max. |
| `.pgm` | P2 or P5 grayscale | Values are normalised by the PGM maxval. |

Masks must have identical width and height after loading.

## Usage

```shell
PYTHONPATH=. python ai/scripts/eval_saliency_per_mb.py \
  --pred-dir runs/saliency/pred \
  --gt-dir datasets/dhfik/masks \
  --block-size 16 \
  --threshold 0.5 \
  --out-json runs/saliency/per_mb_iou.json
```

Use `--block-size 16` for x264/libaom-style macroblock evaluation and
`--block-size 64` for SVT-AV1 / VVenC super-block or CTU evaluation.

## Output

The JSON payload has `schema_version: 1`, aggregate `macro_iou` /
`micro_iou`, one row per paired mask, and `run_provenance` for the CLI
entrypoint, argv, parsed arguments, input mask directories, and JSON output
path:

```json
{
  "schema_version": 1,
  "block_size": 16,
  "threshold": 0.5,
  "n_pairs": 1,
  "macro_iou": 0.75,
  "micro_iou": 0.75,
  "run_provenance": {"schema": "ai-run-provenance-v1"},
  "rows": [
    {
      "stem": "clip001_000120",
      "pred_blocks": 14,
      "gt_blocks": 16,
      "intersection_blocks": 12,
      "union_blocks": 18,
      "iou": 0.6666666666666666
    }
  ]
}
```

The full row also includes source paths, width, height, and block size.

## Reproducer

```shell
PYTHONPATH=. .venv/bin/python -m pytest ai/tests/test_eval_saliency_per_mb.py -q
```

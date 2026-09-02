# Research 0134: saliency per-block evaluation

## Question

What evaluation target should the video-saliency follow-up optimise
before a new tiny model is shipped?

## Findings

- ADR-0396 notes that encoder ROI paths consume downsampled block
  decisions, not full-resolution saliency maps.
- Pixel IoU can overstate improvements that disappear after 16x16 or
  64x64 reduction. Per-block IoU is closer to the actual x264/libaom
  macroblock and SVT-AV1/VVenC CTU control surfaces.
- A dependency-light evaluator can cover `.npy` and PGM masks, which
  is enough for training pipelines and exported saliency debug masks
  without adding Pillow or OpenCV to the base test path.

## Decision

Ship `ai/scripts/eval_saliency_per_mb.py` as the ADR-0396 Phase-2
measurement harness. It reports per-mask rows plus macro and micro IoU
after block reduction.

## Reproducer

```shell
PYTHONPATH=. .venv/bin/python -m pytest ai/tests/test_eval_saliency_per_mb.py -q
```

## References

- ADR-0396: Video-temporal saliency extension to `saliency_student_v1`
- req: "do more, we run out of pr's"

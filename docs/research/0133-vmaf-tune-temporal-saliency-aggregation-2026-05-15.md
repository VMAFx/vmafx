# Research 0133: vmaf-tune temporal saliency aggregation

## Question

Can `vmaf-tune recommend-saliency` close the ADR-0396 Phase-1 gap without
shipping a new video-saliency model?

## Findings

- ADR-0396 already scoped a cheap temporal baseline before training
  `video_saliency_student_v1`: keep `saliency_student_v1`, but reduce
  sampled per-frame masks with video-aware reducers.
- The shipped path already samples multiple frames and converts yuv420p
  to RGB before ONNX inference, so the missing work is the reducer, not
  model I/O or encoder sidecar plumbing.
- The safe compatibility posture is to keep `mean` as the default and
  expose `ema`, `max`, and `motion-weighted` as explicit operator
  choices. That lets the fork measure temporal baselines without
  changing existing saliency-aware encodes.

## Decision

Implement ADR-0396 Phase 1 in `vmaftune.saliency.compute_saliency_map`
and expose it through `vmaf-tune recommend-saliency
--saliency-aggregator`.

## Reproducer

```shell
PYTHONPATH=tools/vmaf-tune/src pytest tools/vmaf-tune/tests/test_saliency.py -q
```

## References

- ADR-0396: Video-temporal saliency extension to `saliency_student_v1`
- req: "do more, we run out of pr's"

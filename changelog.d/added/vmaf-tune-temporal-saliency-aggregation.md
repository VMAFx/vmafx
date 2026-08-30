`vmaf-tune recommend-saliency` now supports temporal saliency aggregation
via `--saliency-aggregator {mean,ema,max,motion-weighted}` and
`--saliency-ema-alpha`, implementing the ADR-0396 Phase-1 video-saliency
baseline without changing the default `mean` behaviour.

# `vmaf-tune` Ladder Default Sampler

The ladder default sampler is the production fallback used when
`vmaftune.ladder.build_ladder(..., sampler=None)` is called. It is
implemented in `tools/vmaf-tune/src/vmaftune/ladder.py::_default_sampler`.

## Contract

For each `(source, encoder, width, height, target_vmaf)` cell, the
sampler:

1. Chooses the codec adapter's `medium` preset when available, or the
   middle declared preset otherwise.
2. Runs the canonical 5-point CRF sweep:
   `18, 23, 28, 33, 38`.
3. Uses the normal `vmaftune.corpus.iter_rows()` encode+score path.
4. Picks the row closest to the requested VMAF target via
   `vmaftune.recommend.pick_target_vmaf()`.
5. Returns a `LadderPoint(width, height, bitrate_kbps, vmaf, crf)`.

The sampler deliberately stays replaceable. Operators that need a
different CRF grid, source shape, sample-clip policy, or precomputed
corpus should pass an explicit `sampler=` callable to `build_ladder()`.

## Defaults

| Setting | Value |
| --- | --- |
| CRF sweep | `18,23,28,33,38` |
| Pixel format | `yuv420p` |
| Framerate | `24.0` |
| Nominal duration | `1.0` second |
| Encode cleanup | temporary directory, deleted after each cell |

## Example Override

```python
from pathlib import Path
from vmaftune.ladder import LadderPoint, build_ladder

def sampler(src: Path, encoder: str, width: int, height: int, target: float) -> LadderPoint:
    return LadderPoint(width, height, bitrate_kbps=2400.0, vmaf=target, crf=23)

ladder = build_ladder(
    Path("ref.yuv"),
    "libx264",
    resolutions=[(1920, 1080), (1280, 720)],
    target_vmafs=[95.0, 92.0, 88.0],
    sampler=sampler,
)
```

## See Also

- [`vmaf-tune-bitrate-ladder.md`](vmaf-tune-bitrate-ladder.md) — CLI
  ladder workflow.
- [ADR-0307](../adr/0307-vmaf-tune-ladder-default-sampler.md) — sampler
  decision.
- [ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md) — ladder
  design.

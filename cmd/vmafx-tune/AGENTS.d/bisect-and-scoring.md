---
paths:
  - pkg/bisect/*.go
  - cmd/vmafx-tune/cmd/compare*.go
  - cmd/vmafx-tune/cmd/ladder*.go
invariant: Midpoint rounds higher (lo+hi+1)/2; keep ScoreFunc seam; Y4MScorer vs YUVScoreFunc distinct.
---
<!-- markdownlint-disable MD013 MD029 -->
# Bisect midpoint, `ScoreFunc` seam and Y4M vs raw-YUV scoring

## Rebase-sensitive invariants

3. **Bisect midpoint bias** (`pkg/bisect/bisect.go`): midpoint rounds toward
   *higher* CRF end `(lo + hi + 1) / 2` so best-so-far record never
   populated with unvalidated CRF. Changing rounding direction breaks
   monotonicity invariant.

4. **ScoreFunc seam** (`pkg/bisect/bisect.go`): `ScoreFunc` = subprocess
   boundary. Tests inject mock score functions. Never merge score function
   inline into `Run`; seam load-bearing for unit testability.

16. **`YUVScoreFunc` is not interchangeable with `Y4MScorer` /
    `VMAFScoreFunc`** (`pkg/bisect/`): `Y4MScorer` (`score_y4m.go`, used by
    `compare` and `ladder`; `VMAFScoreFunc` wraps one per call) decodes every
    input that is not `.y4m` to Y4M and calls `vmaf` with no geometry flags,
    so libvmaf's y4m branch reads both legs; raw `.yuv` input is refused.
    Handing `vmaf` Matroska encode directly made every compare and
    ladder probe fail while commands exited 0. `YUVScoreFunc` = raw-YUV
    path: decodes containerised distorted file to raw YUV first, passes
    `--width/--height/--pixel_format/--bitdepth/--model`. Those flags flip
    libvmaf's `use_yuv` branch, why `.y4m` deliberately **absent**
    from `rawYUVSuffixes` — Y4M header then trips file-size guard in
    `raw_input_open` (ADR-0499). ladder rung encodes with
    `bisect.Params.EncodeExtraArgs = -vf scale=W:H` and scores against
    reference rung's `Y4MScorer` decodes through same
    `bisect.ScaleFilter`; keep both legs on one filter. same rung scores
    with `Y4MScoreParams.Model = corpus.SelectVMAFModelVersion(W, H)` (4K model
    from 2160 lines, default below), as Python's `ladder` does (ADR-0289);
    empty `Model` (`compare`, `bisect`) leaves `--model` off. rule is pinned
    to `tools/vmaf-tune/tests/data/resolution_model_table.json` on both sides.
    QSV encodes append
    their upload to caller's `-vf` chain (`appendVideoFilter` in
    `pkg/encoder/hardware.go`); second `-vf` would drop scale.

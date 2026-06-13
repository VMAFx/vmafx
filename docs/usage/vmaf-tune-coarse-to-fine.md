# `vmaf-tune --coarse-to-fine`

`vmaf-tune corpus --coarse-to-fine` runs the ADR-0306 two-pass CRF
search instead of enumerating a full manual CRF grid. It is useful when
the operator has a target VMAF and wants the corpus rows needed to pick
the smallest acceptable bitrate without scoring every CRF in the codec
range.

The implementation lives in
`tools/vmaf-tune/src/vmaftune/corpus.py::coarse_to_fine_search` and is
wired through `tools/vmaf-tune/src/vmaftune/cli.py`.

## How It Works

1. Coarse pass: score a wide CRF sweep such as `10,20,30,40,50`.
2. Pick the coarse cell closest to `--target-vmaf`.
3. Fine pass: score a narrow window around that cell.
4. Emit the union of visited rows to the normal Phase-A JSONL schema.

With defaults, one source/preset visits up to 15 CRFs instead of a full
0..51 sweep:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --encoder libx264 \
    --preset medium \
    --coarse-to-fine --target-vmaf 92 \
    --output corpus_c2f.jsonl
```

## Flags

| Flag | Default | Notes |
| --- | --- | --- |
| `--coarse-to-fine` | off | Enables the two-pass search for `corpus`; `recommend` uses it by default. |
| `--target-vmaf` | — | Target used to centre the fine pass. |
| `--coarse-step` | `10` | CRF spacing for the coarse pass. |
| `--fine-radius` | `5` | Half-width around the best coarse CRF. |
| `--fine-step` | `1` | CRF spacing inside the fine window. |

## Output

Rows are ordinary `vmaf-tune` corpus rows. Downstream consumers do not
need a separate parser; `recommend`, predictor training, and Phase-B
bisect tooling can consume the JSONL as usual.

## See Also

- [`vmaf-tune.md`](vmaf-tune.md) — base tool documentation.
- [`vmaf-tune-recommend.md`](vmaf-tune-recommend.md) — target-picking
  consumer for coarse-to-fine rows.
- [ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md) — design
  decision and search strategy.

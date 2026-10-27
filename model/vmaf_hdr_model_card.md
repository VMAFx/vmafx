# HDR VMAF models — fork status

This file is **documentation, not a model**. Its `.md` extension keeps it out
of the model resolvers.

## What ships

The fork ships Netflix's HDR-VMAF v1.1.8 models, ported verbatim from upstream
Netflix/vmaf commit `74f930d17d`:

- `model/hdr_vmaf_v1.1.8/hdr_vmaf_v1.1.8_{3d0h,3d0h_2160,5d0h,1d5h_2160}.json`
- `model/hdr_vmaf_v1.1.8_hfr/hdr_vmaf_v1.1.8_hfr_*.json` (high frame rate
  variants)

All eight are also built into the library and can be loaded by version, for
example `--model version=hdr_vmaf_v1.1.8_3d0h`. Each model names the colour
conversion it expects (`conversion_target`). See
[the v1 model guide](../docs/models/v1.md) for the variants and their viewing
conditions.

The models are trained on PQ (SMPTE ST 2084) content.

## Which model is used

- **`vmaf` CLI**: when both inputs are tagged PQ and no `--model` is given,
  the CLI scores with `hdr_vmaf_v1.1.8_3d0h`; otherwise the SDR default stays
  in effect. See [the CLI guide](../docs/usage/cli.md).
- **`vmaf-tune`**: `select_hdr_vmaf_model()`
  ([`hdr.py`](../tools/vmaf-tune/src/vmaftune/hdr.py)) returns, in order, a
  `vmaf_hdr_v0.6.1.json`, for PQ sources the shipped
  `hdr_vmaf_v1.1.8/hdr_vmaf_v1.1.8_3d0h.json`, then any other
  `vmaf_hdr_*.json` under `model/`. See
  [vmaf-tune HDR scoring](../docs/usage/vmaf-tune-hdr-and-sampling.md).

## HLG sources still fall back to SDR

No HLG-trained model exists. For an HLG source `vmaf-tune` warns once per run
and scores with the SDR model (`vmaf_v0.6.1.json`), as
[ADR-0300](../docs/adr/0300-vmaf-tune-hdr-aware.md) describes. Treat such a
score as a lower bound: the SDR model never saw the HLG transfer during
training.

## How to verify which model the harness used

`vmaf-tune`'s corpus driver records the resolved model path in each output row
(the schema-v2 `model_path` field):

```bash
jq -r '.model_path' < corpus.jsonl | sort -u
# /…/model/hdr_vmaf_v1.1.8/hdr_vmaf_v1.1.8_3d0h.json   ← PQ source, HDR model
# /…/model/vmaf_v0.6.1.json                            ← SDR fallback (HLG)
```

## History

Before the port the fork shipped no HDR model; the search for one is recorded
in [research-0089](../docs/research/0089-hdr-vmaf-model-search.md).

## License

The model files are Netflix's, under BSD-2-Clause-Patent; the licence text is
`NOTICE` at the repository root. This documentation file is under the same
licence.

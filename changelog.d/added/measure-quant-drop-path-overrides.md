**`measure_quant_drop.py --fp32 / --int8` path overrides** (Research-2029 gap 5)

- `ai/scripts/measure_quant_drop.py` gained `--fp32 PATH --int8 PATH` to gate an
  explicit pair of ONNX files without a `model/tiny/registry.json` entry, plus
  `--budget FLOAT` (default `0.01`) for the PLCC-drop budget the registry would
  otherwise supply and `--id NAME` for the report label. Intended for PTQ / QAT
  scratch output, CI smoke artifacts, and pre-release gating of a freshly built
  checkpoint — the registry-driven `--all` and positional forms are unchanged.
- The overrides are rejected together with `--all` or a positional path (exit 2)
  and never load the registry. Documented in
  [`docs/ai/quantization.md`](../../docs/ai/quantization.md).

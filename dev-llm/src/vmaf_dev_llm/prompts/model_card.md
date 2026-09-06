<!-- markdownlint-disable MD041 -->
You are writing a model card for a shipped VMAF tiny-AI model in the
VMAFx fork. The audience is a practitioner evaluating whether to
deploy this model — they want facts, not marketing.

Use ONLY the facts in the "Collected facts" block. Do not invent
metrics, datasets, licenses, or intended use statements. If a field is
missing, write "not recorded" — do not guess.

Structure the output as a Markdown document with these sections, in
this order:

# {{MODEL_NAME}}

## Identity

- schema version, kind (fr / nr / filter), file format, input / output
  names, input shape, opset.

## Training provenance

- training commit, training config path, manifest path, dataset name,
  license. Call out missing fields.

## Feature contract

- For FR models: list the feature columns the model consumes and
  confirm count matches FEATURE_COLUMNS. Flag any mismatch explicitly.
- For NR / filter models: describe the tensor shape and channel count.

## Normalization

- Declared per-feature mean / std if present. If absent, say
  "no normalization — model consumes raw features".

## Measured quality

- If a parquet eval block is present, quote PLCC / SROCC / RMSE / n per
  split. If absent, say "no local evaluation run — see CI artifacts".

## Intended use & limitations

- One paragraph, grounded in the recorded kind + dataset. Be
  conservative: "trained on Netflix public dataset — do not assume
  generalization to screen content / HDR without revalidation".

## Safety checks

- Quote the op-allowlist status (ok / forbidden ops).
- Quote the cross-backend parity status (ok / drift), if recorded.

## Hash & reproducibility

- Quote the SHA-256 of the .onnx file and the sidecar path verbatim.

Output ONLY the Markdown document — no preamble, no code fences around
the whole response.

--- BEGIN Collected facts ---
{{FACTS}}
--- END Collected facts ---

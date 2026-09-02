# Research-0519: `--tiny-codec` / `--tiny-preset` / `--tiny-crf` CLI surface

- **Status**: Implemented (ADR-0519)
- **Date**: 2026-05-18
- **Companion ADR**: [ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md)

## Question

How should the `vmaf` CLI expose the codec / preset / CRF parameters
that codec-aware tiny models (today `fr_regressor_v2`) need to fill
their second-input "codec block", given that ADR-0518 unblocked the
loader but left the block pre-seeded to the "unknown" encoder bucket?

## Findings

### 1. The codec-block layout is fully pinned by the trainer

`ai/scripts/train_fr_regressor_v2.py` carries every constant needed
to reconstruct the block on the inference side:

- `ENCODER_VOCAB` (12 entries, append-only) — `libx264`, `libx265`,
  `libsvtav1`, `libvvenc`, `libvpx-vp9`, then six hardware encoders
  (`h264_nvenc`, `hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`,
  `av1_qsv`), then `unknown` at index 11.
- `PRESET_ORDINAL[encoder][preset_str] → int` mirroring each
  encoder's preset vocabulary. `PRESET_MAX_ORDINAL = 9.0`.
- `CRF_MAX = 63.0` as the union normaliser across encoders.
- Layout `[encoder_onehot(N_ENCODERS), preset_norm, crf_norm]`.

The sidecar (`model/tiny/fr_regressor_v2.json`) carries
`encoder_vocab`, `encoder_vocab_version`, and `codec_block_layout` so
the CLI can validate user input against the *exact* vocabulary the
shipped ONNX was trained against (forward-compatible with ADR-0302
v3 16-slot expansion).

### 2. The block must be set BEFORE the first `vmaf_read_pictures`

The ORT session reads `extra_in_buf` on every inference. Setting the
block once after `vmaf_use_tiny_model` (and before the frame loop)
is sufficient — every frame inference picks it up. This matches the
training-data assumption that codec is stream-constant (no frame-level
codec switching in the v2 trainer).

### 3. Three orthogonal flags vs. one combined flag

Considered: `--tiny-codec-context libx264:medium:28`. Rejected
because scripting "no preset, codec+CRF" needs sentinels
(`libx264::28`) that break shell quoting, and because the three
parameters truly are orthogonal — preset and CRF lookup tables are
indexed *by* the encoder name. Three flags compose better with
ladders that already pass `c:v` separately from `-preset` and `-crf`.

### 4. Unknown codec name: hard-error vs. silent bucket

The Python `codec_index()` silently buckets unknown names to
`unknown` (for corpora that genuinely don't know the codec). The C
CLI takes the stricter line: when the *user* explicitly passed
`--tiny-codec X`, an X not in the sidecar's `encoder_vocab` is a
typo or schema-mismatch, not a "genuine unknown". Hard-fail at
attach time so the typo doesn't silently fall back to the default
constant score. The underlying `vmaf_dnn_codec_block_fill` still
writes the `unknown` bucket and returns `-ENOENT` so library
consumers that want the Python-style soft-bucket behaviour can
ignore the return value.

### 5. PRESET_ORDINAL duplication

The C-side preset table mirrors the Python trainer's
`PRESET_ORDINAL` dict (lines 169..234 of
`ai/scripts/train_fr_regressor_v2.py`). Both must update together
when the trainer adds an encoder; the `core/src/dnn/AGENTS.md`
note flags both files as a co-edit pair. A cleaner long-term move
would be for the trainer to emit the table into the sidecar JSON so
the C side reads it, but that needs a sidecar-schema bump that
breaks the v2 model card — deferred until the v3 retrain PR.

### 6. Validation harness — score deltas confirm the path works

On the Netflix canonical 576x324 pair (`src01_hrc00` vs
`src01_hrc01`), the v2 model returns:

- `--tiny-codec` unset (default = "unknown" bucket): score 25.72.
- `--tiny-codec libx264 --tiny-preset medium --tiny-crf 28`: 52.45.
- `--tiny-codec libsvtav1 --tiny-preset 5 --tiny-crf 30`: 49.58.

Three distinct codec contexts yield three distinct scores, which
proves the second input is being read by the ORT session. The
absolute values matter less than the deltas — the trainer's
"unknown" baseline is one of 12 one-hot options and is the most
distributionally rare in the training corpus, so the score gap is
expected.

## Decision

See [ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md).

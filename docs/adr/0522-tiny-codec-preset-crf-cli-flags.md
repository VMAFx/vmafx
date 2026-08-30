<!-- markdownlint-disable MD013 MD060 -->
# ADR-0522: `--tiny-codec` / `--tiny-preset` / `--tiny-crf` populate codec one-hot block

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: `cli`, `ai`, `dnn`, `tiny-model`

## Context

ADR-0518 (PR #1278) unblocked the rank-2 feature-vector tiny models
shipped under `model/tiny/`, but stopped short of wiring the codec
identity into `fr_regressor_v2`'s second input. The loader pre-seeds
the 14-slot `codec` block to the "unknown" encoder one-hot
(index 11 in the v2 vocab, with `preset_norm=0` and `crf_norm=0`) so
inference still produces a finite score, but the model never sees the
real encoder context. The Consequences section of ADR-0518 explicitly
defers the user-facing API to "a future PR".

That follow-up is this ADR. Without it, every `vmaf --tiny-model
fr_regressor_v2.onnx …` invocation returns the same `VMAF=42.97` score
on the canonical Netflix 576x324 pair regardless of the actual codec
used to produce the distorted YUV — the conditioning vector is
constant. Downstream tooling (`vmaf-tune`, the MCP `run_tiny_inference`
probe, the `vf_libvmaf` filter) cannot benefit from the codec lift
documented in ADR-0235 / Research-0040.

The trainer pins the codec-block contract in
`ai/scripts/train_fr_regressor_v2.py`:

- `ENCODER_VOCAB` — closed, append-only, 12 slots today
  (`libx264`, `libx265`, `libsvtav1`, `libvvenc`, `libvpx-vp9`,
  `h264_nvenc`, `hevc_nvenc`, `av1_nvenc`, `h264_qsv`, `hevc_qsv`,
  `av1_qsv`, `unknown`).
- `PRESET_ORDINAL` — per-encoder dict mapping preset string to an
  integer in `[0, 9]`; the feature is normalised by
  `PRESET_MAX_ORDINAL = 9.0`.
- `CRF_MAX = 63` — union upper bound across all encoders; the feature
  is `crf / 63`.
- Layout: `[encoder_onehot(N_ENCODERS), preset_norm, crf_norm]`.

The sidecar `model/tiny/fr_regressor_v2.json` carries `encoder_vocab`,
`encoder_vocab_version`, and `codec_block_layout` so the C-side
validation can refuse unknown encoder names instead of silently
bucketing them into the "unknown" slot.

## Decision

Add a public `vmaf_dnn_set_codec_context(VmafContext *, const char
*codec, const char *preset, int crf)` entry point in `libvmaf/dnn.h`
and three CLI flags (`--tiny-codec`, `--tiny-preset`, `--tiny-crf`)
that call it after `vmaf_use_tiny_model`. The sidecar loader gains an
`encoder_vocab[]` field parsed from the same JSON, and the loader
ships a `vmaf_dnn_codec_block_fill` helper that builds the one-hot +
preset_norm + crf_norm block from the user-supplied parameters.

Concrete pieces:

- **Sidecar** (`model_loader.h`/`.c`):
  - `VmafModelSidecar` gains `n_encoder_vocab`, `encoder_vocab[]`,
    `codec_aware`. Parser reads `encoder_vocab` JSON array using the
    existing `extract_string_array` helper; missing = not codec-aware
    (silent no-op for non-v2 models).
  - `vmaf_dnn_codec_block_fill(buf, buf_len, vocab, n_vocab, codec,
    preset, crf)` writes the canonical layout; case-insensitive
    matching with the same `h264 → libx264` / `hevc → libx265` etc.
    aliases the trainer uses; per-encoder `PRESET_ORDINAL` table
    mirroring `train_fr_regressor_v2.py` lines 169..234.
- **Public API** (`include/libvmaf/dnn.h`,
  `src/dnn/dnn_attach_api.c`):
  - `vmaf_dnn_set_codec_context(ctx, codec, preset, crf)` looks up
    the attached sidecar's `encoder_vocab`, calls
    `vmaf_dnn_codec_block_fill` into `ctx->dnn.extra_in_buf`,
    returns `-ENOENT` when the codec name is non-NULL but unknown,
    `-ENOTSUP` when the attached model has no codec block (rank-4
    image or rank-2 single-input), `-EINVAL` on bad args.
- **CLI** (`tools/cli_parse.{c,h}`, `tools/vmaf.c`):
  - Three new long options. `--tiny-crf` accepts an unsigned integer;
    the runtime clamps to `[0, 63]`.
  - `configure_tiny_codec_context` runs immediately after the tiny
    model loads; when the model has a codec block but the user
    supplied no flags, the pre-seeded "unknown" baseline stays in
    place (no behaviour change vs ADR-0518).
- **Tests** (`core/test/dnn/test_codec_block.c` +
  `test_cli.sh` extension):
  - C unit test: `vmaf_dnn_codec_block_fill` produces the right
    layout for `(libx264, medium, 28)` and returns `-ENOENT` for
    `unknown_enc`.
  - CLI smoke test: `--tiny-codec libx264 --tiny-preset medium
    --tiny-crf 28` against `fr_regressor_v2.onnx` produces a VMAF
    score that differs from the "unknown"-default 42.97.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Auto-probe the codec from the distorted YUV via `ffprobe` | Zero-config UX | YUV streams carry no codec metadata; the distorted file is raw decoded planes by the time libvmaf sees it; requires a sibling container file path the CLI does not own | Cannot infer codec from the data libvmaf actually receives |
| Single combined `--tiny-codec-context libx264:medium:28` flag | Fewer flags to remember | Encoding "no preset, just codec+CRF" needs sentinels (e.g. `libx264::28`) that are hostile to scripting; conflicts with users who already script `--tiny-codec` as a positional | Three orthogonal flags compose better with shell tooling |
| Make the codec context per-frame (variable-bitrate streams) | Models per-frame codec switches | No shipped model is trained on per-frame codec switches; per-frame would change `extra_in_buf` lifetime + require buffering | Out of scope; the v2 trainer treats codec as stream-constant |
| Read `--tiny-codec` from an env var (e.g. `VMAF_TINY_CODEC`) | One less flag in command lines | Hides the parameter in the environment, defeats the documentation goal | CLI surface is the canonical place for inference-time configuration |
| Reject unknown codecs hard instead of bucketing to "unknown" | Catches typos | Breaks valid corpus-tagging use cases where the encoder is genuinely unknown (Netflix Public corpus); contradicts trainer behaviour | Return `-ENOENT` from the API so callers can choose; the CLI hard-errors only when the user *passed* a name, never on absence |

## Consequences

- **Positive**:
  - `fr_regressor_v2.onnx` produces meaningful per-codec score
    differentiation for the first time in this fork.
  - The codec-block layout is documented and validated against the
    sidecar — typos in encoder names fail with a clear error at
    attach time, not at training data time.
  - `vmaf-tune` and the MCP `run_tiny_inference` probe can now
    propagate the ladder's `c:v` argument straight through to
    inference without any per-tool plumbing.
  - The helper `vmaf_dnn_codec_block_fill` lives in `model_loader.c`
    next to the sidecar parser; future tiny models with the same
    layout (v3 16-slot vocab per ADR-0302) only need to bump the
    `encoder_vocab` JSON array and ship a new sidecar — no C
    re-compile.
- **Negative**:
  - The PRESET_ORDINAL table is duplicated between Python
    (`train_fr_regressor_v2.py`) and C (`model_loader.c`). The
    AGENTS.md note under `core/src/dnn/` flags both sites as a
    co-edit pair; a future refactor could move the table into the
    sidecar JSON itself so the C side reads it instead of duplicating.
  - The "unknown" pre-seed remains the default for callers that omit
    the flags — this preserves ADR-0518 behaviour for backwards
    compatibility but means `vmaf --tiny-model fr_regressor_v2.onnx`
    still returns 42.97 unless the user opts in.
- **Neutral / follow-ups**:
  - When ENCODER_VOCAB bumps to v3 (16 slots, ADR-0302), the
    sidecar-driven validation surface here works unchanged; only the
    `unknown` slot index moves and the `model_loader.c` resolution
    follows the sidecar.
  - The same API powers future `vmaf-tune` and `vf_libvmaf` plumbing
    by exposing one stable entry point.

## References

- `req`: agent dispatch brief "Add `--tiny-codec`, `--tiny-preset`,
  `--tiny-crf` CLI flags to `vmaf` so codec-aware tiny models (e.g.
  `fr_regressor_v2`) get their codec one-hot block populated instead
  of being pre-seeded to 'unknown' at attach time (per ADR-0518
  Consequences)." (2026-05-18).
- ADR-0518 — Tiny-model loader accepts external-data and feature-vector
  ONNX (the predecessor that introduced the pre-seeded "unknown"
  baseline this ADR replaces).
- ADR-0235 — Codec-aware FR regressor (motivation).
- ADR-0302 — `ENCODER_VOCAB` v3 16-slot schema expansion (forward
  compatibility).
- ADR-0291 / ADR-0272 — `fr_regressor_v2` model card.
- ADR-0042 — Tiny-AI docs required per PR (drives the
  `docs/ai/inference.md` update in this PR).
- Trainer source of truth:
  `ai/scripts/train_fr_regressor_v2.py` (`ENCODER_VOCAB`,
  `PRESET_ORDINAL`, `CRF_MAX`, `_row_to_features`).
- Sidecar: `model/tiny/fr_regressor_v2.json` (`encoder_vocab`,
  `encoder_vocab_version`, `codec_block_layout`).

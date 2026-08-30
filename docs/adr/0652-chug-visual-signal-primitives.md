<!-- markdownlint-disable MD013 MD060 -->
# ADR-0652: Add CHUG Visual-Signal Primitives

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: ai, chug, hdr, features, training

## Context

The signal-mix audit found that CHUG HDR MOS rows still lack explicit
noise, grain, and blur signals. Canonical libvmaf features capture detail
loss and motion, but they do not expose cheap row-local primitives that
make it easy to test whether HDR MOS errors cluster around texture loss,
blur, grain, or high-frequency noise. CHUG extraction already decodes
reference and distorted clips to aligned 10-bit YUV before running libvmaf,
so the materialiser can compute luma-domain side signals without another
decode pass.

## Decision

We will compute four cheap decoded-luma primitives for every CHUG
reference and distorted clip during feature materialisation:
`luma_std`, `sharpness_laplacian_var`, `highfreq_abs_mean`, and
`noise_lap_mad`. The emitted row carries `feature_ref_*`,
`feature_dis_*`, and `feature_delta_*` distorted-minus-reference fields
for those primitives. The fields are diagnostic model inputs, not a
replacement for a trained no-reference VQA model.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wait for a trained NR/noise model | Stronger semantic signal after training | Leaves the immediate CHUG table blind to blur/noise/grain axes | Rejected because cheap primitives unblock correlation audits now |
| Add only one sharpness metric | Minimal schema expansion | Collapses blur, grain, and noise into one ambiguous value | Rejected because the audit called out separate missing signal families |
| Compute signals in a separate post-processor | Keeps `chug_extract_features.py` smaller | Requires another full pass over local videos or brittle joins against shards | Rejected because the decoded YUVs already exist inside the materialiser |

## Consequences

- **Positive**: CHUG feature rows gain direct blur/noise/grain proxies with
  no additional operator command.
- **Negative**: Feature extraction performs a small NumPy pass over up to
  eight sampled decoded frames per side when the visual-signal cache is
  missing.
- **Neutral / follow-ups**: Later trainer schemas can decide whether to
  consume these primitives directly, combine them with saliency, or replace
  them with learned NR features after model validation.

## References

- [ADR-0650](0650-signal-mix-audit.md)
- [ADR-0651](0651-chug-hdr-row-metadata.md)
- Source: `req` — "well and in this audit perhaps find gaps that we have no metric/signal for at all or so"
- Source: `req` — "yeah every possible gain through intersection (even of not yet included metrics)"

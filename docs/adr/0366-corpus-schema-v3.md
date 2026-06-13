<!-- markdownlint-disable MD060 -->
# ADR-0366: vmaf-tune corpus schema v3 — canonical-6 per-feature aggregates

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: lusoris, claude
- **Tags**: `ai`, `tools`, `vmaf-tune`, `corpus`, `schema`

## Context

The schema-v2 vmaf-tune corpus row records only the pooled
``vmaf_score`` per encode and drops the libvmaf per-feature aggregates
that the libvmaf CLI already emits in its JSON output. The
codec-aware FR regressors (``train_fr_regressor_v2.py`` and
``train_fr_regressor_v3.py``) need the canonical-6 per-feature signals
(``adm2``, ``vif_scale0..3``, ``motion2``) to learn beyond what
``vmaf_score`` already implies. The
[partial-integration audit
(``docs/research/0091-partial-integration-audit-2026-05-08.md``)](../research/0091-partial-integration-audit-2026-05-08.md)
called this out: ``train_fr_regressor_v2.py`` carries an inline TODO
documenting the absence and falls back to a synthetic-data smoke
mode; ``train_fr_regressor_v3.py`` raises ``ValueError`` on any
real-corpus DataFrame because the columns it asserts (``adm2``, ...,
``vmaf``, ``cq``, ``frame_index``) do not exist on disk.

We could keep the trainer-side workaround (a sidecar JSON written
out-of-band by a wrapper script that re-runs libvmaf in feature-extraction
mode) but that doubles the encode wall-time, breaks the single-source-of-
truth contract for the corpus row, and forces every downstream consumer
to know about the sidecar.

## Decision

We extend the corpus row schema to v3 by adding the canonical-6
libvmaf features as 12 mean / std aggregate columns (``adm2_mean``,
``adm2_std``, ``vif_scale[0..3]_{mean,std}``, ``motion2_{mean,std}``)
parsed directly from libvmaf's existing ``pooled_metrics`` block. The
``vmaftune.score.run_score`` driver already runs libvmaf with the
features registered by the active model; we surface the per-feature
``mean`` / ``stddev`` it already computes via
``parse_feature_aggregates`` and the corpus row writer projects them
into the new columns. Features the active model does not expose (e.g.
the cambi-only path) become ``NaN`` in the row, never ``0.0`` — the
trainer drops the row instead of training on synthetic zeros. The
reader (``vmaftune.corpus.read_jsonl``) loads schema-v2 rows and
back-fills the missing columns with ``NaN`` so legacy corpora do not
crash newer consumers.

The schema-v3 trainers (v2 + v3) read the columns directly from the
corpus DataFrame; the previous out-of-band sidecar JSON path is
removed. ``train_fr_regressor_v2.py`` keeps its ``--smoke`` path as a
documented diagnostic flag (the smoke synthesises both
``per_frame_features`` and the v3 ``<feature>_mean`` columns so both
materialisation paths in ``_row_to_features`` are exercised).
``train_fr_regressor_v3.py`` retires its synthetic-by-default fallback
and demands a v3 corpus on the real path; ``--smoke`` continues to
synthesise a v3-shaped corpus for pipeline integrity checks but the
exported registry row is always tagged ``smoke: true``.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Status quo (sidecar JSON) | No schema change | Doubles encode wall-time, two sources of truth, every consumer needs sidecar awareness | Already documented as the partial-integration debt that motivated this ADR |
| Per-frame rows in the corpus | Maximally rich | 30-100x row inflation, breaks Phase B coarse-to-fine row counts, schema overhauled | Aggregates are the input the trainers actually use |
| Add canonical-6 + extra metrics (cambi, psnr, ssim, ms-ssim) in one bump | One schema change covers Phase D too | Wider blast radius, harder to land before downstream consumers stabilise | Defer additional metrics to a future v4 bump once consumers settle |
| Out-of-band per-encode log file with the libvmaf JSON | Zero corpus-row impact | Inflates dev surface, shells the same parser, still leaves the trainers blocked | Same problem as sidecar JSON |

## Consequences

- **Positive**: ``train_fr_regressor_v2.py`` and
  ``train_fr_regressor_v3.py`` consume canonical-6 per-feature signals
  directly from the corpus DataFrame. The synthetic fallback that
  ``train_fr_regressor_v3.py`` defaulted to on real corpora is
  removed — runs that fail to load the v3 schema fail loudly with a
  pointer to this ADR. Phase B / C consumers get one source of truth
  for per-feature data and can drop NaN rows (cambi-only fixtures,
  encode failures) instead of training on invented zeros.
- **Negative**: The on-disk row width grows by 12 columns. Estimated
  +220 bytes per row on a typical sweep (52 CRFs × 5 presets × 9
  sources × 6 codecs ≈ 14k rows ⇒ ~3 MB additional disk for a full
  corpus), which is within budget. Schema bump from 2 to 3 means
  every consumer pinned to ``SCHEMA_VERSION == 2`` needs a forward
  bump or to use the back-compat reader.
- **Neutral / follow-ups**: ``ShotFeatures`` (``predictor_features.py``)
  is not yet wired to the new mean / std columns — that is a
  separate PR (and that work is out of scope here).
  ``hw_encoder_corpus.py`` may still emit per-frame rows with bare
  feature names; downstream ensemble-LOSO trainers consume that
  shape and are unaffected by this PR.

## References

- Audit digest: ``docs/research/0091-partial-integration-audit-2026-05-08.md``.
- ADR-0237 (vmaf-tune Phase A umbrella).
- ADR-0297 / ADR-0301 (schema v2 sample-clip mode).
- ADR-0272 (codec-aware FR regressor v2 scaffold).
- ADR-0291 (fr_regressor_v2 prod ship).
- ADR-0302 (encoder-vocab v3 schema expansion).
- ADR-0323 (fr_regressor_v3 train and register).
- Source: `req` — task brief 2026-05-08 to "ship the corpus-schema
  bump that unblocks ``train_fr_regressor_v2.py`` and
  ``train_fr_regressor_v3.py`` consuming canonical-6 per-frame
  features directly from the corpus".

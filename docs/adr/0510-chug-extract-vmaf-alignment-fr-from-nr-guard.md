<!-- markdownlint-disable MD013 MD060 -->
# ADR-0510: CHUG re-extract VMAF-alignment fix — FR-corpus guard on the FR-from-NR extractor

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: ai, corpus, chug, k150k, extractor, training-data, regression-guard

## Context

The 2026-05-18 CHUG re-extract at
`.workingdir/dev-mcp-probes/chug_reextract/full_features_chug.parquet`
shipped 5992 rows × 72 cols with VMAF clustered tightly around 99
across every bitrate-ladder rung, including 360p @ 0.2 Mbps — a
configuration that should physically score in the 30–60 band. The
parquet's identity-pair fingerprint was unambiguous:

| column           | min      | mean     | max      | NaN count |
|------------------|----------|----------|----------|-----------|
| `adm2_mean`      | 1.0000   | 1.0000   | 1.0000   | 0         |
| `vif_scale0..3`  | 1.0000   | 1.0000   | 1.0000   | 0         |
| `psnr_y_mean`    | 60.0000  | 60.0000  | 60.0000  | 0         |
| `ciede2000_mean` | NaN      | NaN      | NaN      | 5992/5992 |
| `psnr_hvs_mean`  | NaN      | NaN      | NaN      | 5992/5992 |
| `vmaf_mean`      | 97.4480  | 99.5856  | 99.9915  | 0         |

These are the documented identity-pair floor values for the
FR-from-NR adapter (ADR-0346 / ADR-0362, `ai/AGENTS.md` §K150K-A
corpus extraction invariants). Manual re-derivation of one
360p_0.2M_ row against its real 1080p reference via the FR-aware
script and `chug_extract_features.py`'s scaling policy produced
`adm2=0.775`, `vif_scale0=0.276`, `vmaf=27.98` — confirming the
underlying corpus and the FR-aware pipeline are correct.

Root cause: the parquet was produced by
`ai/scripts/extract_k150k_features.py`, not
`ai/scripts/chug_extract_features.py`. The K150K script is an
**FR-from-NR adapter for genuinely no-reference corpora**
(KoNViD-150k-A): it passes the same decoded YUV as both
`--reference` and `--distorted` to the libvmaf CLI on purpose.
When that pipeline is pointed at a full-reference corpus like
CHUG (which ships one `chug_ref==1` reference plus six
bitrate-ladder distortions per `chug_content_name`), every clip
is scored against itself and the parquet carries zero training
signal — exactly what the 2026-05-18 extract demonstrated.

The bug is operator-level (wrong script for the corpus), not a
logic bug inside either extractor. But the misuse is silent: no
exit-code, no warning, no per-row provenance flag distinguishes
"identity-pair feature dump" from "genuine FR feature pair". The
operator only noticed after pandas inspection of the resulting
parquet, several GPU-hours into the run.

## Decision

1. **Add a refuse-and-explain FR-corpus guard to
   `ai/scripts/extract_k150k_features.py`.** When the
   `--metadata-jsonl` sidecar advertises real reference rows
   (any `chug_content_name` group containing at least one row
   with `chug_ref==1` AND at least one with `chug_ref==0`), the
   script exits 2 before spawning any worker process and points
   the operator at `ai/scripts/chug_extract_features.py`. The
   detection is in a new public helper `detect_fr_corpus_misuse()`
   so callers and unit tests can probe it without invoking
   `main()`.
2. **Bypass flag `--allow-fr-from-nr`** for the rare case where
   operators genuinely want self-vs-self scoring on an FR corpus
   (e.g., comparing FR-from-NR features across corpora to study
   the identity-pair floor itself). The flag must be explicit;
   it carries no default-on path.
3. **Pin the FR-aware contract on `chug_extract_features.py` with
   two regression tests**:
   `test_chug_pairing_never_uses_identity_pairs_for_distorted_rows`
   (asserts `ref_path != dis_path` for every emitted pair) and
   `test_chug_pairing_skips_distorted_rows_without_matching_reference`
   (asserts orphan distorted rows are dropped rather than
   silently falling back to identity). Plus an end-to-end smoke
   test against synthetic ref/dis YUV (`test_chug_extract_features_smoke.py`)
   that asserts `adm2_mean < 0.95` on a deliberately destroyed
   distorted clip — gates the actual VMAF subprocess invocation
   when the binary is available; otherwise skipped.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Refuse-and-explain guard with explicit bypass flag (chosen) | Operator-friendly error; preserves existing FR-from-NR semantics for K150K-A; no schema change; covers both worker pools and per-clip overrides | Adds one CLI flag + one detection helper | None — strictly additive, zero behaviour change for K150K-A users |
| Hard-block the script on any `chug_*` sidecar field | Simpler check | Breaks the legitimate workflow of FR-from-NR study on CHUG identity pairs and forbids future NR-augmented CHUG variants | Too broad; would force a NOLINT-style escape hatch the first time someone wants identity-pair stats |
| Add a per-row `feature_alignment="identity_pair"` provenance column and let downstream training drop those rows | Self-describing parquet | Operator still burns GPU-hours producing useless rows; provenance after-the-fact rather than gate-before-the-run | Punts the cost; the 2026-05-18 run was 3.6 wall hours that should never have started |
| Detect the floor in `_aggregate_frames` and zero out the row | Fails after the fact rather than refusing the run | Pollutes the parquet with NaN columns and obscures the actual symptom in downstream training | Same as above — wrong layer; the cost is the GPU-time, not the storage |

## Consequences

- **Positive**: the FR-corpus misuse cannot recur silently. The
  next operator who points the K150K script at a CHUG sidecar
  sees an exit-2 error naming the right script (and an example
  `chug_content_name` so they can verify the guard fired for the
  right reason). Three new regression tests pin the contract on
  the FR-aware extractor — both the pairing logic and the
  end-to-end behaviour on synthetic YUV.
- **Negative**: one new CLI flag (`--allow-fr-from-nr`) on
  `extract_k150k_features.py` widens the surface that K150K
  operators must reason about. Documented in `ai/AGENTS.md`
  K150K-A invariants.
- **Neutral / follow-ups**: the bad parquet at
  `.workingdir/dev-mcp-probes/chug_reextract/full_features_chug.parquet`
  is gitignored and will be deleted by the operator. A
  re-extract on the same 5992-row CHUG manifest using
  `ai/scripts/chug_extract_features.py` is queued (~3–4 wall
  hours on the dev-mcp CUDA lane per the user's
  cross-backend-multiplexing rule). The smoke test only requires
  ~10 GPU-seconds on six 320×240 frames; the FR-corpus guard
  test is pure Python and runs in <100 ms.

## References

- ADR-0346 (FR-from-NR adapter pattern), ADR-0362 (identity-pair
  metric degeneration), ADR-0382 (`extract_k150k_features.py`
  parallelism), ADR-0431 (`ssimulacra2` schema-v2 omission),
  ADR-0427 (CHUG FR-aware materialiser).
- Same-family precedent: ADR-0503 (BBB v5 cluster
  `source_is_container=True` propagation) — different code path
  but the same symptom shape: an upstream pipeline misconfiguration
  silently produced systematically-wrong VMAF scores across the
  entire output, and the fix is a refuse-or-correct guard at the
  point where the misconfiguration enters the pipeline.
- Source: `req` ("CRITICAL bug fix in VMAFx/vmafx … VMAF≈99 for
  ALL CHUG bitladder rows — including 360p @ 0.2 Mbps (which
  should physically be VMAF 30-60, not 99)") — verbatim quote
  from the dispatch request that opened this fix; paraphrased to
  neutral English in `## Context` per CLAUDE.md §13 / global rule
  "User-quote handling in project artifacts".

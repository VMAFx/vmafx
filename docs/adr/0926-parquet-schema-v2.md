<!-- markdownlint-disable MD060 -->
# ADR-0926: Parquet schema v2 — canonical column order, zstd-3, schema metadata

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: ai, data, storage, parquet, k150k, chug

## Context

The fork's AI training and evaluation pipelines emit parquet files at several
points in the stack: K150K feature extraction (`extract_k150k_features.py`),
CHUG sweeps, KonViD-1k full-feature builds, BVI-DVC sweeps, eval-LOSO reports,
and metadata enrichment. Corpora at scale (K150K is approximately 150 000
clips, CHUG is hundreds of GB) make storage cost and read-side ergonomics
first-class concerns.

Two pain points showed up repeatedly:

1. **Compression default is snappy.** Snappy is fast but its compression
   ratio is poor on the float / categorical mix typical of VMAF feature
   tables. On real K150K-shape data with repeating clip ids and per-clip
   shared MOS, switching to zstd-3 reclaims roughly 20-30 % of disk at
   comparable read/write CPU. On float-dense synthetic data the win is
   smaller (about 6 %), but real data is rarely float-dense once metadata
   and identifiers are included.
2. **Column order is whatever pandas decided.** Downstream readers — the
   training loop, `validate_norm.py`, eval scripts, the manifest enricher —
   re-discover "which column holds the score" by name lookup at every site.
   This is harmless but it adds boilerplate to every new script.

The fork also has no way to *detect* what produced a parquet file. A reader
cannot tell whether a file came from a v1 ad-hoc `df.to_parquet(...)` call,
from a snappy-compressed bulk extraction, or from a fresh run with the
current column conventions — and that ambiguity has blocked at least one
in-flight refactor.

## Decision

Define a **parquet schema v2** that the central
`aiutils.parquet_utils.write_parquet_atomic` helper produces by default, and
keep the v1 reader path intact so legacy files remain consumable.

The v2 contract:

1. **Canonical column order**: `[clip_id?, clip_name?, frame_idx?,
   frame_index?, ...features (alphabetical), ...labels, ...metadata]`.
   Labels and metadata are identified from a small built-in allowlist
   (`mos`, `dmos`, `score`, `*_label`, `*_target` → labels; `source`,
   `split`, `codec`, `*_hash`, ... → metadata) or from explicit `labels=`
   / `metadata=` lists the caller passes.
2. **Compression**: `zstd` at `compression_level=3` by default. Callers may
   override via `compression="snappy"` or `compression_level=N`; the
   override is honoured exactly.
3. **File-level metadata**: pyarrow custom-metadata block carries
   `vmafx_schema_version=2` (ASCII bytes) and `vmafx_pipeline_hash=<12-char
   git short SHA>`. The hash is best-effort; an empty value is acceptable
   in environments without git.
4. **Reader**: a new `read_parquet_with_schema(path)` returns
   `(dataframe, schema_version)`. Files with no `vmafx_schema_version`
   key are reported as v1. The simple `pd.read_parquet(...)` path keeps
   working for both versions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| zstd-9 (max ratio) | Another 5-10 % smaller | 2-4x write CPU; marginal benefit | The default has to stay cheap on the extraction hot path. Cold archives can opt into a higher level. |
| brotli | Best ratio on tabular text | Pyarrow brotli support varies across builds | Pyarrow zstd is the de-facto standard; portability beats marginal ratio. |
| LZ4 | Faster than snappy | Worse ratio than snappy | We want more compression, not less. |
| Schema versioning in a sidecar `.manifest.json` | No format coupling | Sidecar drift is a real failure mode (see the K150K resume-set bug); two files travel together poorly. | Inline pyarrow custom-metadata is a single source of truth and survives `cp` / `rclone copy`. |
| Hard-code a column allowlist (no heuristics) | Predictable | Every new feature requires a code change | Heuristics keep the helper usable for ad-hoc tables; explicit `labels=` / `metadata=` is available for callers that need precision. |
| Bump compression default but skip column reorder | Smaller PR | Doesn't address the "find the score column" pain point | The two changes share the same write path; landing them together is one ADR and one rebase note. |

## Consequences

- **Positive**:
  - K150K / CHUG cold storage shrinks roughly 20-30 % on real data
    (mix of floats plus repeating categorical identifiers);
    test-synthetic random-float data sees about 6 %.
  - Downstream readers can rely on column 0 being `clip_id` (when present)
    and the score column being in a predictable block; this opens the door
    to deleting per-script "find score column" boilerplate in a follow-up.
  - `vmafx_pipeline_hash` makes "which build wrote this?" answerable
    without sidecar files.
  - `vmafx_schema_version` future-proofs further format changes — v3 can
    branch on the same key.

- **Negative**:
  - Files written by older callers (`df.to_parquet(...)` directly) remain
    v1; the codebase keeps two read paths in mind until the call sites
    are migrated.
  - Zstd decompression is slightly slower than snappy on large reads;
    the difference is negligible on PCIe NVMe with modern CPUs but is
    measurable on cold spinning disk or RAM-bound workers.

- **Neutral / follow-ups**:
  - Migrate the direct `df.to_parquet(...)` call sites in
    `ai/scripts/` to `write_parquet_atomic` in a follow-up sweep
    (tracked separately — out of scope for this ADR to keep the change
    surface small).
  - Once all writers are migrated, evaluate raising the default zstd
    level to 5 for archival writes.

## References

- Source: req — user direction for modernization #19, 2026-05-31, to
  "standardize Parquet column ordering + switch snappy to zstd-3
  compression + add schema_version metadata".
- Related: `ai/src/aiutils/parquet_utils.py`,
  `ai/src/aiutils/AGENTS.md`.
- ADR-0221 (changelog fragment pattern).
- ADR-0108 (deep-dive deliverables rule).

<!-- markdownlint-disable MD013 MD060 -->
# ADR-0349: `fr_regressor_v3` namespace — reserve `_v3plus_features` for the next feature-set bump

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (planning agent)
- **Tags**: `ai`, `docs`, `naming`

## Context

[ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) bumped `ENCODER_VOCAB`
from 13 to 16 slots. [ADR-0323](0323-fr-regressor-v3-train-and-register.md)
shipped the matching production checkpoint as `fr_regressor_v3` (PR #428,
merged 2026-05-06). The registry row carries `smoke: false` and a fixed
sha256 (`eaa16d23461eda74940b2ed590edfcaf13428aade294e47792a5a15f4d3b999c`);
[ADR-0291](0291-fr-regressor-v2-prod-ship.md) production-flip semantics make
that checkpoint immutable in-place.

A separate workstream wants to introduce a second axis of variation on the
same regressor lineage: the canonical-6 + `encoder_internal` + shot-boundary

- `hwcap` feature-set superset (driven by ADR-0235's codec-aware extension
plus the TransNet shot-boundary surface and the hardware-capability fingerprint
from the vmaf-tune corpus). That work was originally referred to as
"feature-set v3" in agent reports, which collides verbatim with the
`fr_regressor_v3` name already on master.

The collision is purely cosmetic — the schema axis (encoder vocab) and the
feature axis (input dimensionality + auxiliary heads) are orthogonal — but
two artefacts cannot share the same registry id. Investigation against
master tip `2c2f9ad7`:

- `fr_regressor_v3` is referenced in **19 call sites across 12 files**
  (registry row, sidecar, model card, trainer + tests, ADR-0302 / ADR-0323,
  Research-0078 / 0088, `ai/AGENTS.md` invariant block, changelog fragment,
  `docs/rebase-notes.md`).
- One merged PR claims the name (#428 — `feat(ai): fr_regressor_v3 — train +
  register on ENCODER_VOCAB v3 (16-slot)`).
- The registry already uses `vmaf_tiny_v3` and `vmaf_tiny_v4` for an
  unrelated MLP-capacity sweep — version-suffix overloading is established
  precedent in this tree, so a name collision on `_v3` would silently
  conflate two distinct lineages.

## Decision

Keep `fr_regressor_v3` as the live production name (no renames, no file
moves, sha256 unchanged) and reserve `fr_regressor_v3plus_features` as the
namespace for the canonical-6 + `encoder_internal` + shot + `hwcap`
feature-set bump. The reservation is documentation-only at this ADR's
landing — the registry stays unchanged because
[`core/test/dnn/test_registry.sh`](../../core/test/dnn/test_registry.sh)
treats every registry row as a hard contract (file must exist, sha256 must
match, sidecar must accompany every `smoke: false` entry). A stub row would
fail the test on day one. The future PR that ships the new model populates
the row in the same commit that lands the `.onnx`.

The reservation is enforced via:

- This ADR (the canonical citation).
- A new namespace-invariant block in [`ai/AGENTS.md`](../../ai/AGENTS.md)
  — preventing future agents from re-using `fr_regressor_v3plus_features`
  for an unrelated workstream.
- A status-update appendix on [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md)
  per [ADR-0028](0028-adr-maintenance-rule.md) (Accepted-ADR immutability)
  pointing forward to this ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) Rename existing v3 to `fr_regressor_v3_vocab16` | Frees the `_v3` name for a future major bump; keeps semantic versioning clean | Touches 19 call sites + the production registry sha256 contract; breaks downstream model-loader consumers; renames a `smoke: false` checkpoint that PR #428 already promoted | Real cost (production breakage) for a cosmetic gain; violates the production-flip immutability ADR-0291 establishes |
| (b) Rename future feature-set to `fr_regressor_v4_features` | No touch to existing v3; no migration | Inflates "v4" to mean "name-conflict workaround", not a real architectural bump; pollutes the major-version axis we genuinely need for future regressor redesigns | Wastes a major-version slot on bookkeeping |
| (c) **Reserve `fr_regressor_v3plus_features`** (chosen) | Keeps existing v3 untouched; clearly signals "v3 plus extra feature axes"; preserves `_v4` for genuine architectural bumps; namespace reserved in docs without a stale registry row | Slightly more verbose name; reservation enforced by docs not by registry validator | Best balance: zero migration cost, no major-version inflation, future-proof |

## Consequences

- **Positive**: production `fr_regressor_v3` registry row (sha256
  `eaa16d23…`) stays bit-identical; PR #428 consumers untouched; `_v4`
  preserved for genuine architectural bumps; future feature-set PR has
  a clear, documented namespace to claim.
- **Negative**: namespace-reservation enforcement is documentation-only
  until the future PR fills the slot — relies on `ai/AGENTS.md` being
  read by agents working in the area. Mitigation: the AGENTS.md note
  cites this ADR by number; agents working on `fr_regressor_*` already
  re-read `ai/AGENTS.md` per CLAUDE §9.
- **Neutral / follow-ups**: when the canonical-6 + `encoder_internal` +
  shot + `hwcap` feature-set work lands, that PR (a) populates
  `fr_regressor_v3plus_features` in `model/tiny/registry.json`, (b)
  ships the `.onnx` + sidecar, (c) cites this ADR in its References,
  and (d) supersedes the AGENTS.md "reserved, not yet shipped" note
  with the actual model card link.

## References

- [ADR-0028](0028-adr-maintenance-rule.md) — Accepted-ADR immutability
  rule that forces the status-update appendix on ADR-0302 (rather than
  in-place rewrite).
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — production-flip
  semantics that make `smoke: false` registry rows immutable in-place.
- [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) — the schema
  bump that motivated the `fr_regressor_v3` name.
- [ADR-0323](0323-fr-regressor-v3-train-and-register.md) — production
  shipment of `fr_regressor_v3` (PR #428).
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware regressor
  invariants the future `_v3plus_features` model also inherits.
- Investigation: 19 call sites in 12 files; `gh search prs --merged
  "fr_regressor_v3"` returns one hit (#428); registry row has
  `smoke: false` (production checkpoint).
- Source: agent reports `abd6ed552ac8cae60` and `abda108c8263491da`
  surfaced the namespace collision; user direction (paraphrased) — pick
  the alternative with the smallest blast radius and reserve the future
  slot in docs, not in a stub registry row.

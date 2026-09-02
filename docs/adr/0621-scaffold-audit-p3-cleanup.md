<!-- markdownlint-disable MD013 MD060 -->
# ADR-0621: Scaffold Audit P3 — six cleanup items + state drift

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: hygiene, ai, python, test, docs, ci, fork-local

## Context

The scaffold / stub / dead-code audit of 2026-05-19 (`docs/research/scaffold-audit-2026-05-19.md`)
surfaced six P3 (cleanup / cosmetic) items and two state-drift gaps that were safe to bundle
into a single housekeeping PR without touching any functional path:

1. **P3-1** — `scripts/dev/permutation_importance.py:22` had a hardcoded
   `/home/kilian/dev/vmaf` path that fails on any other machine.
2. **P3-2** — Roughly 13 scripts under `ai/scripts/*.py` still defaulted corpus roots to
   `.workingdir2/<corpus>/`; data migrated to `.corpus/` in PR #983 (ADR-0547) but the
   default paths were never updated.
3. **P3-3** — Four `@unittest.skip` decorators carried vague or incorrect rationales
   (wrong ADR citation, "numerical value has changed" without explanation).
4. **P3-4** — Several non-bootstrap tests asserted VMAF scores at `places=1` without an
   inline comment explaining why 1-decimal precision is correct for those paths.
5. **P3-5** — Six registry entries with `smoke: true` (CI-only test fixtures) and one
   `smoke: false` entry (`lpips_sq_v1`) lacked a model card or had a card-name mismatch.
   No ADR-0042 bar applies to smoke fixtures; the lpips_sq_v1 mismatch is documented.
6. **P3-6** — Three upstream cJSON `TODO`/`FIXME` markers in
   `core/src/mcp/3rdparty/cJSON/cJSON.c` were firing the semgrep TODO-hunter rule.
   Fixing them would diverge from upstream cJSON; the correct action is to exclude the file.

State drift gaps:

- **SD-1** — `T-VULKAN-MOTION-LAVAPIPE-INIT` was referenced in two CI YAML comments and
  two `continue-on-error: true` steps but had no Open-bugs row in `docs/state.md`.
- **SD-2** — `T-PYTHON-PERMUTATION-IMPORTANCE-HARDCODED-PATH` was in the Open-bugs table
  but is resolved by this PR; moved to Recently closed.

## Decision

1. Replace the hardcoded `REPO` path in `permutation_importance.py` with
   `Path(__file__).resolve().parents[2]`.
2. Update all `.workingdir2/<corpus>/` fallback defaults in `ai/scripts/*.py` to
   `.corpus/<corpus>/` using `Path(__file__).resolve().parents[2]` anchoring.
   Env-var overrides (added by ADR-0547) are preserved unchanged.
3. Rewrite each stale `@unittest.skip` rationale to precisely name the root cause:
   XML attribute ordering (result_test), numpy RNG API drift (routine_test),
   deprecated `vmaf_feature` binary (feature_extractor_test), and missing
   `fps`/`format` filter-key additions (asset_test). Correct the false ADR-0326 citation.
4. Add inline justification comments at each `places=1` assertion explaining that the
   score passes through libsvm's int→float32 SVM head (deterministic but lossy at 1 dp)
   or through bootstrap resampling that accumulates rounding across seeds.
5. Add a "CI-only smoke fixtures" section to `docs/ai/model-registry.md` listing the six
   `smoke: true` entries and explaining why they are exempt from ADR-0042. Document the
   `lpips_sq_v1` / `lpips_sq.md` name mismatch as a tracked cosmetic gap.
6. Add `core/src/mcp/3rdparty/cJSON/cJSON.c` to `.semgrepignore` with an explanatory
   comment, preserving upstream cJSON parity.
7. Add an Open-bugs row for `T-VULKAN-MOTION-LAVAPIPE-INIT` in `docs/state.md` with a
   stated closure condition (promote both advisory CI steps to required once the kernel
   init error on lavapipe is resolved).
8. Move `T-PYTHON-PERMUTATION-IMPORTANCE-HARDCODED-PATH` from Open to Recently closed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Fix all items in one bundle (this decision)** | Single PR overhead; items are trivially independent. | Larger diff. | Chosen — all items are non-functional cleanups; batching is safe. |
| **Separate PR per item** | Smaller, easier to bisect. | Seven PRs for one-liner changes; PR overhead dominates. | Overhead outweighs the benefit for audit-cleanup items. |
| **Modify cJSON to fix upstream TODOs** | Silences the lint rule without ignoring. | Diverges from upstream cJSON 1.7.x; breaks the next vendor bump. | Not chosen — silencing is the correct policy for vendored upstream code. |
| **Delete stale @skip tests instead of rewriting rationales** | Removes dead weight. | Loses the intention signal; the tests document real gaps (deprecated binary, unimplemented filter keys). | Kept with precise rationale so the gap is visible; deletion is a follow-up decision. |

## Consequences

- **Positive**: `permutation_importance.py` works on any checkout; AI scripts default to
  the correct `.corpus/` layout matching ADR-0547; skip rationales now accurately describe
  actionable gaps; `places=1` assertions are self-documenting; CI smoke-fixture exempt
  status is documented; semgrep is clean without diverging from upstream cJSON;
  `docs/state.md` correctly reflects the Vulkan motion lavapipe gap.
- **Negative**: None; all changes are non-functional.
- **Neutral / follow-ups**: The `fps_cmd` / `format_cmd` / `get_filter_cmd` gap
  (adding `fps` and `format` to `Asset.ORDERED_FILTER_LIST`) remains open for a future
  PR. The `lpips_sq_v1` card-name mismatch is documented but not renamed here (deferred
  to a dedicated model-card cleanup PR). The `T-VULKAN-MOTION-LAVAPIPE-INIT` kernel fix
  is a separate engineering task.

## References

- `docs/research/scaffold-audit-2026-05-19.md` — source audit document.
- ADR-0547 — `VMAF_<NAME>_DIR` env-var overrides (precursor to P3-2).
- ADR-0042 — tiny-AI docs-required-per-PR (smoke fixtures are exempt).
- ADR-0165 — `docs/state.md` bug-tracking rule (SD-1, SD-2 updates).
- PR #983 — `.corpus/` data migration.
- Source: per user direction (scaffold-audit-p3-cleanup task brief, 2026-05-19).

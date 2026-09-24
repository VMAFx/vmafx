# Research-2086: BUG-048 strict JSON emitter restoration

- **Status**: Complete
- **Workstream**: BUG-048 A10 silent-revert closure
- **Last updated**: 2026-09-24

## Question

Did the strict JSON contracts documented by Research-0721, Research-0724, and
Research-0726 survive on current `master`, and what is the smallest restoration
that also preserves newer atomic-write and package contracts?

## Sources

- Producer commits `d8eaf643c`, `cce8274bc`, and `48a7c3e1d` for the AI
  manifest, stdout, and legacy-writer family.
- Producer commit `f04bf0e78` for the `vmaf-tune` artifact family.
- Clobber commits `384d97d03` (repository-layout squash) and `fedce9889`
  (cache-metadata strict-JSON change based on an older tune tree).
- [Research-0721](0721-ai-run-manifest-strict-json.md),
  [Research-0724](0724-ai-json-stdout-helper.md), and
  [Research-0726](0726-ai-legacy-json-writer-helper.md).
- Current assigned base `4e6916d16ac57647105d14a47a6680117d6b5738`.

## Findings

The producer and clobber commits are all ancestors of the assigned base, so
this is a silent overwrite rather than an unmerged branch. The AI producer's
`run_manifest.py` blob was `939303aad4112a5ab24fd48b937cb416a3158b4e`;
`384d97d03` replaced it with
`c138b8d7b11df35ad23daefcc93f9306b62f4c1e`, removing recursive non-finite
normalization, `dumps_manifest_json()`, list-root support, and
`allow_nan=False`. The pre-fix base later carried blob
`776c38c5f2beacfbeae0f203269c6b9e78a6646f`: atomic writing had landed, but it
atomically persisted the same invalid `NaN` / `Infinity` tokens.

The squash also left legacy AI artifacts on local serializers: evaluation
reports, Netflix/KoNViD/BVI per-clip caches, full-feature caches, the YouTube
UGC content manifest, and K150K summary stdout. The restoration routes those
boundaries through the shared strict helpers while retaining the newer atomic
write layer. The model registry keeps its later independently strict serializer
because its formatting contract differs and was not clobbered.

For `vmaf-tune`, `f04bf0e78` routed conformal calibration, auto-plan, and
ladder JSON through `jsonio.dumps_strict()`. Commit `fedce9889` directly
replaced those calls with bare `json.dumps()` and removed the corresponding
checks; `384d97d03` preserved those clobbered blobs. Current code retained a
correct and tested `dumps_strict()` helper, so only the emitter routing was
lost.

A five-test red-cap injected `NaN`, positive infinity, and negative infinity at
the AI string/file boundary and each affected tune emitter. On the assigned
base all five failed for the exact defect: strict parsing encountered a bare
`NaN`, and the AI string helper was absent. Restoring the central helper and
the three tune imports made the same five tests pass without changing any
in-memory calculation.

## Alternatives explored

| Alternative | Benefit | Cost / risk | Decision |
| --- | --- | --- | --- |
| Cherry-pick the four producer commits | Reuses historical patches verbatim | Overwrites newer atomic writes, Pydantic registry behavior, licenses, and later module evolution | Rejected |
| Add only `allow_nan=False` | Small diff | Raises on legitimate non-finite diagnostics instead of preserving the documented `null` representation | Rejected |
| Sanitize independently in every caller | Local ownership | Duplicates recursive policy and allows surfaces to drift again | Rejected |
| Restore shared strict emitters and route affected callers through them | One fail-closed policy while preserving newer atomic writes | Requires explicit caller tests and rebase invariant | Chosen |

No new ADR is needed: this restores behavior already decided and documented by
the 0721/0724/0726 research chain and the existing `vmaf-tune` portability
contract; it introduces no new architecture or scope decision.

## Verification

The red/green cap is:

```text
PYTHONPATH=ai/src:tools/vmaf-tune/src .venv/bin/python -m pytest -q \
  ai/tests/test_run_manifest.py::test_write_manifest_json_replaces_all_nonfinite_values \
  ai/tests/test_run_manifest.py::test_dumps_manifest_json_replaces_all_nonfinite_values \
  tools/vmaf-tune/tests/test_conformal.py::test_split_conformal_sidecar_replaces_all_nonfinite_values \
  tools/vmaf-tune/tests/test_auto_confidence_aware.py::test_emit_plan_json_replaces_all_nonfinite_values \
  tools/vmaf-tune/tests/test_ladder.py::test_emit_json_replaces_all_nonfinite_quality_values
```

Before the fix: 5 failed. After the fix: 5 passed.

The affected report/cache/stdout and tune regression selection passes 175
tests. `make verify-all` also passes with all 34 touched files HISS-clean; the
cleanup removed two unbounded read loops and split inherited overlong CLI/test
functions without changing their arguments or behavior.

## Open questions

None for A10. The wider BUG-048 train remains independently serialized by the
parent integration workflow.

## Related

- BUG-048 A10
- `changelog.d/fixed/bug048-strict-json-ai-tune.md`
- `docs/rebase-notes.md` section “BUG-048 A10 strict JSON emitters”

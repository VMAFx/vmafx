<!-- markdownlint-disable MD060 -->
# ADR-0109: Nightly bisect-model-quality runs against a synthetic placeholder cache

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, ci, tiny-ai, framework

## Context

Issue [#4](https://github.com/VMAFx/vmafx/issues/4) requested a nightly
CI wrap of the existing `bisect-model-quality` tool (`vmaf-train
bisect-model-quality`, landed in commit `4a6b76eb`). The issue itself
flagged three prerequisites:

1. A reproducible **golden feature cache** (parquet of feature vectors +
   DMOS targets) drawn from a frozen subset of NFLX-public / LIVE /
   KonIQ.
2. A canonical **model timeline ordering** so the bisect monotonicity
   contract holds.
3. A **sticky tracking issue** + comment edit-in-place flow so nightly
   verdicts do not become a wall of duplicates.

The first two are blocked: the fork does not yet ship the source
datasets in tree, has no DMOS labels for the YUV fixtures we *do* ship,
and the model registry currently lists three artifacts (two smoke
probes + LPIPS-SqueezeNet) rather than a quality timeline. Waiting for
all three to materialise before wiring any nightly job means the bisect
tool stays unused and the AC stays open indefinitely.

## Decision

Ship the wiring now against a deterministic synthetic placeholder
cache, on the explicit understanding that the real DMOS-aligned cache
swaps in via a follow-up. Concretely:

- Commit `ai/testdata/bisect/` (features parquet + 8 linear FR ONNX
  models, all "good") generated reproducibly from
  `ai/scripts/build_bisect_cache.py` with fixed seeds.
- CI re-runs the generator with `--check` and asserts byte-equality
  against the committed tree before exercising the bisect, catching
  drift in pandas / pyarrow / onnx serialisation.
- Add `.github/workflows/nightly-bisect.yml` (cron `37 4 * * *`,
  separate file as the issue text requested) running
  `vmaf-train bisect-model-quality --fail-on-first-bad`.
- Always upload the JSON report as a workflow artifact.
- Always edit a single sticky comment on tracking issue #40 with the
  rendered table — even on a green run — so nightly health is one
  click from the issue.
- A red workflow (`first_bad_index is not None`) is the alert; the
  sticky comment is the audit log.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Synthetic-timeline placeholder cache (chosen) | Ships now; deterministic; tiny commit (~16 KB); easy swap path | "No regression" verdict by construction until real models replace it | Best fit: unblocks the wiring without faking real signal |
| Wait for full DMOS-aligned cache + real timeline | True quality regression detection | Open-ended blocker; depends on dataset licensing + label collection + frozen libvmaf build; AC stays open for months | Not viable in current quarter |
| Workflow scaffold with no cache, runs `--help` only | Trivial PR | Documents intent only, proves nothing, leaves cron slot occupied with a no-op | Lowest signal; punts the work |
| Sticky issue comment as the only alert | One surface; matches issue wording | Easy to miss; no failed-check signal in PR/branch view | Failed status check is a firmer alert mechanism |
| Both: fail-job AND sticky comment (chosen for alert) | Fail-job is the urgent alert; sticky comment is the historical audit trail | Two surfaces to keep in sync | Picked: each surface serves a distinct purpose |
| Run inside `nightly.yml` as a new job | Slightly less file sprawl | Couples timeout / concurrency to TSan & friends; deviates from the issue's wording | Issue text said `nightly-bisect.yml`; honoured |

## Consequences

- **Positive**:
  - Issue #4 closes; the nightly slot now exercises onnx + onnxruntime
    ndas + pyarrow + the bisect algorithm against a reproducible
    fixture, surfacing toolchain breakage within 24 h.
  - Sticky comment on issue #40 gives at-a-glance health visibility.
  - Drift check guards against silent serialiser changes between Python
    minors that would otherwise stay invisible until a real-cache swap.
- **Negative**:
  - Until the real cache lands, a green workflow does not prove
    *quality* — only that the wiring works. This is documented in
    [`ai/testdata/bisect/README.md`](../../ai/testdata/bisect/README.md)
    and [`docs/ai/bisect-model-quality.md`](../ai/bisect-model-quality.md)
    so the limitation is not silently inherited.
  - Rebases that change `pandas` or `pyarrow` versions can break the
    drift check; the fix is regenerate + commit (see the README).
- **Neutral / follow-ups**:
  - Follow-up issue (`tiny-ai`, `ci`): replace the synthetic cache with
    a real DMOS-aligned subset once the dataset + label situation
    stabilises. The workflow file + sticky-comment helper stay
    unchanged at swap time.
  - Tracking issue #40 is sticky — never close while the workflow
    runs.

## References

- Issue [#4](https://github.com/VMAFx/vmafx/issues/4) — feature
  request that drove this PR.
- [`ai/src/vmaf_train/bisect_model_quality.py`](../../ai/src/vmaf_train/bisect_model_quality.py)
  [`ai/tests/test_bisect_model_quality.py`](../../ai/tests/test_bisect_model_quality.py)
  — the underlying tool + synthetic-regression unit test that satisfies
  the issue's "deliberately bad ONNX trips the alert" AC.
- [Research-0001](../research/0001-bisect-model-quality-cache.md) —
  alternatives explored for the cache shape; swap-path notes.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md),
  [ADR-0100](0100-project-wide-doc-substance-rule.md) — doc-substance
  rules satisfied by [`docs/ai/bisect-model-quality.md`](../ai/bisect-model-quality.md).
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive
  deliverables rule that this PR also satisfies (digest, ADR,
  reproducer, fork-changelog, rebase-notes, AGENTS invariant note).
- Source: per user direction in popup answer, scope = "Full cache +
  workflow + sticky issue", alert = "Fail the job (red CI)", workflow
  location = "Separate `.github/workflows/nightly-bisect.yml`".

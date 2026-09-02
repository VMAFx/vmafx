<!-- markdownlint-disable MD060 -->
# ADR-0749: Sunset VmafLegacyQualityRunner (float-path runner)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `python`, `quality-runner`, `breaking-change`, `cleanup`

## Context

`VmafLegacyQualityRunner` is a Python quality runner that drives the
`VmafFeatureExtractor` float-path via the `vmafexec` binary, collecting
the four legacy SVM features (vif, adm, ansnr, motion) and scoring them
with a libsvm model (`model_V8a.model`).

PR #38 removed the `float_ansnr` feature extractor from the C backend — it
was a pre-VMAF metric that Netflix never adopted in production. After that
removal, `VmafFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT` maps the
`"ansnr"` feature to the key `"float_ansnr"`, which no longer appears in
vmafexec output. Any invocation of `VmafLegacyQualityRunner` against the
current binary silently returns no ansnr scores or raises a `KeyError` during
the SVM scoring step.

CI infrastructure agent T-LEGACY-RUNNER-ANSNR-BROKEN (PR #86) surfaced this
as a required-status-check failure. The user authorized formal sunset of the
runner class and all tests that exclusively exercise it.

`VmafIntegerFeatureExtractor` (which inherits from `VmafFeatureExtractor` but
uses the integer-path vmafexec keys) is unaffected; the canonical Netflix
golden test `test_run_vmaf_runner` uses `VmafQualityRunner` and is untouched.

## Decision

Remove `VmafLegacyQualityRunner` from `compat/python-vmaf/core/quality_runner.py`
and delete all Python tests that exclusively exercise it. `VmafFeatureExtractor`
(the Python class) is retained because `VmafIntegerFeatureExtractor` and
several other non-legacy quality runners depend on it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Restore `float_ansnr` C implementation | Runner would work again | Reinstates a pre-VMAF metric Netflix never shipped; contradicts PR #38 rationale | Rejected — the feature was dropped deliberately |
| Keep runner, mark deprecated, skip ansnr | Runner callable without crashing | Ansnr column silently absent; score math is wrong (SVM model expects 4 features); deceptive API | Rejected — broken API is worse than no API |
| Sunset now (chosen) | Removes broken surface; unblocks CI | BREAKING change for any caller using the legacy Python runner | Accepted — callers should use `VmafQualityRunner` + a modern `.json` model |

## Consequences

- **Positive**: CI gate T-LEGACY-RUNNER-ANSNR-BROKEN resolved. Test suite
  no longer has an unconditionally-failing test class that blocks the merge
  train. The codebase no longer exports a broken public API.
- **Negative**: Any Python code calling `VmafLegacyQualityRunner` will
  receive an `ImportError` on the next install. Migration path: use
  `VmafQualityRunner` with `vmaf_float_v0.6.1.pkl` or a current `.json` model.
- **Neutral / follow-ups**:
  - `VmafFeatureExtractor.ATOM_FEATURES` still lists `"ansnr"` (mapped to
    the absent `float_ansnr` key). This is a latent hazard for any direct
    caller. A follow-up PR should remove `ansnr` from `ATOM_FEATURES` per
    the roadmap in `docs/research/0733-feature-importance-audit-2026-05-28.md`
    Phase 2.
  - `feature_extractor_test.py` contains many tests using `VmafFeatureExtractor`
    directly, some asserting `ansnr` scores via the float path. Those tests
    may also be broken; they are out of scope for this PR (they reference
    features beyond just `ansnr` and require broader assessment).

## References

- PR #38: drop legacy `ansnr` feature extractor from C backend
- PR #86: CI infrastructure agent flagged T-LEGACY-RUNNER-ANSNR-BROKEN
- `docs/research/0733-feature-importance-audit-2026-05-28.md` — Phase 2
  roadmap (remove `ansnr` from `VmafFeatureExtractor.ATOM_FEATURES`)
- req: "Sunset the legacy `VmafFeatureExtractor` (float-path) runner.
  PR #86's CI infra agent flagged this as T-LEGACY-RUNNER-ANSNR-BROKEN —
  the legacy runner still calls `float_ansnr` which was dropped by PR #38.
  User authorized formal sunset."

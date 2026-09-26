# Research-0480: Bootstrap Name Owner Restoration

## Question

Did ADR-0480's shared bootstrap score-name owner survive the repository-layout
refactor, and what is the narrowest regression gate that can keep it intact?

## Evidence

- Historical commit `442e3aab2` introduced `core/src/bootstrap_names.h` and
  converted both `vmaf_score_pooled_model_collection()` and
  `bootstrap_append_named_scores()` to its four suffix symbols and sizing macro.
- The current tree still contained the header, ADR, changelog fragment, and
  generated documentation, but both consumers had returned to four local
  literals plus a duplicated buffer-size expression.
- Existing model-collection tests exercise the published names and numerical
  scores. They cannot detect this structural regression because two copied
  implementations emit identical output until one copy changes.
- The generic deduplication scan also reported a clean tree while this small
  literal family was duplicated, so it is not a sufficient regression seam.

## Result

Restore both consumers to `bootstrap_names.h` without changing either loop or
floating-point work. Add a fast source-contract test that requires both includes
and all five shared symbols, and rejects the four literal suffixes in either
consumer. This directly pins ADR-0480's ownership boundary while the existing C
tests continue to pin runtime behavior.

No benchmark or retraining run is part of this correctness restoration.

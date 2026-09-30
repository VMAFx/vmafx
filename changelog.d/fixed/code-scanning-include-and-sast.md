- **CodeQL include-non-header alert #1309 resolved with internal test accessors and CI guard.**
  `core/test/test_feature_backend_twin.c` linked directly against `libvmaf` instead
  of unity-including `core/src/libvmaf.c`. Narrow internal accessors
  (`vmaf_backend_twin_verdict_for_test`, `vmaf_context_fake_backend_for_test`,
  `vmaf_context_set_gpumask_for_test`, `vmaf_context_append_registered_feature_extractor_for_test`,
  and `vmaf_context_resolve_context_fallbacks_for_test`) are declared in
  `core/src/libvmaf_priv.h` with static definitions in `core/src/libvmaf.c`. A new
  `scripts/ci/check-no-non-header-includes.sh` check runs in pre-commit and CI to
  prevent non-header source file inclusions under `core/test/`.
- **Scorecard SAST alert #6 resolved by running CodeQL Actions universally on every PR.**
  Scorecard's `sastToolInCheckRuns` evaluates PR head commits across the last 30 commits
  on master. Under [ADR-1389](docs/adr/1389-codeql-actions-universal-pr-sast.md),
  `CodeQL (Actions)` now runs unconditionally on all pull requests and pushes,
  providing 100% commit SAST coverage across docs-only and non-code PRs with
  negligible (~15–20s) overhead, and is enforced in the required checks aggregator.

- **`docs/state.md` drift sync 2026-05-30.** Moved 2 stale Open bug rows
  to Recently closed after verifying they were actually fixed on master:
  (1) **T-LEGACY-RUNNER-ANSNR-BROKEN** — `AnsnrFeatureExtractor` deleted
  in PR #283 + ADR-0749 legacy-runner sunset; (2)
  **T-LEGACY-RUNNER-STUB-MISSING-2026-05-29** — `VmafLegacyQualityRunner`
  imports removed from `python/test/quality_runner_test.py` via the
  ADR-0749 sunset. Both verified by grep against `origin/master`.

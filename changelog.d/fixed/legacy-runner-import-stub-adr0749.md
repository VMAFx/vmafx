- `from vmaf.core.quality_runner import VmafLegacyQualityRunner` no longer
  raises `ImportError`. A `NotImplementedError`-raising stub class is now
  exported from `compat/python-vmaf/core/quality_runner.py`; any attempt to
  instantiate it raises `NotImplementedError` with a migration pointer to
  `VmafQualityRunner`. The underlying runner was sunset in ADR-0749 / PR #87;
  this stub closes the T-LEGACY-RUNNER-STUB-MISSING-2026-05-29 import-failure
  gap that PR #213 surfaced but did not merge.

- **Build dependencies**: the Python package build now requires
  `setuptools>=83.0.0` and `wheel>=0.46.2` (previously `>=77.0.1` and
  unbounded), and the documentation requirements use the same floors. This
  excludes releases affected by PYSEC-2025-49, PYSEC-2026-3447 (setuptools)
  and PYSEC-2026-2047 (wheel). The hash-locked installs already used
  setuptools 84.0.0 and wheel 0.48.0, so locked builds are unchanged.

## dev image: rr and ClangBuildAnalyzer (2026-10-16)

- `dev/Containerfile` installs `rr` (Ubuntu 26.04 package `5.9.0-5`) in the `build-deps` stage via `rr=${RR_APT_VERSION}` for reverse debugging and deterministic execution recording (ADR-3336).
- `dev/Containerfile` compiles and installs `ClangBuildAnalyzer` v1.6.0 (Unlicense) in the `build-deps` stage after verifying its release tarball against `CLANGBUILDANALYZER_SHA256`.
- `dev/Containerfile` owns `RR_APT_VERSION`, `CLANGBUILDANALYZER_VERSION`, and `CLANGBUILDANALYZER_SHA256` as single-source `ARG`s.
- `scripts/ci/check_ort_tarball_pins.py` statically enforces presence of pins in `dev/Containerfile`, 64 lowercase hex digest validity, and `sha256sum` verification prior to building ClangBuildAnalyzer (ADR-3336, generalizing ADR-3334 per HISS-19).

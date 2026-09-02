# Fixed: CI required-check failures blocking merge train (round 3)

Resolved 8 pre-existing required-aggregator failures that blocked every PR post-#60:

- **Windows MSVC CUDA + SYCL build**: stale `libvmaf\build` path (ADR-0700 rename
  leftover) corrected to `core\build` in `libvmaf-build-matrix.yml`.
- **Ubuntu HIP smoke test**: removed `test_float_ansnr_hip_extractor_registered`
  (the `float_ansnr_hip` extractor was dropped in PR #38 / ADR-0720; the smoke
  test was not updated).
- **Netflix CPU Golden Tests (D24)**: `VmafIntegerFeatureExtractor` was requesting
  the removed `float_ansnr` feature from the CLI, causing exit-255 before any score
  was computed. Removed from the integer-path feature list; the CI gate tests
  (`test_run_vmaf_runner`, `test_run_vmaf_runner_checkerboard`) already expected
  ansnr to be absent (assertRaises KeyError).
- **CodeQL (Python)**: `codeql-config.yml` paths updated from `libvmaf/` to `core/`
  (ADR-0700 rename); added explicit no-op build step to prevent C++ autobuild from
  running for the Python-only job.
- **Gitleaks**: added `go.sum`, `Cargo.lock`, and generated protobuf stubs to
  `.gitleaks.toml` allowlist; package-manager integrity hashes are not credentials.
- **Semgrep**: updated `.semgrepignore` to include `compat/python-vmaf/matlab/` and
  `compat/python-vmaf/resource/` (ADR-0700 rename; old `python/vmaf/matlab/`
  exclusion no longer matched). Verified locally: 0 findings.
- **Tiny AI**: implemented missing `dumps_jsonl_row` (`aiutils.jsonl_utils`) and
  `dumps_registry_json` / `write_registry_json` (`vmaf_train.registry`) functions
  that tests imported but that were never implemented.

Research digest: `docs/research/0735-ci-required-failures-round-3-2026-05-28.md`

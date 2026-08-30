- Fixed master CI regressions in the DNN coverage gate, MCP smoke test, and
  formatting checks by adding targeted DNN regression coverage, aligning the
  tiny-model CLI smoke with the documented resize requirement, and removing a
  stale MCP unknown-tool assertion.
- Added fail-fast timeouts to the Netflix golden and lavapipe parity CI gates
  so stuck subprocesses fail with diagnostics instead of burning the full job
  budget.
- Raised the Netflix normal-pair golden timeout to the cold-runner budget it
  actually needs while preserving the split normal/checkerboard D24 gate; a
  GitHub-hosted Ubuntu run later timed out exactly at the previous 11-minute
  wrapper before any assertion output, so the normal-pair wrapper now leaves a
  21-minute diagnostic budget.
- Raised the Vulkan VIF lavapipe cross-backend timeout to a cold-runner budget
  after GitHub-hosted Ubuntu timed out exactly at the old 8-minute wrapper
  without producing a numerical mismatch.
- Narrowed the Netflix golden CI gate to the D24 golden pair tests instead of
  running unrelated Python quality and feature suites in that lane.
- Fixed the macOS writer-test SIGSEGV path by making the POSIX thread-local
  locale helper derive its C numeric locale from a duplicated global locale
  instead of allocating a fresh all-category locale with a NULL base.
- Fixed the remaining macOS writer-test SIGSEGV path by removing the
  `test_output.c` duplicate implementation-TU include pattern and routing its
  collector access through a small internal test accessor.
- Hardened DNN sidecar loading so oversized sidecars are rejected via metadata
  before entering the stdio read path, and made the regression test use the
  committed smoke ONNX fixture so a missed sidecar gate fails cleanly instead
  of falling through to invalid-model ORT behavior.
- Flushed output writer streams before restoring/freeing the temporary C
  numeric locale, keeping macOS `fdopen()` path-dispatch flush/close behavior
  outside the freed locale lifetime.
- Routed public ABI coverage tests through the shared libvmaf target when
  `default_library=both`, left the private writer test on the static target
  with LTO disabled only on Darwin, and added a macOS-only crash backtrace
  hook to the C test harness for future hosted-runner SIGSEGV diagnostics.
- Implemented the Python `Asset` `fps_cmd` / `format_cmd` filter-key gap and
  unskipped the existing regression tests that were documenting the missing
  surface.
- Cleaned high-volume CI compiler warnings across the libvmaf build matrix,
  including backend-conditional `VmafFeatureExtractor` LTO type mismatches,
  stale unused helper code, duplicate inline specifiers, and unchecked ONNX
  Runtime status returns. Follow-up cleanup also removes Clang strict-prototype
  test noise and ARM/no-asm helper warnings that were still visible after the
  first warning pass.
- Fixed a vmaf CLI teardown hang where EOF/read-error paths leaked
  preallocated picture-pool slots after output had already been written,
  causing Netflix golden and lavapipe parity CI lanes to hit their diagnostic
  timeout wrappers instead of exiting cleanly.
- Fixed the GPU parity CI extractor-name resolver after the Vulkan integer ADM
  canonical rename so the `adm` matrix row invokes `integer_adm_vulkan` instead
  of the retired `adm_vulkan` compatibility name.
- Fixed the AVX-512 float convolution vertical pass to use unaligned memory ops
  on buffers that only meet the project-wide 32-byte alignment contract,
  avoiding `float_vif` SIGSEGVs on AVX-512 CPU runners.
- Fixed Python `run_test_on_dataset()` stats collection so bootstrap confidence
  fields are requested only from bootstrap-capable runners; normal VMAF and
  PSNR runners now use the standard regressor stats path instead of failing the
  macOS tox lane with `get_bagging_score_key` lookup errors.
- Stabilized Python doctests for NumPy 2 scalar repr and Python 3.14 assertion
  detail output so macOS tox no longer fails examples in `vmaf.tools.misc` and
  `vmaf.tools.stats` while Linux hides the same portability issue.

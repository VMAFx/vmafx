<!-- markdownlint-disable MD013 MD036 MD060 -->
# Research: Pre-existing macOS / Python / Tiny-AI CI Failures (2026-06-01)

**Context:** Master tip `40d192ef1` had 20+ failing required checks, all requiring admin bypass to merge. This document investigates the root causes of three pre-existing failures blocking every PR from merging via the Required Checks Aggregator.

## Failure 1 — `test_metal_kernel_coverage_audit` (macOS all configs)

**Job:** `Build — macOS clang (CPU)`, `Build — macOS clang (CPU) + DNN`, `Build — macOS Metal (T8-1 scaffold)`

**Error:** `fail, every Metal kernel basename must have a registered <basename>_metal extractor`

**Root cause:** The test introduced in `T-METAL-KERNEL-PARITY-ROUND4-2026-05-31` (PR test/metal-kernel-coverage-round4) lists `g_metal_kernel_basenames[]` using the `.mm` file stems rather than the registered extractor name prefixes. Specifically:

- The `.mm` file is `integer_motion_v2_metal.mm`
- The basename extracted by the test: `"integer_motion_v2"` → expected extractor: `"integer_motion_v2_metal"`
- The actual registered extractor name: `"motion_v2_metal"` (short alias, documented in ADR-0421 / T8-1c)

The `motion_v2_metal` short-alias was chosen intentionally in the original T8-1 scaffold to keep the user-facing name consistent with the CPU twin `"motion_v2"`. The dispatch strategy (`core/src/metal/dispatch_strategy.c`) and the existing registration smoke test (`test_metal_kernel_registration.c`) both use `"motion_v2_metal"` correctly. Only the new round-4 coverage audit used the incorrect filename-derived name.

**Fix:** Change `"integer_motion_v2"` to `"motion_v2"` in `g_metal_kernel_basenames[]` and add a clarifying comment explaining that entries are registered extractor name prefixes, not `.mm` file stems.

## Failure 2 — Tiny AI pytest suite (`ai/tests/`)

**Job:** `Tiny AI (DNN Suite + ai/ Pytests)` — 9 failed, 954 passed

Three distinct root causes:

### 2a — `test_data_datasets_branches.py` (3 failures)

**Error:** `pydantic_core._pydantic_core.ValidationError: sha256 must be a 64-char hex digest`

The `ManifestEntry._sha256_shape` validator (added in PR #506 Python-surfaces bug audit) enforces that `sha256` must be exactly 64 lowercase hex characters. The test fixtures used abbreviated stubs (`"deadbeef"`, `"cafebabe"`, `"s"`) that pre-dated the validator.

**Fix:** Replace stubs with valid 64-char hex constants (`"deadbeef" * 8`, `"cafebabe" * 8`, `"0" * 64`).

### 2b — `test_frame_loader.py` (4 failures)

**Error:** `TypeError: _popen_factory.<locals>.fake_popen() got an unexpected keyword argument 'stderr'`

`iter_frames` was updated to pass `stderr=subprocess.PIPE` to the Popen call (to capture ffmpeg diagnostics on non-zero exit). The test's `fake_popen` stub only accepted `stdout:int` as a keyword argument.

Additionally, `iter_frames` now accesses `proc.stderr` in the cleanup path; the `_FakeProcess` stub had no `stderr` attribute.

**Fix:** Add `stderr: int | None = None` to `fake_popen`'s signature and add `self.stderr: io.BytesIO | None = None` to `_FakeProcess`.

### 2c — `test_parquet_utils.py` (1 failure)

**Error:** `Failed: DID NOT RAISE <class 'RuntimeError'>`

The test overrode `DataFrame.to_parquet()` to raise `RuntimeError`, but `write_parquet_atomic` was refactored to use `pyarrow` directly via `_write_v2()` — it never calls `df.to_parquet()`. The subclass override never fires.

**Fix:** Use `monkeypatch.setattr(aiutils.parquet_utils, "_write_v2", ...)` to inject the failure at the actual call site.

## Failure 3 — Go CI (`go vet + go test`)

**Error:** `ERROR: Neither source directory 'core/build-cpu' nor build directory None contain a build file meson.build.`

**Root cause:** `.github/workflows/go-ci.yml` calls `meson setup core/build-cpu ...` with only one positional argument. Meson interprets the single positional argument as the *source* directory, but `core/build-cpu/` has no `meson.build`. The correct two-argument form is `meson setup <srcdir> <builddir>`.

The correct form used everywhere else in the matrix (`build.yml` line 331, 341, 352) is `meson setup core core/build ...`.

**Fix:** Change `meson setup core/build-cpu` to `meson setup core core/build-cpu`.

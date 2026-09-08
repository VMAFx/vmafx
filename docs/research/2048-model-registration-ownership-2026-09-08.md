<!-- markdownlint-disable MD013 -->
# Research-2048: Feature registration option-copy ownership

## Confirmed defect

Base: `2143174c7bf933f97aec610f423c559a885b8e9a`.
`vmaf_use_features_from_model()` copies each model feature's options, then
returns directly when context creation rejects them. The constructor releases
its own allocations but does not consume the dictionary on failure. A real
model override with `motion_force_zero=invalid` returns `-EINVAL` and leaks the
private copy. The retained initial shared-library control reports 170 bytes in
four allocations. The analogous worker context-creation path has the same
source-level ownership gap.

`vmaf_dictionary_copy()` also returns the OR of per-entry insertion errors and
can leave a partially allocated destination. Its three registration callers
previously returned on that error without releasing the destination. The
explicit registration path already cleaned up context-create failures, but
still lacked the partial-copy cleanup.

## Correction and unchanged contracts

`fex_options_copy()` frees a partial destination on copy failure.
`fex_ctx_create_owned_options()` frees the private copied dictionary on context
creation failure, transferring it to the context only on success. All three
callers use the helpers. No dictionary implementation or public declaration
changes. `vmaf_use_feature()` preserves its existing early argument/name
rejection guards and supplied-dictionary consumption. Model and worker options
remain borrowed; successful contexts own their copies. Registration remains
nontransactional when an earlier feature succeeded before a later failure.

The API guide's shorthand about freeing dictionaries on `-EINVAL` incorrectly
included invalid options for a known extractor. It now names the precise early
rejections; the error code alone cannot identify whether ownership transferred.
No feature calculations, Netflix assertions, public signatures, FFmpeg patch
contract, backend policy or valid-output format changes. No alternatives: this
is cleanup of allocations already owned by the caller; no new ADR is needed.

## Regression design

- Public-only shared-library test: invalid option value and unknown option key
  each fail three times, followed by a separate valid-option transfer case.
  Repeated registration preserves the borrowed model dictionary.
- Linux shared-library partial-copy test: a one-shot ELF interposer creates a
  real one-entry destination from a source with at least two entries, then
  returns `-ENOMEM`. Unarmed calls forward through `RTLD_NEXT` to the real
  library implementation. Explicit and model registration propagate the error
  and then succeed on retry; leak detection checks the partial allocation.
- The interposer has no production hook and does not replace other engine
  implementations. The natural rejection test is a separate executable without
  private symbols, so an old-library control cannot accidentally use fixed
  implementation objects from its caller.
- Worker failure cleanup is covered by shared helper source review, not by an
  injected asynchronous worker failure. No worker retry or GPU runtime claim.

## Touched-file lint and retained integrations

Four DNN bridge exports have callers outside this CPU profile:
`vmaf_ctx_dnn_attach`, `vmaf_ctx_dnn_set_codec_context`,
`vmaf_ctx_dnn_is_codec_aware`, and `vmaf_ctx_dnn_set_resize_mode` are called in
`core/src/dnn/dnn_attach_api.c`. Two declared integration scaffolds remain:
`vmaf_ctx_dnn_has_session` in `core/src/dnn/dnn_ctx.h` and
`vmaf_register_metadata_handler` in `core/src/metadata.h`. The six precise
`unusedFunction` annotations retain those functions and existing signatures;
ordinary unused checks remain enabled elsewhere. The weak
`__libc_single_threaded` identifier is dictated by glibc's ABI and retains its
ADR-0141 exception with an inline citation. Read-only descriptor constness and
an unused initializer are cleaned without changing behavior.

## Validation

Component results and exact commands are retained under
`.workingdir2/evidence/2026-09-08-model-registration-ownership/`; disposable
builds are under `.workingdir2/cache/model-registration-cleanup-20260908/`.

- GCC 15.2 release CPU build with default LTO: all five cases pass.
- Separate ASan/UBSan debug build, LTO disabled for instrumentation and leak
  detection enabled: all five cases pass. Exact-base `libvmaf.c` was compiled
  and relinked into the same shared-library build for the negative control.
  Its natural rejection tests leak 1,023 bytes in 24 allocations; the two
  partial-copy controls leak 334 bytes in eight allocations. Both old-library
  controls exit 86; both fixed-library controls exit zero. Loader receipts
  identify each actual library, and full compile/link commands are retained.
- clang-tidy 22.1.8 reports zero warnings and zero uncited markers across all
  three touched native TUs. The scoped writer records both new tests and
  tightens the production uncited-marker count from one to zero.
- Cppcheck 2.21.1 uses all checks, exhaustive analysis and the official POSIX
  model on all 1,181 configured commands / 283 tracked sources. None of the
  touched native sources has a diagnostic. The complete profile still exits
  one for 327 style findings in other files and three missing-include notices,
  plus the informational checker summary. These are not a full lint pass.
This component receipt is not full-tree, golden-data, GPU or native Windows
acceptance. The Linux interposer target requires a shared library; the ordinary
rejection test also builds without ELF interposition.

## Reproduce

```sh
ninja -C build test/test_model_registration_ownership test/test_registration_partial_copy
meson test -C build --no-rebuild --print-errorlogs test_model_registration_ownership test_registration_partial_copy
```

Repeat in a separate CPU debug build with `-Db_sanitize=address,undefined
-Db_lto=false`, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. For a static or non-Linux build, run only
`test_model_registration_ownership`; the ELF-specific target is not registered.

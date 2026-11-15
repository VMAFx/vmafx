---
paths:
  - core/src/fex_ctx_vector.cpp
  - core/src/fex_ctx_vector.h
invariant: Feature context registration compares option keys for deduplication and manages option-copy lifecycle.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Feature context registration, deduplication, and option ownership

- **Feature-context deduplication includes parsed feature parameters**
  (ADR-0385; [Research-2047](../../docs/research/2047-option-aware-context-registration-2026-09-08.md)):
  [`src/fex_ctx_vector.cpp`](../src/fex_ctx_vector.cpp) compares
  `vmaf_feature_name_from_options()` keys for shared advertised feature bases.
  CPU/GPU twins with equivalent defaults or alias-spelled options still use
  first registration. Different feature parameters must coexist: matching
  only `provided_features[]` drops second model/explicit option set.
  When either list is absent, preserve extractor-name fallback with parsed
  options. Name-allocation failure returns `-ENOMEM` without consuming
  incoming context; growth failure preserves pointer table, count and
  capacity. Private capacity helper checks both `UINT_MAX` and `SIZE_MAX`
  before doubling, including release builds. Keep actual-motion public
  scoring test and Linux allocation-failure controls on rebase.
- **`vmaf_fex_ctx_pool_create` has three-label cleanup chain**
  (fork-local, ADR-1060, r10 audit): `fail` → `free_p` → `free_fex_list`
  in `src/feature/feature_extractor.cpp`. Adding more allocations between
  `malloc(fex_list_sz)` and `pthread_mutex_init` needs corresponding label
  and goto. Prior two-label chain (`free_p` / `fail`) leaked `fex_list`
  on mutex-init failure.
- **`get_fex_list_entry` slot init is all-or-nothing** (fork-local,
  ADR-1060, r10 audit): `pthread_cond_init`, `ctx_list` malloc, and
  `vmaf_dictionary_copy` are all checked; any failure destroys cond,
  frees `ctx_list` before returning NULL. `pool->cnt` is NOT incremented on
  failure so partial slot is effectively invisible but still zeroed.
  Any rebase adding new resources to slot init sequence must add
  matching cleanup on early-return path.

## Rebase-sensitive invariants (2026-06-04)

- **`vmaf_fex_integer_motion_v2` registration**: CPU extractor
  `vmaf_fex_integer_motion_v2` (from `feature/integer_motion_v2.c`) MUST
  appear in `feature_extractor_list[]` in `feature_extractor.cpp`. Removing
  it breaks `vmaf_get_feature_extractor_by_name("motion_v2")` on all CPU
  builds. Comment claiming this symbol was "merged into v1" in
  `feature_extractor.cpp` was incorrect, has been removed.

## Registration option-copy ownership (Research-2048)

In `src/libvmaf.c`, `fex_options_copy()` must release any partial destination
when `vmaf_dictionary_copy()` fails. Dictionary implementation can allocate
some entries before returning error. Every caller starts with independent
NULL destination: explicit registration, model registration and worker-context
creation. Preserve supplied-dictionary consumption guards of
`vmaf_use_feature()`; model and worker source dictionaries remain borrowed.
`fex_ctx_create_owned_options()` transfers only private copy on success,
releases it when context creation rejects options or fails allocation.

Keep public-only rejection regression separate from Linux ELF
partial-copy control: only latter intentionally interposes dictionary copy.
It forwards normal calls to actual shared library, injects real partial
destination, then checks error propagation and registration retry. Never link
private engine objects into these targets or invalidate old-library control.
Four DNN bridge exports retain out-of-profile consumers in
`src/dnn/dnn_attach_api.c`; `vmaf_ctx_dnn_has_session` and
`vmaf_register_metadata_handler` remain declared integration scaffolds in
`src/dnn/dnn_ctx.h` and `src/metadata.h`. Their six exact unused-function markers
preserve those interfaces without disabling ordinary unused checks. glibc
weak symbol retains its dictated ABI spelling. See
[Research-2048](../../docs/research/2048-model-registration-ownership-2026-09-08.md).

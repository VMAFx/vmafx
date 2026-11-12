---
paths:
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_extractor.h
invariant: feature_extractor_list[] is exactly-once and all extractors register in feature_extractor.cpp.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Feature Extractor Exactly-Once Registration

- **`feature_extractor_list[]` is exactly-once** (ADR-0544):
  static `feature_extractor_list[]` in `feature_extractor.c` must
  list every `&vmaf_fex_*` symbol **at most once** under its correct
  `#ifdef HAVE_*` guard. duplicate is silently masked by
  first-match `vmaf_get_feature_extractor_by_name()` but breaks
  ctx-pool's iterator dispatch: pool's `get_fex_list_entry()` keys
  on `fex->name` so same name registered twice still collapses to
  one pool entry there, but any caller that walks registry
  directly (e.g. iterator dispatch path that fans out
  `vmaf_use_features_from_model`) allocates one entry per registered
  pointer and runs `init`/`extract`/`flush` once per copy per picture.
  audit helper `vmaf_feature_extractor_list_audit()` runs from
  `vmaf_init()` and returns `-EINVAL` on duplicates;
  `test_feature_extractor_list_no_duplicates` C unit test exercises
  it on live registry. When adding new backend or extractor:
  add **one** `extern VmafFeatureExtractor vmaf_fex_*_<bk>` decl and
  **one** `&vmaf_fex_*_<bk>` entry inside matching `#if HAVE_<BK>`
  block — do not paste whole `&vmaf_fex_*` cluster.
- **Register extractors in `feature_extractor.cpp`, NOT
  `feature_extractor.c`** (ADR-0846 / ADR-1110): build compiles
  C++23 `feature_extractor.cpp` (see `core/src/meson.build`);
  old `feature_extractor.c` is **dead twin** left over from
  ADR-0846 conversion and is *not* in any build target. Editing only
  `.c` makes `vmaf_get_feature_extractor_by_name()` return NULL
  (symptom: `problem loading feature extractor: <name>` from
  CLI). When adding new extractor, put `extern` decl +
  `feature_extractor_list[]` entry in `.cpp`. (stale `.c`
  should be removed in separate cleanup.)

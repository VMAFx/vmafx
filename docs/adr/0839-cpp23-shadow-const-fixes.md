<!-- markdownlint-disable MD013 MD060 -->
# ADR-0839: C++23 wave — shadow-identifier and implicit-cast cleanup

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cpp23`, `lint`, `core`, `sycl`, `fork-local`

## Context

The C++23 wave (ADR-0708 and follow-on waves) converted several `.c` files to `.cpp`.
After conversion, three categories of C-allows-but-C++-warns issues remained unaddressed:

1. **Shadowed identifier** — `fex_ctx_vector.cpp`: local variable `capacity` at the
   realloc growth path shares a name with the `rfe->capacity` struct member. Clang's
   `-Wshadow` and clang-tidy `bugprone-shadow` flag this.
2. **Implicit C-style casts** — `feature_collector.cpp`: `(char*)`, `(void*)`,
   `(FeatureVector**)`, `(VmafFeatureCollector*)`, `(VmafPredictModel*)`, and the
   `(decltype(...))` pattern were carried forward from the C original. In C++ these are
   `reinterpret_cast`-equivalent and defeat the narrowing checks `static_cast` provides.
   `(VmafModelFlags)0` is a C-style enum cast where `static_cast<VmafModelFlags>(0)` is
   the idiomatic C++ form.
3. **Implicit C-style casts** — `core/src/sycl/common.cpp`: `(uint8_t*)`, `(size_t)`, and
   `(unsigned)` casts in the frame-upload and plane-copy paths.

None of these caused incorrect behaviour on existing platforms, but they produced
clang-tidy `cppcoreguidelines-pro-type-cstyle-cast` hits and blocked the `-Wshadow`
clean-compile goal for the C++23-wave files.

## Decision

Apply mechanical fixes to the three files:

- Rename `capacity` → `new_capacity` in `fex_ctx_vector.cpp` to eliminate the shadow.
- Replace every C-style cast with the appropriate `static_cast<>` in
  `feature_collector.cpp` and `common.cpp`.
- No behaviour change; the fixes are type-system-only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| NOLINT suppressions | Zero-diff to logic | Does not fix the underlying issue; NOLINT requires ADR citation per ADR-0278 | Against policy: refactor first |
| Leave as-is until a dedicated lint sweep | Minimal churn | ADR-0141 (touched-file cleanup rule) requires fixing warnings in files we touch | Violated project rule |

## Consequences

- **Positive**: three files now compile clean under `-Wshadow` and without
  `cppcoreguidelines-pro-type-cstyle-cast` hits; `static_cast<>` makes the intent of
  each conversion auditable.
- **Negative**: none — the changes are purely syntactic.
- **Neutral**: the `(decltype(...))` idiom in `feature_collector.cpp` is replaced with
  `static_cast<decltype(...)>(...)` which is slightly more verbose but standard C++.

## References

- ADR-0708 (C++23 pilot), ADR-0723 (fex_ctx_vector wave), ADR-0727 (dict wave),
  ADR-0729 (model/feature_name/picture_copy wave), ADR-0733 (output wave),
  ADR-0735 (cpu/ref/thread_locale/log wave).
- ADR-0141 (touched-file cleanup rule).
- ADR-0278 (NOLINT citation closeout).

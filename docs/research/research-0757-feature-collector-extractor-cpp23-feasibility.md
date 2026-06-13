# Research-0757 — C++23 Feasibility: feature\_collector + feature\_extractor

**Date:** 2026-05-29
**Branch:** research/cuda-13.3-impact-assessment-20260528

**Files audited:**

- `core/src/feature/feature_collector.c` (546 lines)
- `core/src/feature/feature_extractor.c` (948 lines)

---

## 1. Static functions accessed by tests

### feature\_collector.c

`core/test/test_feature_collector.c` includes the implementation source directly:

```c
#include "feature_collector.c"
```

It then calls these **static** functions by name — bypassing the public API:

| Static function           | Called from test      |
|---------------------------|-----------------------|
| `aggregate_vector_init`   | yes — line 130        |
| `aggregate_vector_append` | yes — lines 135–153   |
| `aggregate_vector_destroy`| yes — line 160        |
| `feature_vector_init`     | yes — line 169        |
| `feature_vector_append`   | yes — lines 174–183   |
| `feature_vector_destroy`  | yes — line 186        |

`find_feature_vector`, `feature_collector_grow_capacity`,
`feature_collector_ensure_vector`,
`feature_collector_dispatch_metadata`, and
`feature_collector_run_model_predict` are **not** called directly by any
test — they are exercised indirectly through the public API.

### feature\_extractor.c

`core/test/test_feature_extractor.c` does **not**
`#include "feature_extractor.c"`. It includes only the headers
(`feature_extractor.h`, `feature_collector.h`, `fex_ctx_vector.h`).
Static functions `get_fex_list_entry`, `ctx_pool_ensure_slot_ctx`,
`ctx_pool_claim_slot`, and `vmaf_fex_ctx_parse_options` are not accessed
directly by any test.

`test_feature_extractor_list_no_duplicates` calls
`vmaf_feature_extractor_list_audit()`, which is a regular (non-static)
public function — no concern there.

---

## 2. Atomics inventory (`feature_extractor.c`)

`feature_extractor.h` already has the C/C++ bridge:

```c
#if defined(__cplusplus) && defined(_MSC_VER)
#include <atomic>
using std::atomic_int;
#else
#include <stdatomic.h>
#endif
```

`feature_extractor.c` uses C11 `<stdatomic.h>` generics:

| Call site (line)        | Operation          | Field                                                           |
|-------------------------|--------------------|-----------------------------------------------------------------|
| 753                     | `atomic_init`      | `entry.capacity`                                                |
| 754                     | `atomic_init`      | `entry.in_use`                                                  |
| 813, 846, 882, 910, 933 | `atomic_load`      | `entry->capacity`, `entry->in_use`, `pool->fex_list[i].capacity`|
| 823                     | `atomic_fetch_add` | `entry->in_use`                                                 |
| 885                     | `atomic_fetch_sub` | `entry->in_use`                                                 |

The field type is `atomic_int` (declared in `feature_extractor.h`).
The header bridge means `atomic_int` already resolves to
`std::atomic_int` on MSVC C++ and to `_Atomic int` on gcc/clang.
Converting `feature_extractor.c` to `.cpp` would:

- On gcc/clang Linux: `<stdatomic.h>` C11 generics work in C++ mode as
  a GNU extension. No ABI change. The header bridge is already exercised
  by `fex_ctx_vector.cpp`.
- On MSVC (`icpx-cl`): the header already emits `std::atomic_int`; the
  `.cpp` TU would pick that path. ABI unchanged.

`feature_collector.c` has **zero** atomics — uses `pthread_mutex_t`
exclusively.

---

## 3. Comparison with already-converted files

| File                      | TU type | Atomics    | Test includes .c  | Guard pattern                                     |
|---------------------------|---------|------------|-------------------|---------------------------------------------------|
| `mem.cpp`                 | `.cpp`  | None       | No                | `extern "C"` in `mem.h`                           |
| `opt.cpp`                 | `.cpp`  | None       | No                | `extern "C"` in `opt.h`                           |
| `fex_ctx_vector.cpp`      | `.cpp`  | Via header | No                | `extern "C"` in `fex_ctx_vector.h` (ADR-0723)     |
| **`feature_collector.c`** | `.c`    | None       | **Yes — blocker** | No guards in header                               |
| **`feature_extractor.c`** | `.c`    | Yes (C11)  | No                | No guards in header                               |

The key difference from the three converted files:
`test_feature_collector.c` hard-includes `feature_collector.c` and calls
its static functions directly. This means:

1. Renaming `feature_collector.c` to `.cpp` makes the test fail to
   compile — a C test cannot `#include` a C++ TU (VLAs, compound
   literals, C++-reserved keywords, `static` linkage semantics differ).
2. Moving the 6 static functions into a `feature_collector_internal.h`
   (test-internal header pattern) would fix the test — but requires
   touching the test source.

`feature_extractor.c` has no test that includes it directly; it is a
straightforward conversion candidate.

---

## 4. Recommended migration path

### feature\_extractor.c — straightforward conversion

1. `git mv core/src/feature/feature_extractor.c
   core/src/feature/feature_extractor.cpp`
2. Add `extern "C"` guards to `feature_extractor.h` (matching the
   `fex_ctx_vector.h` pattern from ADR-0723). The atomic bridge guard
   already exists; wrap the non-atomic declarations in
   `#ifdef __cplusplus extern "C" { #endif`.
3. Replace C-style casts (`(FeatureVector **)realloc(...)`) with
   `static_cast`; replace `(void *)` casts as needed. Expect 10–20
   cast fixups.
4. `<stdatomic.h>` C11 generic calls compile as GNU extension in C++
   on Linux. No change needed; MSVC path is already bridged.
5. Update `core/meson.build`: move source from `c_sources` to
   `cpp_sources`.
6. No `ffmpeg-patches/` impact — `feature_extractor.c` is not a
   public-API surface.

### feature\_collector.c — requires test-internal header pattern first

Two options:

#### Option A (recommended): test-internal-header pattern

1. Extract the 6 static functions from `feature_collector.c` into
   `core/src/feature/feature_collector_internal.h` (not installed,
   guarded by `__VMAF_FEATURE_COLLECTOR_INTERNAL_H__`).
2. Rewrite `test_feature_collector.c` to
   `#include "feature/feature_collector_internal.h"` instead of
   `#include "feature_collector.c"`.
3. `git mv feature_collector.c feature_collector.cpp`; add
   `extern "C"` guards to `feature_collector.h`; update `meson.build`.

#### Option B (defer): keep as `.c`, convert later

Keep `feature_collector.c` in C. The file has no atomics and no
C++23-specific gain that cannot be achieved in C11. Convert only after
the test rewrite is done (can be a separate PR).

Option A is preferred because it eliminates the `.c`-include
anti-pattern, which is the primary reason Wave 7 retries failed. The
internal-header pattern is lower risk than it appears: the structs
(`AggregateVector`, `FeatureVector`) and their init/append/destroy
functions are purely heap-allocated value types with no cross-TU ABI
surface.

---

## 5. Effort estimate

| Task                                        | Effort    |
|---------------------------------------------|-----------|
| `feature_extractor.cpp` conversion          | 2–3 h     |
| `feature_collector_internal.h` extraction   | 1 h       |
| `test_feature_collector.c` rewrite          | 0.5 h     |
| `feature_collector.cpp` conversion          | 1–2 h     |
| `extern "C"` guard addition to both headers | 0.5 h     |
| Meson build wiring + CI validation          | 0.5 h     |
| **Total**                                   | **5–7 h** |

---

## 6. Risk assessment

| Risk                                             | Severity | Mitigation                                              |
|--------------------------------------------------|----------|---------------------------------------------------------|
| C++ strict aliasing breaks cast patterns         | Low      | Mechanical `static_cast` sweep; CI catches it           |
| `atomic_int` ABI mismatch on Linux gcc/clang     | Low      | GNU extension guarantees layout equivalence             |
| MSVC `icpx-cl` atomic path                       | Low      | Bridge already in `feature_extractor.h`                 |
| Test rewrite introduces coverage gap             | Low      | Static fns move verbatim into header; identical coverage|
| `extern "C"` wrapping collides with `pthread.h`  | Low      | Wrap function declarations only, not struct/typedef     |
| Netflix golden data regression                   | None     | Orchestration only; no score computation                |

**Overall: LOW risk. The extractor conversion is straightforward; the
collector requires one prerequisite step (internal-header extraction)
before it can be converted safely.**

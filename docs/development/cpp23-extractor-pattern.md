<!-- markdownlint-disable MD060 -->
# C++23 extractor conversion pattern

When converting a `core/src/*.c` internal file to C++23 per the ADR-0708 /
ADR-0723 migration plan, follow this checklist.

## Why convert?

Internal implementation files have no ABI constraint — they are never included
by downstream consumers. Converting them to C++23 enables:

- RAII lifecycle management (`std::unique_ptr`, destructors) replacing manual
  `malloc`/`free` pairs.
- `std::vector<T*>` replacing manual realloc/grow patterns.
- `std::span` for read-only views over raw pointer+length pairs.
- `[[nodiscard]]` on return-by-value factory functions.
- `nullptr`, `constexpr`, `static_assert` throughout.

## Step-by-step

### 1. Rename the file

```bash
git mv core/src/foo.c core/src/foo.cpp
```

### 2. Add `extern "C"` guards to the public header

The header is included by C callers; wrap the entire public surface:

```c
#ifdef __cplusplus
extern "C" {
#endif

/* existing struct typedefs and function declarations */

#ifdef __cplusplus
} /* extern "C" */
#endif
```

### 3. Handle `feature_extractor.h` in the `.cpp` file

`feature_extractor.h` uses `atomic_int` (via `<stdatomic.h>` in C, or
`<atomic>` + `using std::atomic_int` in C++). Including `<atomic>` inside
an `extern "C"` block causes a hard error ("template with C linkage"). The
required include order:

```cpp
/* 1. Pull in <atomic> BEFORE any extern "C" block. */
#include <atomic>

/* 2. Wrap all internal C headers that lack extern "C" guards. */
extern "C" {
#include "feature/feature_extractor.h"
#include "feature/feature_name.h"
#include "log.h"
/* ... other internal C headers ... */
} /* extern "C" */

/* 3. Include your own header last (it already has extern "C" guards). */
#include "foo.h"
```

The `<atomic>` include guard short-circuits the second pull from inside
`feature_extractor.h`, so no duplication occurs.

### 4. Update `core/src/meson.build`

Mirror the `metadata_handler_cpp20_lib` pattern:

```meson
foo_cpp23_lib = static_library(
    'foo_cpp23',
    src_dir + 'foo.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    pic : true,
    install : false,
)
```

Do **not** add `override_options : ['cpp_std=...']`. The C++ standard is
project-wide: `core/meson.build` probes the newest `-std=c++2x` the compiler's
standard library actually supports (or `/std:c++latest` on MSVC) and injects
it through `add_project_arguments` (ADR-1003 / ADR-1056). Meson emits that
flag after any per-target `cpp_std=` option, so a per-target override never
changed the effective standard; the redundant ones were removed under
epic #1241 (see `core/AGENTS.md`, invariant 7).

Remove the `.c` entry from `libvmaf_sources` and add a comment pointing to
the new lib. Add `foo_cpp23_lib.extract_all_objects(recursive: true)` to the
`objects:` list of the final `library()` target (alongside
`metadata_handler_cpp20_lib` and `fex_ctx_vector_cpp23_lib`).

### 5. Update `core/test/meson.build`

Any test executable that directly compiles `../src/foo.c` must be updated to
`../src/foo.cpp`. Meson handles mixed C/C++ `executable()` targets
transparently — `metadata_handler.cpp` and `fex_ctx_vector.cpp` are both
already compiled this way in `test_feature_extractor`.

### 6. Exception policy

All `std::bad_alloc` (or any other exception) must be caught before crossing
the `extern "C"` boundary and converted to an `int` error code (typically
`-ENOMEM`). The public C API signatures are unchanged.

## Reference implementations

| File | ADR | C++ idioms used |
|---|---|---|
| `core/src/metadata_handler.cpp` | ADR-0708 | `std::unique_ptr` + custom deleter for linked-list teardown |
| `core/src/fex_ctx_vector.cpp` | ADR-0723 | `std::vector::reserve` for init guard; `extern "C"` + pre-`<atomic>` include pattern |

## Governing ADRs

- [ADR-0708](../adr/0708-vmafx-cpp23-internals-pilot.md) — Wave 1 policy,
  `metadata_handler.c` pilot.
- [ADR-0723](../adr/0723-cpp23-pilot-fex-ctx-vector.md) — Wave 2,
  `fex_ctx_vector.c`; establishes the `feature_extractor.h` include pattern.

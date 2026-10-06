---
paths:
  - core/src/output.cpp
  - core/src/thread_locale.cpp
  - core/src/thread_locale.h
  - core/src/opt.cpp
  - core/src/dict.cpp
invariant: Score capacity checks use >=; JSON writers guard delimiters; locale pushes flush before pop.
---
<!-- markdownlint-disable MD013 -->
# Output writers, score bounds, and thread locale pushes

## 4. Capacity bounds checks in `output.c` must use `>=`, not `>` (ADR-0606)

All frame-iteration loops in [`output.c`](../output.c) guard per-feature
access with:

```c
if (i >= fc->feature_vector[j]->capacity)  /* ADR-0606: >= not > */
    continue;
```

Allocated score array covers indices `0..capacity-1`. Index `capacity`
= one past end. Using `>` (strictly greater) allows access at
`i == capacity`, = heap buffer overread (UB). Under
`MALLOC_PERTURB_=198` (macOS CI setting), poisoned byte at
`score[capacity].written` = `0xC6` (truthy), causing spurious "written"
results and downstream SIGSEGV under Apple Clang's UB optimizations.

If upstream sync or cherry-pick replaces any of 7 capacity-check
sites with `>`, revert back to `>=` in same commit.

## 5. JSON writers must use explicit `bool first` flags (ADR-0606)

`json_write_pool_score` and `json_write_frames` in [`output.c`](../output.c)
track whether comma separator needed via explicit `bool first` /
`bool first_frame` flags. Never replace these with:

- `j > 1` (pool method enum) — wrong when `j == 1` call skipped and
  `j == 2` first, producing leading comma in JSON object.
- `i > 0` (frame index) — wrong when frame 0 has no written scores and
  frame 3 first, producing leading comma in JSON array.

## 6. macOS locale pushes must use a duplicated base locale

`thread_locale.c::vmaf_thread_locale_push_c()` must not call
`newlocale(..., "C", NULL)` on POSIX hosts. On macOS, allocator poisoning can
leave Apple libc's freshly allocated internal locale object with poisoned
category pointers before `uselocale()` / `fprintf()` touches it, causing
writer tests to SIGSEGV only on Darwin. Invariant:

```c
locale_t base = duplocale(LC_GLOBAL_LOCALE);
state->c_locale = newlocale(LC_NUMERIC_MASK, "C", base);
```

Never pass `LC_GLOBAL_LOCALE` directly as `newlocale()` base; duplicate it
first, and `freelocale(base)` on `newlocale()` failure. Output writers only
need numeric formatting isolation, so never widen this back to `LC_ALL_MASK`
without macOS CI run covering `test_output`, `test_public_api_score`, and
`test_vmaf_use_tiny_model`.

## 7. `test_output` must not include libvmaf implementation TUs

`core/test/test_output.c` links against libvmaf and reaches owned
collector through `libvmaf_priv.h::vmaf_feature_collector_get()`. Never bring
back `#include "libvmaf.c"` or `#include "output.c"` in that test while it also
links libvmaf: Apple ld64 + LTO has resolved duplicate external definitions
incorrectly under allocator poisoning, crashing macOS writer tests.

## 8. Output writers flush before popping the C numeric locale

`output.c` writers call `fflush(outfile)` before
`vmaf_thread_locale_pop(locale_state)`. Keep stream flush inside
temporary C numeric locale lifetime. Path-based `vmaf_write_output()` uses
`fdopen()` and may otherwise leave final flush to `fclose()` after
locale has been restored/freed; that = macOS-only SIGSEGV shape for
`test_output` and `test_public_api_score`.

## Option numbers: C locale on the calling thread (T-OPTION-NUMBERS-CALLER-LOCALE-2026-10-06)

`opt.cpp` `parse_double()` + `dict.cpp` `dict_normalize_numeric()`: `strtod()` / `%g`
inside `vmaf_thread_locale_push_c()` scope (`CLocaleScope` RAII in `dict.cpp`). Decimal-comma
caller locale otherwise refuses `0.7` (-EINVAL; `vmaf_v1.0.16_3d0h` unusable) or stores
`0.02` as `0` / `0,02`. Standalone test builds of either file link `thread_locale.cpp`.
Guard: `test_locale_handling` (`test_option_double_with_comma_locale`,
`test_dictionary_number_with_comma_locale`, `test_model_features_with_comma_locale`).

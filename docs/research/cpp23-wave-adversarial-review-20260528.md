<!-- markdownlint-disable MD013 MD036 MD060 -->
# Research: Adversarial code review — C++23 conversion wave (PRs #41–#58)

**Date**: 2026-05-28
**Scope**: PRs #41, #43, #44, #45, #48, #51, #54, #56, #58
**Reviewer**: adversarial agent (claude-sonnet-4-6)
**Reproducer**: `gh pr diff <N> -R VMAFx/vmafx > /tmp/pr-<N>.diff` for N in {41,43,44,45,48,51,54,56,58}

---

## Executive Summary

**CRITICAL findings: 4**
**HIGH findings: 3**
**MEDIUM findings: 5**
**LOW / nit findings: 3**

Four CRITICAL issues were identified and must be resolved before any of the
affected PRs merges into master. Two are use-after-free / allocator mismatch
bugs, one is a genuine ODR violation risk, and one is a latent UB in the
`std::string_view` + `strtol`/`strtod` boundary.

---

## PR #41 — `mem.c` → `mem.cpp` (ADR-0720)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 1 | `core/src/mem.h` (declarations) | CERT MEM31-C; Power of 10 #9 | LOW | `[[nodiscard]]` is applied to the `.cpp` definition but NOT to the C-visible declaration in `mem.h`. Under the `extern "C"` guard the declaration is a plain `void *aligned_malloc(...)` with no attribute. C callers compiled without `[[nodiscard]]` propagation will silently drop returns. This is the original intent (comment says "C callers are unaffected") but limits the safety benefit to C++ TUs only. | Acceptable as-is given the stated design intent; document the limitation explicitly in the header comment. |
| 2 | `core/src/mem.cpp` line 502 | Power of 10 #5 (assertion density) | LOW | Both `aligned_malloc` and `aligned_free` have zero `assert()` / `VMAF_ASSERT_DEBUG()` calls. Per §1.1 rule 5, functions should have ≥ 2 assertions on average. At minimum assert `alignment > 0 && (alignment & (alignment - 1)) == 0` (power-of-two) at entry. | Add `assert(alignment > 0 && (alignment & (alignment - 1)) == 0);` at the top of `aligned_malloc`. |

**Verdict: NEEDS-CHANGES (low-priority)** — findings are nits; no blocker.

---

## PR #43 — `opt.c` → `opt.cpp` (ADR-0721)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 3 | `core/src/opt.cpp` `parse_int` (lines 139–141) | CERT STR32-C; Power of 10 #2 | **CRITICAL** | `std::string_view` is constructed from the caller's `const char *val` (a C-string), then `sv.data()` is passed directly to `std::strtol`. The code comment says "in practice vmaf_option_set is always called with a C string literal or argv element, so sv.data() is NUL-terminated." This assumption is **not enforced**. If `parse_int` is ever invoked with a `string_view` slice that is NOT NUL-terminated (e.g. from future C++23 callers constructing a `string_view` over a substring), `strtol` will read past the slice boundary. The `*end != '\0'` check catches the case where there is trailing non-numeric text, but cannot prevent the overread for a non-NUL-terminated slice. | Either (a) keep the `string_view` parameter for the internal API signature but copy `sv` to a `std::string` before calling `strtol` (one allocation per parse, safe), or (b) add `assert(sv.data()[sv.size()] == '\0')` to make the assumption explicit and auditable. The same issue applies to `parse_double`. |
| 4 | `core/src/opt.cpp` `vmaf_option_set` (line 179) | JPL Rule 21; Power of 10 #10 | MEDIUM | `[[nodiscard]]` is applied to `vmaf_option_set` in the `.cpp` definition but the declaration in `opt.h` has no `[[nodiscard]]`. The existing C caller in `feature_extractor.c` does check the return value, but if the header is included from future C++ TUs the attribute on the definition does not propagate from the definition to call sites that see only the declaration. | Add `[[nodiscard]]` to the declaration in `opt.h` inside the `extern "C"` block (C compilers ignore the attribute; C++ compilers honour it). |

**Verdict: NEEDS-CHANGES (CRITICAL #3 must be addressed)**

---

## PR #44 — `fex_ctx_vector.c` → `fex_ctx_vector.cpp` (ADR-0723)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 5 | `core/src/fex_ctx_vector.cpp` `feature_extractor_vector_init` (lines 179–202) | Power of 10 #3 (no heap in hot paths) | MEDIUM | The `std::vector<VmafFeatureExtractorContext *> v` constructed inside `feature_extractor_vector_init` is created and immediately discarded with `(void)v`. The comment says "vector used only to validate the reserve above". This is dead code — the `try/catch` block wrapping it serves no purpose because `v` is a local that is destroyed before it can fail in any observable way, and `malloc` below is the actual allocating path. The try/catch around the vector reserve catches a `std::bad_alloc` that is irrelevant because the `malloc` that immediately follows is entirely outside the try block. If the intent was to use RAII, the vector is abandoned before that can help. | Remove the dead try/catch + vector entirely. Replace with a single comment that `malloc` below handles OOM. The `vector` idiom was a reasonable intent but the implementation as written is dead code that misleads future readers. |
| 6 | `core/src/fex_ctx_vector.cpp` `feature_extractor_vector_append` (lines 258–268) | CERT INT30-C; Power of 10 #2 | MEDIUM | `const size_t capacity = static_cast<size_t>(rfe->capacity) * 2u;` — capacity doubling can overflow `size_t` when `rfe->capacity` is near `SIZE_MAX / 2`. The result is then cast back to `unsigned` for `rfe->capacity` (line 265). If `capacity` overflowed, `rfe->fex_ctx` will be reallocated to a huge (wrapping) size and the cast-back to `unsigned` will silently truncate, causing a buffer overrun on the next append. In practice the vector will never grow that large, but this is UB on the path. | Add `assert(rfe->capacity <= (SIZE_MAX / 2) / sizeof(*rfe->fex_ctx));` before the realloc. |

**Verdict: NEEDS-CHANGES (MEDIUM)**

---

## PR #45 — `log.c` → `log.cpp` (ADR-0725)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 7 | `core/src/log.cpp` `vmaf_log` (line 152) | CERT STR32-C; Power of 10 #10 | **HIGH** | `std::fprintf(stderr, sf.data(), ...)` — `sf` is a `std::string_view` obtained from `level_str_color[idx]` and `level_str[idx]`, both of which are string literals. The comment says "data() pointer passed to fprintf is a null-terminated string literal, so it satisfies the %s format requirement." This is correct for the ANSI escape sequences passed to `%s`. However, `sf.data()` for a `std::string_view` does NOT guarantee NUL-termination in the general case; if this code is ever refactored and a non-literal `string_view` is passed (e.g. a slice), `%s` will read past the end. The fix should be `.c_str()` on a `std::string` copy, or use `std::format`/`std::print` which don't have this footgun. | The current instances are provably safe because they use string literals. Add a `static_assert` or `assert` that `sv.data()[sv.size()] == '\0'` at the point of construction to document and enforce the NUL invariant. |
| 8 | `core/src/log.cpp` static variables `vmaf_log_level`, `istty` (lines 107–108) | Power of 10 #6 (smallest scope) | LOW | `static VmafLogLevel vmaf_log_level` and `static int istty` are file-scope mutable state shared across threads. `vmaf_set_log_level` and `vmaf_log` are public `extern "C"` functions that read and write these without any synchronisation. This was a pre-existing issue in the C version, but the C++23 migration is an opportunity to address it with `std::atomic`. | Convert both to `std::atomic<VmafLogLevel>` and `std::atomic<int>` with `memory_order_relaxed` for the read, `memory_order_release` for the write (same approach as PR #58 / ADR-0735 for `cpu.cpp`). |

**Verdict: NEEDS-CHANGES (HIGH #7)**

---

## PR #48 — `dict.c` → `dict.cpp` + project-wide `cpp_std=c++23` (ADR-0727)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 9 | `core/src/dict.cpp` `dict_normalize_numeric` (line 148) | CERT FLP34-C (floating-point precision); JPL Rule 14 | **CRITICAL** | `double dv = std::strtof(val.data(), &end);` — the function signature says `double` but calls `std::strtof` (which returns `float`). This is an implicit widening from `float` to `double` that introduces precision loss: the original `dict.c` presumably used `strtod` or `strtof` intentionally, but here the result is widened back to `double` before being passed to `snprintf(nullptr, 0, "%g", dv)`. The `%g` format of a `float`-precision value differs from a `double`-precision one. For example, `"0.1"` via `strtof` → 0.10000000149... then `%g` → `"0.1"` (okay), but `"0.12345678"` via `strtof` → `0.123457` (6 sig figs) then formatted back as `"0.123457"`, losing precision vs the user-supplied string. This is a **score-corruption bug** if callers pass high-precision floating-point option values (e.g. `vmaf_feature_dictionary_set` passes raw option strings through here). | Change `std::strtof` to `std::strtod` to match the `double dv` variable type. |
| 10 | `core/test/test_dict.cpp` (line 451) | Power of 10 #8 (preprocessor restraint) | MEDIUM | `#include "dict.cpp"` is a source-inclusion unity pattern to test the `static isnumeric()` helper. This is documented as "intentional: white-box test". However, including a `.cpp` implementation file from a test file creates an **ODR (One-Definition-Rule) violation** if `dict.cpp` is also compiled as a regular TU in the same link unit. The meson.build wires `test_dict` to compile `dict.cpp` only via the `include` (the `test_dict.cpp` itself is the only source), so in practice no double-definition occurs in the current build graph — but this is fragile. If someone adds `dict.cpp` to the test's `sources:` list (the intuitive fix when symbols are missing), they get duplicate symbol link errors or silent COMDAT folding. | Export `isnumeric` as `internal linkage` via a companion header (`dict_internal.h`) that can be `#include`d from tests without including the `.cpp`. Alternatively, move `isnumeric` into a separate `dict_utils.cpp` that can be compiled independently. |

**Verdict: NEEDS-CHANGES (CRITICAL #9 must be addressed)**

---

## PR #51 — Wave 3 part B: `psnr_tools.cpp`, `luminance_tools.cpp`, `mkdirp.cpp` (ADR-0731)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 11 | `core/src/feature/mkdirp.cpp` `mkdirp()` (lines 289–314) | Power of 10 #1 (no recursion) | **HIGH** | `mkdirp` is implemented recursively: line 302 calls `mkdirp(parent.c_str(), mode)`. The original C version was also recursive. Power of 10 rule 1 explicitly prohibits recursion. On deeply nested paths (adversarial input with path depth > ~1000 on Linux default stack), this recurses once per path component and can overflow the stack. The C→C++ conversion preserved the recursion without flagging it. | Replace with an iterative approach: accumulate path components in a `std::vector<std::string>`, then create them in forward order. |
| 12 | `core/src/feature/luminance_tools.cpp` anonymous namespace (line 111) | JPL Rule 21; Power of 10 #10 | MEDIUM | `[[nodiscard]] static int range_foot_head(...)` is declared `static` inside an anonymous namespace. Both `static` and anonymous namespace give internal linkage; combining them is redundant and produces an `-Wredundant-decls` warning with `-Wall -Wextra`. | Remove the `static` keyword; anonymous namespace already provides internal linkage. |

**Verdict: NEEDS-CHANGES (HIGH #11 — recursion in mkdirp violates Power of 10 #1)**

---

## PR #54 — Wave 3 part A: `feature_name.cpp`, `picture_copy.cpp`, `model.cpp` (ADR-0729)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 13 | `core/src/model.cpp` `vmaf_model_collection_append` (line 887) | CERT INT30-C; CERT MEM30-C | **CRITICAL** | `const size_t name_sz = strlen(model->name) - 5U + 1U;` — if `model->name` has fewer than 5 characters, `strlen(model->name) - 5U` wraps around to a huge `size_t` (unsigned subtraction underflow), causing `calloc(1, huge_value)` to fail or to allocate an enormous buffer. The subsequent `strncpy(name_buf, model->name, name_sz - 1U)` would then copy `SIZE_MAX - 4` bytes, a definite heap overflow. No guard exists for `strlen(model->name) < 5`. This is present in the original `model.c` as well (pre-existing), but the C++23 conversion is an opportunity to fix it. | Add `assert(strlen(model->name) >= 5U);` before the subtraction, or use `const size_t raw_len = strlen(model->name); const size_t name_sz = raw_len >= 5U ? raw_len - 5U + 1U : 1U;`. |
| 14 | `core/src/feature/picture_copy.cpp` (line 407) | CERT INT31-C | MEDIUM | `dst += dst_stride / static_cast<std::ptrdiff_t>(sizeof(float));` — `dst_stride` is `std::ptrdiff_t` (signed), `sizeof(float)` is `size_t` (unsigned). The cast to `ptrdiff_t` of `sizeof(float)` is safe (always 4), but the division result is a signed `ptrdiff_t` added to a `float *`. If `dst_stride` is negative (e.g. for bottom-up images), the arithmetic is defined but the pointer advance is backwards. This is intentional in the original C code for negative-stride cases; the C++23 port preserves the behaviour correctly. | POSSIBLY: Flag only if negative strides are not a supported use case. If negative strides are valid, add a comment explaining the semantics. |
| 15 | `core/src/feature/feature_name.cpp` (lines 158–162) `vmaf_feature_name_from_opts_dict` | CERT MEM31-C | MEDIUM | `vmaf_dictionary_copy(&opts_dict, &sorted_raw)` — the return value of `vmaf_dictionary_copy` is discarded without checking. If the copy fails (returns non-zero due to OOM), `sorted_raw` will be NULL and the subsequent `DictPtr sorted(sorted_raw)` will wrap a null pointer. The loop `for (unsigned i = 0; i < sorted->cnt; i++)` then dereferences `sorted` which is null via `sorted->cnt`, causing a null-pointer dereference. | Check the return value: `if (vmaf_dictionary_copy(&opts_dict, &sorted_raw) != 0) return nullptr;` |

**Verdict: NEEDS-CHANGES (CRITICAL #13)**

---

## PR #56 — Wave 4: `output.c` → `output.cpp` (ADR-0733)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 16 | `core/src/output.cpp` `LocaleGuard` class (lines 199–218) | extern "C" / exception safety | MEDIUM | `LocaleGuard::LocaleGuard()` calls `vmaf_thread_locale_push_c()` (a C function declared `extern "C"`) in the constructor body. `vmaf_thread_locale_push_c` is declared `noexcept` in the destructor but not in the implementation. If `vmaf_thread_locale_push_c` calls `newlocale` / `duplocale` which fail and the current implementation returns `nullptr`, the `LocaleGuard` stores a null `state_` without error propagation. Then `LocaleGuard::~LocaleGuard()` calls `vmaf_thread_locale_pop(nullptr)`, which must handle null gracefully. Inspecting PR #58's `thread_locale.cpp`: `vmaf_thread_locale_pop` does `std::unique_ptr<VmafThreadLocaleState> guard(state);` — if `state` is null, `unique_ptr` destructor is a no-op, so this is safe. However, the locale push silently failed and the writers proceed without C-locale — potentially producing incorrect decimal separators on systems with non-C locale. | The `LocaleGuard` constructor should detect `state_ == nullptr` and take an observable action (log a warning). Document that locale-push failure is non-fatal but degrades output. |
| 17 | `core/src/output.cpp` `pool_method_name` array (line 237–244) | JPL Rule 23 (switch default) | LOW | The static `pool_method_name[]` array comment says "Index 0 (UNKNOWN) is nullptr and never accessed". The looping callers use `j=1` to `VMAF_POOL_METHOD_NB-1`. If `VMAF_POOL_METHOD_NB` is ever extended and an enum value ≥ 5 is added upstream without updating the array, `pool_method_name[j]` will read out of bounds silently. | Add a `static_assert(VMAF_POOL_METHOD_NB == 5, "pool_method_name array size mismatch — update array");` immediately after the array definition. |

**Verdict: NEEDS-CHANGES (MEDIUM)**

---

## PR #58 — Wave 5: `cpu.cpp`, `ref.cpp`, `thread_locale.cpp` (ADR-0735)

| # | File:line | Rule | Severity | Description | Suggested fix |
|---|-----------|------|----------|-------------|---------------|
| 18 | `core/src/ref.cpp` `vmaf_ref_init` (line 297) | CERT MEM34-C; allocator mismatch | **CRITICAL** | `std::make_unique<VmafRef>()` allocates via `operator new`. `vmaf_ref_close` uses `delete ref`. This pairing is **correct** when both are in the same TU compiled by the same C++ compiler. However, `VmafRef` is a C-ABI struct whose definition is in `ref.h`, included by multiple C TUs (`picture_pool.c`, `framesync.c`). **If any of those C callers ever directly free a `VmafRef*`** (e.g. via `free(ref_ptr)` as was done in the original C code), they will `free` memory that was allocated by `operator new`, causing heap corruption. The public `vmaf_ref_close` function now uses `delete` internally, but C callers have no way to know this from the header signature. The original `malloc`/`free` pair was safe for cross-TU C callers. | The allocator mismatch is an ABI contract change that must be documented and enforced. Add `static_assert` or a linker-version script that prevents `free(VmafRef*)` at call sites. At minimum add a prominent comment to `ref.h` that `vmaf_ref_close` is now the ONLY valid deallocator and that `free()` on a `VmafRef*` is UB. |
| 19 | `core/src/cpu.cpp` `g_flags` / `g_flags_mask` (lines 114–115) | Static-storage initialization ordering | MEDIUM | `std::atomic<unsigned> g_flags{0u}` and `g_flags_mask{std::numeric_limits<unsigned>::max()}` are namespace-scope objects with non-trivial constructors. If another translation unit's static-storage initializer calls `vmaf_get_cpu_flags()` before `cpu.cpp`'s statics are constructed (static initialization order fiasco), the call will operate on unconstructed `std::atomic` objects — UB. The original `static unsigned flags = 0` was trivially initialized. | Mark both as `constinit std::atomic<unsigned>` (C++20/23 keyword guarantees compile-time initialization with no SIOF risk). `constinit std::atomic<unsigned> g_flags{0u};` |

**Verdict: NEEDS-CHANGES (CRITICAL #18)**

---

## Summary

| PR | ADR | Verdict | Critical | High | Medium | Low |
|----|-----|---------|---------|------|--------|-----|
| #41 | 0720 | NEEDS-CHANGES (nits) | 0 | 0 | 0 | 2 |
| #43 | 0721 | NEEDS-CHANGES | 1 | 0 | 1 | 0 |
| #44 | 0723 | NEEDS-CHANGES | 0 | 0 | 2 | 0 |
| #45 | 0725 | NEEDS-CHANGES | 0 | 1 | 0 | 1 |
| #48 | 0727 | NEEDS-CHANGES | 1 | 0 | 1 | 0 |
| #51 | 0731 | NEEDS-CHANGES | 0 | 1 | 1 | 0 |
| #54 | 0729 | NEEDS-CHANGES | 1 | 0 | 2 | 0 |
| #56 | 0733 | NEEDS-CHANGES | 0 | 0 | 2 | 0 |
| #58 | 0735 | NEEDS-CHANGES | 1 | 0 | 1 | 0 |

**Clean PRs: 0**
**PRs needing fixup: 9/9**
**Total findings: 19** (4 critical, 2 high, 10 medium, 3 low)

### Critical findings at a glance

1. **PR #43 / `opt.cpp` finding #3** — `strtol`/`strtod` called on potentially non-NUL-terminated `string_view::data()`; UB if any future caller passes a substring slice.
2. **PR #48 / `dict.cpp` finding #9** — `strtof` (float precision) assigned to `double dv`, then formatted with `%g`; precision loss on high-precision option values causes score corruption.
3. **PR #54 / `model.cpp` finding #13** — `strlen(model->name) - 5U` wraps to `SIZE_MAX-4` when name is shorter than 5 chars; heap overflow via `calloc(1, SIZE_MAX-4)`.
4. **PR #58 / `ref.cpp` finding #18** — `make_unique` → `operator new`; C callers that call `free(ref_ptr)` directly (pre-existing pattern) will corrupt the heap; allocator contract change is invisible at the C-header boundary.

---

## Positive observations

- The `extern "C"` guard pattern across all headers is consistently applied and correct.
- The isolated-static-library meson pattern (`override_options: ['cpp_std=c++23']`) cleanly prevents dialect leakage.
- PR #58's `VmafThreadLocaleState` destructor is a textbook RAII cleanup — the `vmaf_thread_locale_pop` one-liner that adopts into `unique_ptr` is elegant and correctly handles null.
- PR #56's `LocaleGuard` correctly deletes copy/move constructors.
- PR #43's NaN-bypass guard in `parse_double` (finding pre-existing in C version) is a genuine improvement over typical C implementations.

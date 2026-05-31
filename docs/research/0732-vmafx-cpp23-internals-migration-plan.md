<!-- markdownlint-disable MD013 MD049 MD060 -->
# Research-0732: VMAFX C++23 Internals Migration Plan

_Date: 2026-05-28_
_Author: lusoris (AI-assisted, Claude Code)_
_Status: Accepted — accompanies ADR-0708 (pilot conversion of `metadata_handler.c`)_

---

## 1. Motivation

The VMAFX fork's `core/src/` is implemented in C11 (now C23 after ADR-0692). The C ABI
boundary — `core/include/libvmaf/*.h` — is and must remain pure C with `extern "C"` guards.
However, the *internal* implementation files (`core/src/*.c`) have no such constraint: they
are never included by downstream C consumers directly.

Selected internal files contain patterns that cause real footguns in maintenance:

- Manual malloc/free paired with early-return error paths — easy to leak on OOM.
- Raw pointer + length parameter pairs — callers can silently mis-size them.
- `#define` numeric constants — no type safety, no debugger visibility.
- C-style `void *` dispatch tables — no compile-time type checking.

C++23 removes each of these pain points with language-supported features:
`std::unique_ptr`, `std::span`, `constexpr`, `std::expected`, and concepts.

Converting internal files one-by-one (`.c → .cpp`, keeping the C header) is safe:
meson already uses both `c` and `cpp` compilers (the `svm.cpp` vendored file, all SYCL
TUs). The ABI is not affected — symbols in the `.cpp` file that other C TUs call stay
reachable through the `.h` header's `extern "C"` guard.

---

## 2. Survey of `core/src/*.c` candidates

### 2.1 Scoring methodology

| Dimension | 1 | 3 | 5 |
|---|---|---|---|
| **Effort** | Trivial (<100 LOC, no complex types) | Medium (100–300 LOC, one complex struct) | Hard (>300 LOC, threading, complex state) |
| **Value** | Cosmetic / style only | Removes a real footgun (leak, UB, misuse) | Eliminates major correctness risk class |
| **Risk** | No float arithmetic, not on the score path | Touches score bookkeeping, adjacent to float | Hot float path; bit-exactness exposure |

ROI formula: `value / (effort × risk)`. Candidates with ROI ≥ 1.0 are recommended
for conversion; ROI ≥ 2.0 are high-priority.

### 2.2 Candidate table

| File | Lines | Effort | Value | Risk | ROI | C++23 idioms |
|---|---|---|---|---|---|---|
| `metadata_handler.c` | 84 | 1 | 4 | 1 | **4.0** | `unique_ptr<VmafCallbackItem>`, `constexpr`, RAII linked-list |
| `log.c` | 72 | 1 | 3 | 1 | **3.0** | `constexpr` arrays, `std::string_view`, `std::format` (C++23) |
| `mem.c` | 52 | 1 | 2 | 1 | **2.0** | `std::align_val_t`, `operator new` with alignment |
| `opt.c` | 123 | 2 | 3 | 1 | **1.5** | `std::variant<bool,int,double,char*>`, `constexpr`, concepts |
| `ref.c` | 54 | 1 | 2 | 2 | **1.0** | `std::atomic<long>`, `std::make_unique` |
| `fex_ctx_vector.c` | 151 | 2 | 4 | 2 | **1.0** | `std::vector<VmafFeatureExtractorContext*>`, concepts |
| `dict.c` | 318 | 3 | 5 | 2 | **0.83** | `std::unordered_map`, `std::string`, RAII strdup-free |
| `framesync.c` | 308 | 4 | 4 | 2 | **0.50** | RAII mutex guards, `std::optional` for frame buffers |
| `picture.c` | 228 | 3 | 5 | 3 | **0.56** | `std::unique_ptr` for pixel buffer, pool RAII, `std::span` |
| `thread_pool.c` | 354 | 4 | 3 | 2 | **0.38** | `std::thread`, `std::queue`, `std::mutex`, `std::condition_variable` |
| `predict.c` | 639 | 5 | 3 | 3 | **0.20** | NOT recommended — high risk, touches float score computation |
| `feature/` extractors | varies | 5 | 2 | 5 | **<<1** | Out of scope — bit-exactness critical |
| `pdjson.c` | 963 | 5 | 1 | 1 | **0.20** | Vendored — do not convert |

### 2.3 Top-5 ROI candidates for future conversion PRs

In priority order:

1. **`metadata_handler.c`** (ROI 4.0) — Pilot in this PR. 84-line linked-list manager with
   malloc/free pairs. `unique_ptr<VmafCallbackItem>` replaces the manual teardown in
   `vmaf_metadata_destroy`; a custom deleter can walk the chain. Zero float code,
   zero score impact.

2. **`log.c`** (ROI 3.0) — 72 lines. `constexpr` replaces the `level_str` and
   `level_str_color` C arrays. In C++23, `std::print` / `std::format` can replace
   the raw `fprintf`/`vfprintf` pair for type-safe format strings, though the
   migration must keep the `va_list` C API for compatibility unless `vmaf_log` is
   refactored to a variadic C++ template in an internal header.

3. **`mem.c`** (ROI 2.0) — 52 lines. `aligned_malloc` / `aligned_free` can become
   `operator new(size, std::align_val_t{alignment})` / `operator delete(ptr, align)`.
   The pair has only ~15 direct call sites in `core/src/`; all are in C files that
   include `mem.h` through internal headers — `extern "C"` in `mem.h` keeps the
   link name unchanged.

4. **`opt.c`** (ROI 1.5) — 123 lines. Option parsing with four `switch`-dispatched
   types is the canonical `std::variant` / `std::visit` use case. A C++23 conversion
   replaces the union-of-defaults pattern with `std::variant<bool,int,double,const char*>`
   and eliminates the silent integer-truncation cast `(int)n`. The C API
   (`vmaf_option_set`) stays unchanged.

5. **`fex_ctx_vector.c`** (ROI 1.0) — 151 lines. The manual grow-on-capacity vector
   (`realloc` + `NULL`-fill loop) is `std::vector<VmafFeatureExtractorContext*>`.
   C++23 concepts can constrain the dispatch table type at the call site.

### 2.4 Files recommended to remain C

- All `core/src/feature/*.c` extractors — bit-exactness constraint; ROI < 0.5.
- `pdjson.c` — vendored; minimize diff from upstream for rebasing.
- `predict.c` — largest and most complex; high risk. Defer until strong test coverage
  exists for every code path.
- `libvmaf.c` — public-facing entry point hub; any accidental ABI change is critical.
  Convert only after all internal helpers have migrated.

---

## 3. Migration recipe (per-file)

The same recipe applies to every file in §2.3. The pilot (`metadata_handler.c`)
demonstrates each step concretely.

### Step 1 — `git mv`, not copy

```bash
git mv core/src/<file>.c core/src/<file>.cpp
```

`git mv` preserves blame history. The file content is identical immediately after;
C++-specific changes land in the same commit.

### Step 2 — `meson.build` update

No change needed if the file is already in `libvmaf_sources` — meson detects `.cpp`
automatically and compiles with `$(CXX)`. However, C++23 requires an explicit
`override_options`:

```meson
# In core/src/meson.build, replace the plain string entry:
#   src_dir + 'metadata_handler.c',
# with a files() + override:
libvmaf_sources += files(src_dir + 'metadata_handler.cpp')
```

Actually, for the pilot file we add the `cpp_std=c++23` override via a
`static_library`-level override — see `vulkan/meson.build` precedent. The main
`libvmaf_sources` list includes the `.cpp` file; the project-level `cpp_std=c++11`
default is acceptable for all other C++ TUs. Only the newly-converted files need
`c++23`; we can set that at project level once ≥3 files have migrated.

### Step 3 — Header `extern "C"` guard

The internal `.h` file (e.g. `metadata_handler.h`) is included by C callers
(`feature_collector.c`). Add guards:

```c
#ifdef __cplusplus
extern "C" {
#endif

// … existing declarations …

#ifdef __cplusplus
}
#endif
```

This is idempotent: if the header is ever included by another C++ TU the guard is
a no-op.

### Step 4 — Apply C++23 idioms where they actually remove footguns

Apply only where the change reduces a real bug class. Do not C++-ify code solely
for stylistic parity with C++; the conversion is incremental.

| Pattern (C) | Replacement (C++23) | Footgun removed |
|---|---|---|
| `malloc` + `free` on heap-allocated node | `std::unique_ptr<T>` | Leak on early return |
| `NULL`-terminated linked list traversal + `free` per node | Custom `Deleter` on `unique_ptr` or recursive reset | Partial-free on OOM |
| `#define CONSTANT value` | `constexpr T name = value;` | No type, no debugger, ODR issues |
| Pointer+length pair | `std::span<T>` on internal paths | Mismatched sizes |
| `void *` cast for dispatch table | Concept-constrained template | Unchecked type at call site |
| Pair of `malloc`/`free` in error paths | RAII wrapper | Leak on every new early-return |

### Step 5 — Explicit casts for C → C++ type narrowing

C allows implicit pointer conversions (e.g. `void * → T *`, integer narrowing).
C++ requires explicit casts for most of these. Run:

```bash
ninja -C /tmp/cpp23-build 2>&1 | grep 'error:' | head -40
```

Common fixes:

- `malloc(n)` return — add `static_cast<T*>(malloc(n))`.
- `(void)`-casts on discarded returns — already required by CLAUDE.md §6; should be
  present.
- `int` → `enum` assignments — add `static_cast<EnumType>(…)`.

### Step 6 — Build verification

```bash
meson setup /tmp/cpp23-build core \
  -Denable_cuda=false -Denable_sycl=false
ninja -C /tmp/cpp23-build
```

The build must be clean (zero warnings with `-Wall -Wextra`).

### Step 7 — Netflix golden gate

```bash
make test-netflix-golden
```

Scores must be `76.668` to `places=4` on all three reference pairs. A `.c → .cpp`
rename with internal-only changes cannot change scores; this step is a safeguard
against accidental linkage changes.

### Step 8 — Unit test coverage

Check `core/test/test_<file>.c` exists; if not, add it. For `metadata_handler`, the
test is `test_propagate_metadata.c` (exercises `vmaf_metadata_init`, `_append`,
`_destroy`). Verify it still compiles as a C file including the C header with
`extern "C"` guards.

---

## 4. C++ standard level per migration stage

| Stage | Files | cpp_std required |
|---|---|---|
| Pilot (this PR) | `metadata_handler.cpp` | `c++20` (for `std::span`; `unique_ptr` is `c++11`) |
| Wave 1 (log, mem, opt) | 3 files | `c++20` |
| Wave 2 (ref, fex_ctx_vector) | 2 files | `c++20` |
| Wave 3 (dict) | 1 file | `c++23` (`std::expected` for dict_set error returns) |
| Project-wide bump | All C++ TUs | `c++23` (after ≥5 files converted) |

The project-level `cpp_std=c++11` is not changed in the pilot PR. The pilot uses
a per-target `override_options : ['cpp_std=c++20']` in `meson.build`, matching
the Vulkan backend precedent (see `core/src/vulkan/meson.build:157`).

---

## 5. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| C caller fails to link — `extern "C"` not on the header | Medium | P0 — build break | Checked in Step 3; CI compile gates catch this |
| Float arithmetic changes → golden gate regression | Low | P0 — must not happen | `metadata_handler` has zero float code; scores unchanged by construction |
| clang-tidy C++ rules fire on converted file | Medium | P1 — lint gate fails | Run `make lint` before pushing; address per CLAUDE.md §12 r12 |
| C++ ABI mangles symbol name if `extern "C"` forgotten | Low | P0 — link error | Any C caller that misses `extern "C"` will produce a linker undefined-symbol error, caught immediately |
| SYCL/CUDA device-code compilers reject C++23 | Low | P1 for GPU paths | `metadata_handler` is not included by any GPU TU; risk is zero for the pilot |

---

## 6. Test plan for future PRs

Each wave-N conversion PR must:

1. Pass `meson test -C build --suite=fast` (pre-push gate).
2. Pass `make test-netflix-golden` (scores unchanged, places=4).
3. Pass `make lint` on the converted file (zero new clang-tidy errors).
4. Include a `core/test/test_<file>.cpp` or confirm existing test still compiles.

---

_See ADR-0708 for the policy decision covering this migration plan._
_Pilot conversion: `core/src/metadata_handler.cpp` (was `.c`)._

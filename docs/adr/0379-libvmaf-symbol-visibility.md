<!-- markdownlint-disable MD060 -->
# ADR-0379: libvmaf Symbol Visibility — Hide Internal Symbols with `-fvisibility=hidden`

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: Lusoris fork maintainers
- **Tags**: `build`, `api`, `security`, `abi`, `fork-local`

## Context

Research-0092 (round-4 sanitizer sweep, angle 6) identified that `libvmaf.so.3`
exports **207 internal symbols** that are not part of the public `vmaf_*`-prefixed
API. These symbols fall into several categories:

- **libsvm C API** (~20 symbols: `svm_predict`, `svm_train`, `svm_load_model`, …):
  high-severity interposition risk — any downstream binary that also links libsvm
  has both definitions in its dynamic symbol table; the dynamic linker silently
  resolves to whichever definition it finds first.
- **libsvm C++ internals** (~27 mangled symbols: `_ZN5Cache*`, `_ZN6Solver*`, …):
  moderate interposition risk with any other libsvm-linked code.
- **pdjson JSON parser** (~20 symbols: `json_open_buffer`, `json_next`, …):
  collision risk with any embedded JSON parser in the consumer's process.
- **SIMD kernel functions** (~120: `adm_cm_avx2`, `vif_statistic_8_avx512`, …):
  low collision probability but unnecessary ABI surface that prevents renaming
  without a major-version bump.
- **Internal helpers** (~15: `aligned_malloc`, `aligned_free`, `mkdirp`,
  `picture_copy`, `_cmp_float`): `aligned_malloc`/`aligned_free` collide with
  Windows CRT names.

The root cause is that the build compiled all TUs without `-fvisibility=hidden`.
The linker flag `-Wl,--exclude-libs,ALL` only hides symbols that originate from
statically linked *archives* passed on the link line; objects extracted via
`extract_all_objects()` (the SIMD sub-libraries, libsvm, pdjson) are treated as
first-party objects and are not excluded.

## Decision

Apply `-fvisibility=hidden` to `vmaf_cflags_common` in `core/src/meson.build`,
making all symbols hidden by default. Introduce a `VMAF_EXPORT` macro in
`core/include/libvmaf/macros.h` that maps to
`__attribute__((visibility("default")))` on GCC/Clang and
`__declspec(dllexport)` on MSVC. Annotate every public `vmaf_*` function
declaration in `core/include/libvmaf/*.h` with `VMAF_EXPORT`. The result is
that the dynamic symbol table of `libvmaf.so.3` contains only the 44 intentional
public-API symbols, down from 207 + 44 = 251.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Option A — `-fvisibility=hidden` + `VMAF_EXPORT` (chosen)** | Clean: the attribute is on the declaration, visible to consumers; standard GCC/Clang practice; no ELF versioning side-effects | Requires annotating ~60-80 public declarations | Best practice; produces smallest, cleanest DSO |
| **Option B — GNU version script (`libvmaf.map`)** | No source annotation required; `vmaf_*` glob catches all current public symbols | Adds ELF `SYMVER` versioning which complicates static-link consumers; glob misses non-`vmaf_`-prefixed public symbols added in future | Not chosen because it adds versioning complexity without a clear benefit at this stage |
| **Option C — Version script now, attributes in next major** | Ships the fix immediately with minimal source changes | Defers the annotation work, leaving header consumers without the `VMAF_EXPORT` attribute they need for their own `-fvisibility=hidden` builds; the versioning baggage is permanent | Not chosen — the annotation pass is mechanical and the attribute benefits header consumers |

## Consequences

- **Positive**: Eliminates silent symbol interposition risk from libsvm, pdjson,
  and all internal helpers. Downstream binaries that link both libvmaf and libsvm
  no longer experience silent mis-dispatch. Reduces `nm -D` output from ~251
  symbols to 44. The public API surface is now machine-verifiable with a single
  `nm -D | grep -v vmaf_` command (target: 0 lines).
- **Positive**: Downstream consumers that build their own code with
  `-fvisibility=hidden` now benefit from `VMAF_EXPORT` on `#include
  <libvmaf/libvmaf.h>` declarations — no more manual visibility overrides needed.
- **Negative**: Any code that resolved internal symbols by name at link time
  (unlikely in practice) will break. All such symbols were non-public.
- **Negative (rebase)**: Upstream Netflix/vmaf does not use `-fvisibility=hidden`;
  future upstream merges that add new public entry points in `libvmaf.c` /
  `libvmaf.h` must also receive `VMAF_EXPORT` annotations before the fork can
  merge them. See `docs/rebase-notes.md` for the rebase invariant.
- **Neutral**: The `vmaf_cppflags_common` C++ equivalent (`-fvisibility-inlines-hidden`)
  was not added because no public C++ API surface exists in the fork today. If
  C++ headers are added to the public surface in a future PR, that flag should be
  added then.
- **Follow-up**: A CI gate verifying `nm -D | grep -v vmaf_ | wc -l` equals 0
  should be added to prevent future leaks. Tracked as a follow-up item.

## References

- Research-0092 (`docs/research/0092-round4-symbol-visibility-audit.md`) — full
  symbol audit with per-category counts and root cause analysis.
- GCC manual: `-fvisibility=hidden` and `__attribute__((visibility("default")))`.
- Drepper, "How To Write Shared Libraries", §2.2 — ELF symbol versioning and
  visibility best practices.
- [ADR-0374](0374-disabled-build-enosys-contract.md) — `-ENOSYS` contract for
  build-time-optional APIs; all public symbols remain present in the DSO
  regardless of feature flags.
- Per user direction: implement Research-0092 Option A (recommended fix).

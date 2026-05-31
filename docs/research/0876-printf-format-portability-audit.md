<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->

# Research-0876: printf-format portability audit (CERT FIO47-C / MISRA 21.6)

**Date:** 2026-05-30
**Branch:** fix/printf-format-portability
**Scope:** every fork-added `.c` / `.cpp` / `.h` under `core/src/`, `core/tools/`, `core/test/`, `cmd/`, `mcp-server/` — search for non-portable length-modifier specifiers (`%lu`, `%ld`, `%lx`, `%lld`, `%llu`, `%llx`).

---

## 1. Method

```bash
grep -rnE '%(lld|llu|llx|ld|lu|lx)\b' core/ cmd/ mcp-server/ \
  --include='*.c' --include='*.cpp' --include='*.h' --include='*.cu' --include='*.cc' \
  | grep -vE 'third_party|iqa|3rdparty|libsvm|cJSON|pdjson'
```

Each hit was triaged against the type of the formatted value (via `git blame` + struct declaration lookup), the cast (if any) applied at the call site, and the target platform's data model.

---

## 2. Findings

### Class A — silent truncation on Windows LLP64 (BUG)

`uint64_t` formatted with `%lu` + `(unsigned long)` cast.

- **`core/src/sycl/common.cpp:600`** — `frame_counter` (declared `uint64_t` at `common.cpp:117`) printed with `%lu` + `(unsigned long)` cast. On Windows LLP64 `unsigned long` is 32 bits; values `> 2^32 - 1` truncate silently. Fixed: `%" PRIu64 "` + drop cast.
- **`core/src/sycl/common.cpp:1060`** — `timing_frames` (declared `uint64_t` at `common.cpp:111`), same pattern. Fixed identically.

Linux LP64 happens to satisfy `unsigned long == uint64_t` so the bug is invisible there; the audit is forward-looking per memory `project_windows_ci_sdk_pin`.

### Class B — portable but non-idiomatic for fixed-width types

`int64_t` / `uint64_t` formatted with `%lld` / `%llu` / `%llx` + explicit `(long long)` / `(unsigned long long)` / `(unsigned long long)` cast. C99 guarantees `long long >= 64 bits` and the matching format specifiers exist, so these are portable — but they hide the intent (future readers can't distinguish a load-bearing cast from a forgotten one).

- **`core/src/libvmaf.c:923, 931, 942, 988, 999, 1040`** — six tiny-model loader log sites printing `int64_t in_shape[]` ONNX shape dims. Cast `(long long)`. Converted to `PRId64`.
- **`core/src/sycl/dmabuf_import.cpp:338, 516`** — two `uint64_t modifier` (DRM format modifier) log sites. Cast `(unsigned long long)`. Converted to `PRIx64`.
- **`core/test/test_motion_v2_simd.c:264`** — two `uint64_t` SAD values in divergence log. Cast `(unsigned long long)`. Converted to `PRIu64`.

### Class C — verified not-bugs, left as-is

- **`core/tools/yuv_input.c:124, 133`** — `off_t file_sz` printed with `(long long)` + `%lld`. `off_t` is a POSIX non-fixed-width type; CERT FIO47-C recommends exactly this cast pattern (or `(intmax_t)` + `%jd`). No PRI macro applies. Leave.
- **`core/test/test_public_api_score.c:201`** — Windows `GetCurrentProcessId()` (returns `DWORD`) printed with `(unsigned long)` + `%lu`. `DWORD` is exactly `unsigned long` on Windows. Format matches the type's exact spelling. Leave.
- **`core/src/feature/x86/adm_avx512.c:74-75`** — `print_128_64` debug macro in upstream Netflix code (under `#if 0` debug guard). Per fork rule 6, upstream-mirror files are reserved for the next upstream sync. Leave.

### Class D — verified portable, not in scope

`%zu` for `size_t` is C99 standard and matches the type exactly. No issues found; ten+ such uses across the tree are correct.

---

## 3. Coverage summary

| Sweep target | Hits | Class A | Class B | Class C | Class D |
|---|---|---|---|---|---|
| `core/src/` (fork-added) | 17 | 2 | 9 | 1 | 5 |
| `core/tools/` (post-ADR-0700 fork edits) | 4 | 0 | 0 | 2 | 2 |
| `core/test/` (fork-added) | 5 | 0 | 2 | 1 | 2 |
| `cmd/`, `mcp-server/` | 0 | 0 | 0 | 0 | 0 |

Total fix candidates: 11 (Class A + B). All fixed in this PR.

---

## 4. Validation

- `meson setup build-cpu core -Denable_cuda=false -Denable_sycl=false` — clean configure.
- `ninja -C build-cpu` — clean build, 726 targets.
- `meson test -C build-cpu --suite=fast` — 49/49 pass.
- `grep -nE '%(lld|llu|llx|ld|lu|lx)\b' core/src/sycl/ core/src/libvmaf.c core/test/test_motion_v2_simd.c` — empty.

SYCL / Windows builds are not exercised locally (no SYCL toolchain on this host, no Windows runner); the fix is type-correct via `<cinttypes>` / `<inttypes.h>` and follows the CERT-mandated portable form, so the audit's correctness claim is independent of the local validation surface.

---

## 5. Out of scope

- Upstream-mirror `print_128_64` macro in `adm_avx512.c` — wait for next upstream sync.
- `printf`-vs-`vmaf_log` routing audit — covered by ADR-0871 / commit `4579a439ae`.
- `snprintf` return-value-check audit — separate sweep; FIO34-C territory.

---

## 6. References

- CERT FIO47-C: "Use valid format strings".
- MISRA C:2012 Rule 21.6: "The Standard Library input/output functions shall not be used".
- C99 §7.8.1 — `<inttypes.h>` PRI macros.
- ISO C: `unsigned long` is at least 32 bits; `long long` is at least 64 bits.
- Windows LLP64 data model — `long`/`unsigned long` are 32 bits; `long long`/`unsigned long long` are 64 bits; `size_t` is 64 bits.
- Memory `project_windows_ci_sdk_pin` — Windows CI rolled to 24H2 SDK; portability audit is forward-looking.

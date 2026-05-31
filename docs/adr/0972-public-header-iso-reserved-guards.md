<!-- markdownlint-disable MD013 MD060 -->
# ADR-0972: Public headers — replace ISO-reserved `__VMAF_*__` include guards with `LIBVMAF_*_H` (Round 27 audit A.1, SEI CERT DCL37-C)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `api`, `headers`, `cert-c`, `lint`, `compatibility`

## Context

The C standard (C11 §7.1.3, C17 §7.1.3, C23 §7.1.3) reserves every
identifier that begins with two underscores or with one underscore
followed by an uppercase letter "for any use" by the implementation.
SEI CERT [DCL37-C](https://wiki.sei.cmu.edu/confluence/display/c/DCL37-C.+Do+not+declare+or+define+a+reserved+identifier)
ratifies this: user code (including library headers consumed by users)
must not declare or define such identifiers.

The fork's nine public C-API headers under `core/include/libvmaf/`
shipped with include guards in the form `__VMAF_<NAME>_H__`:

```c
#ifndef __VMAF_PICTURE_H__
#define __VMAF_PICTURE_H__
...
#endif /* __VMAF_PICTURE_H__ */
```

This pattern is doubly reserved (leading `__` plus a name starting with
an uppercase letter after the first underscore). It triggers
`-Wreserved-identifier` / `-Wreserved-macro-identifier` on clang ≥ 13,
and clang 22 (current in the dev-mcp container) hard-errors when paired
with `-Werror`. Downstream consumers compiling with strict-mode flags
inherit the warning through `#include <libvmaf/...>`.

The matching `LIBVMAF_*_H_` pattern was already used by the three
GPU-backend headers added under this fork (`libvmaf_hip.h`,
`libvmaf_metal.h`, `libvmaf_mcp.h`), so the project precedent was
already split. This ADR resolves the split by standardising on a single
ISO-compliant pattern.

## Decision

Every public header under `core/include/libvmaf/` uses the
`LIBVMAF_<BASENAME>_H` include-guard pattern. The transformation is
mechanical:

| File                | Old guard                  | New guard                   |
|---------------------|----------------------------|-----------------------------|
| `libvmaf.h`         | `__VMAF_H__`               | `LIBVMAF_LIBVMAF_H`         |
| `picture.h`         | `__VMAF_PICTURE_H__`       | `LIBVMAF_PICTURE_H`         |
| `feature.h`         | `__VMAF_FEATURE_H__`       | `LIBVMAF_FEATURE_H`         |
| `model.h`           | `__VMAF_MODEL_H__`         | `LIBVMAF_MODEL_H`           |
| `macros.h`          | `__VMAF_MACROS_H__`        | `LIBVMAF_MACROS_H`          |
| `vmaf_assert.h`     | `__VMAF_ASSERT_H__`        | `LIBVMAF_VMAF_ASSERT_H`     |
| `dnn.h`             | `__VMAF_DNN_H__`           | `LIBVMAF_DNN_H`             |
| `libvmaf_cuda.h`    | `__VMAF_CUDA_H__`          | `LIBVMAF_LIBVMAF_CUDA_H`    |
| `libvmaf_sycl.h`    | `__VMAF_LIBVMAF_SYCL_H__`  | `LIBVMAF_LIBVMAF_SYCL_H`    |

The pre-existing `LIBVMAF_HIP_H_`, `LIBVMAF_METAL_H_`, and
`LIBVMAF_MCP_H_` guards in the other three public headers are left
alone — they are already ISO-compliant; harmonising the trailing
underscore is a cosmetic-only follow-up not worth touching ABI-adjacent
files for.

Scope is strictly the public surface (`core/include/libvmaf/`). The 29
internal `core/src/**/*.h` headers that share the same legacy pattern
are out of scope for this PR (they are not visible to downstream
consumers; SEI CERT DCL37-C still applies, but the warning surface is
internal-only and a separate cleanup PR sequence can address them
without ABI risk).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **`LIBVMAF_<BASENAME>_H` (chosen)** | ISO-compliant; matches existing fork-added HIP/Metal/MCP precedent; mechanical rename, zero ABI delta | The full guard `LIBVMAF_LIBVMAF_H` for `libvmaf.h` is mildly redundant | Convention beats elegance; the doubled-name form is the textbook `<PROJECT>_<FILENAME>_H` pattern and shows up unchanged in projects like LLVM, libuv, libpng |
| `VMAF_<NAME>_H` | Shorter; matches the public-symbol prefix (`vmaf_*`) | Risk of collision with caller macros that happen to define `VMAF_*` (no project prefix); diverges from the existing `LIBVMAF_HIP_H_` precedent on three sibling headers | Adopting a third pattern would worsen, not heal, the split |
| `#pragma once` | One-line per header; no guard symbol to maintain | Not in any ISO C standard; while every shipping compiler we target accepts it, fork policy is "ISO C with documented extensions only"; also breaks `gcc -ansi -pedantic` smoke checks downstream consumers may run | Standards posture |
| Leave it alone | Zero change | Hard-errors on clang `-Wreserved-identifier -Werror` builds (demonstrated reproducer in PR body); fails SEI CERT DCL37-C; surfaces in any downstream lint run | The failure mode is real and growing |
| Suppress the warning project-wide | Hides the immediate lint failure | Suppression is not portable; downstream consumers re-enabling the warning still inherit the violation; treats a symptom, not the cause | Standards compliance is the right primitive |

## Consequences

- **Positive**: Public headers compile clean under
  `clang -Wreserved-identifier -Werror` (demonstrated in PR body);
  SEI CERT DCL37-C compliance for the public surface; the three
  ISO-compliant fork-added headers (`libvmaf_hip.h`, `libvmaf_metal.h`,
  `libvmaf_mcp.h`) are now consistent with the rest of the directory.
- **Negative**: Future upstream syncs that touch
  `core/include/libvmaf/libvmaf.h`, `picture.h`, `feature.h`,
  `model.h`, `libvmaf_cuda.h`, or `dnn.h` will diff on the guard lines.
  These are stable headers (rare upstream churn) and the merge driver
  resolves trivially — keep our `LIBVMAF_*_H` lines, drop upstream's
  `__VMAF_*__` ones. `macros.h`, `vmaf_assert.h`, and `libvmaf_sycl.h`
  are fork-only; no upstream divergence risk.
- **Neutral / follow-ups**: 29 internal `core/src/**/*.h` headers
  carry the same legacy pattern. Out of scope here (no downstream
  visibility) but worth a follow-up sweep. Tracked but not blocking
  this PR.

## References

- SEI CERT [DCL37-C: Do not declare or define a reserved identifier](https://wiki.sei.cmu.edu/confluence/display/c/DCL37-C.+Do+not+declare+or+define+a+reserved+identifier).
- C17 §7.1.3 ("Reserved identifiers").
- LLVM `-Wreserved-identifier` documentation:
  <https://clang.llvm.org/docs/DiagnosticsReference.html#wreserved-identifier>.
- [Research-0762: Public-header ISO-reserved guards — Round 27 audit (2026-05-31)](../research/0762-public-header-iso-reserved-guards-2026-05-31.md).
- Source: `req` — Round 27 bug A.1 audit finding ("every public header
  in `core/include/libvmaf/` uses ISO-reserved `__VMAF_*__` include
  guards. This violates SEI CERT DCL37-C").

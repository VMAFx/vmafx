<!-- markdownlint-disable MD013 MD060 -->
# Research-0762: Public-header ISO-reserved include-guards — Round 27 audit A.1 (2026-05-31)

**Date:** 2026-05-31
**Operator:** lusoris
**Branch:** `fix/public-header-iso-reserved-guards`
**Status:** COMPLETE — fix shipped in ADR-0972 / PR

---

## 1. Purpose

Audit and remediate ISO-reserved include-guard identifiers in the
fork's public C-API headers under `core/include/libvmaf/`. Background
and full rationale are in
[ADR-0972](../adr/0972-public-header-iso-reserved-guards.md); this
digest captures the evidence — what the violating pattern is, what the
compiler does about it, the full enumeration of touched files, and the
verification commands used to prove no source-file companions are
needed.

---

## 2. The violation

C11 / C17 / C23 §7.1.3 ("Reserved identifiers") reserves all
identifiers that:

1. Begin with two underscores (`__foo`); **or**
2. Begin with one underscore followed by an uppercase letter (`_F…`).

Such identifiers are reserved "for any use" by the implementation in
**both** file and block scope, and the standard does **not** require a
diagnostic when user code defines them — i.e. silent UB.

SEI CERT [DCL37-C](https://wiki.sei.cmu.edu/confluence/display/c/DCL37-C.+Do+not+declare+or+define+a+reserved+identifier)
formalises the rule as a project-wide invariant. Include guards
matching `__<PROJECT>_<NAME>_H__` violate both clauses (leading `__`,
and the first character after the first underscore is uppercase),
which is the maximum-strength form of the violation.

---

## 3. Enumeration — touched files

`grep -rln '#ifndef __VMAF_' core/include/libvmaf/` on `origin/master`
@ `4948b771c`:

| File | Old guard | New guard |
|---|---|---|
| `libvmaf.h`         | `__VMAF_H__`               | `LIBVMAF_LIBVMAF_H`         |
| `picture.h`         | `__VMAF_PICTURE_H__`       | `LIBVMAF_PICTURE_H`         |
| `feature.h`         | `__VMAF_FEATURE_H__`       | `LIBVMAF_FEATURE_H`         |
| `model.h`           | `__VMAF_MODEL_H__`         | `LIBVMAF_MODEL_H`           |
| `macros.h`          | `__VMAF_MACROS_H__`        | `LIBVMAF_MACROS_H`          |
| `vmaf_assert.h`     | `__VMAF_ASSERT_H__`        | `LIBVMAF_VMAF_ASSERT_H`     |
| `dnn.h`             | `__VMAF_DNN_H__`           | `LIBVMAF_DNN_H`             |
| `libvmaf_cuda.h`    | `__VMAF_CUDA_H__`          | `LIBVMAF_LIBVMAF_CUDA_H`    |
| `libvmaf_sycl.h`    | `__VMAF_LIBVMAF_SYCL_H__`  | `LIBVMAF_LIBVMAF_SYCL_H`    |

Three sibling headers already shipped ISO-compliant guards:
`libvmaf_hip.h` (`LIBVMAF_HIP_H_`), `libvmaf_metal.h`
(`LIBVMAF_METAL_H_`), `libvmaf_mcp.h` (`LIBVMAF_MCP_H_`). Left
untouched — they are already compliant.

---

## 4. Reproducer — clang 22 hard-errors

A minimal isolated reproducer with one before- and one after-header:

```sh
cat > before.h <<'EOF'
#ifndef __VMAF_DEMO_H__
#define __VMAF_DEMO_H__
int demo(void);
#endif
EOF
cat > after.h <<'EOF'
#ifndef LIBVMAF_DEMO_H
#define LIBVMAF_DEMO_H
int demo(void);
#endif
EOF
cat > main.c <<'EOF'
#include "before.h"
int main(void){return 0;}
EOF
clang -Wreserved-identifier -Werror -fsyntax-only main.c
```

**Result (clang 22.1.6 on Linux x86_64):**

```text
In file included from main.c:1:
./before.h:2:9: error: macro name is a reserved identifier
                       [-Werror,-Wreserved-macro-identifier]
    2 | #define __VMAF_DEMO_H__
      |         ^
1 error generated.
```

Swapping `#include "before.h"` → `#include "after.h"` and re-running
yields **no diagnostic** and exit 0.

---

## 5. Scope verification — no source-file companions

To rule out the possibility that any `.c` / `.cpp` / `.cu` file
`#define`s, `#undef`s, or otherwise depends on one of the nine
guard-macro symbols as an ABI surface (which would make a guard rename
a real ABI break, not a cosmetic one):

```sh
grep -rn '__VMAF_H__\|__VMAF_PICTURE_H__\|__VMAF_FEATURE_H__\
|__VMAF_MODEL_H__\|__VMAF_MACROS_H__\|__VMAF_ASSERT_H__\
|__VMAF_DNN_H__\|__VMAF_CUDA_H__\|__VMAF_LIBVMAF_SYCL_H__' \
  core/src/ core/tools/ core/test/ compat/ python/ ai/ mcp-server/ \
  --include='*.c' --include='*.cpp' --include='*.cu' --include='*.cc'
```

**Result:** zero matches. No source file consumes any of the old guard
symbols. Rename is purely cosmetic at the source-file level; ABI is
unchanged.

A broader sweep including all file extensions confirmed the same: no
reference outside `core/include/libvmaf/` itself.

---

## 6. Internal-header `__VMAF_*__` exposure (out of scope)

29 internal headers under `core/src/**/*.h` use the same legacy
pattern (`__VMAF_THREAD_POOL_H__`, `__VMAF_SRC_*_H__`, etc.). These
are not part of the public API and are not visible to downstream
consumers, so they don't trigger the cross-boundary lint failure that
motivates this PR. SEI CERT DCL37-C still applies and a follow-up
sweep should clean them up, but they are intentionally **out of scope**
for this fix to keep the diff laser-focused on the public-API surface.

Tracked for follow-up; not blocking.

---

## 7. Verification — build + header-only syntax check

### 7.1 Full CPU build

```sh
meson setup core/build-cpu core -Denable_cuda=false -Denable_sycl=false
ninja -C core/build-cpu
```

Result: 726 targets linked clean (no warnings related to the guard
rename, no link errors).

### 7.2 Header-only isolated compile sanity

```sh
for h in core/include/libvmaf/*.h; do
  gcc -x c -Icore/include -fsyntax-only -c "$h"
done
```

Result: zero errors across all 12 public headers (the 9 renamed +
the 3 pre-compliant).

---

## 8. Outcome

ADR-0972 ships the rename. The PR carries this digest as the audit
trail, the ADR as the decision record, the `AGENTS.md` invariant note
to lock in the pattern for future contributions, and the changelog
fragment for the next release.

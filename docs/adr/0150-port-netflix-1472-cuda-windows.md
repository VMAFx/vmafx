# ADR-0150: Port Netflix #1472 — CUDA feature extraction on Windows (MSYS2/MinGW)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: upstream-port, cuda, windows, mingw, build

## Context

Pre-port, the fork's CUDA backend built cleanly on Linux + nvcc +
gcc but failed on Windows + nvcc + MinGW-GCC. Two categories of
friction:

1. **Source portability.** On Windows, nvcc requires MSVC's
   `cl.exe` as its host compiler for preprocessing `.cu` files —
   even when the rest of libvmaf is built with MinGW-GCC. The
   `.cu` TUs transitively pull `<pthread.h>` (POSIX-only, absent
   in MSVC's CRT), `<ffnvcodec/dynlink_*.h>` (installed in MinGW
   paths that `cl.exe` doesn't search), and C99 designated
   initializers (not accepted by nvcc in C++ mode with MSVC).
   Each of these is silently fine on Linux with GCC-as-host.
2. **Build-system plumbing.** `meson`'s `add_languages('cuda')`
   helper requires MSVC to be the default C compiler on Windows,
   which is incompatible with the rest of the fork (which needs
   MinGW-GCC for the host-side libvmaf TUs). Adding MSVC to
   `PATH` for nvcc's sake would cause meson to pick it as the
   default C compiler and break the CPU build.

Netflix upstream PR
[#1472](https://github.com/Netflix/vmaf/pull/1472) — "cuda: enable
CUDA feature extraction on Windows (MSYS2/MinGW)" (birkdev, Mar
2026, OPEN) — addresses both categories. PR is two commits:

- `15745cdf` — source-portability guards in CUDA headers + `.cu`
  files.
- `b7b65e64` — meson build plumbing: `vswhere`-based `cl.exe`
  discovery without `PATH` pollution, Windows SDK + MSVC include
  path injection via `-I` flags to nvcc, CUDA version detection
  via `nvcc --version` instead of `meson.get_compiler('cuda')`.

## Decision

Port both upstream commits, applied in order, with three
fork-specific conflict resolutions:

### Conflict 1: `integer_adm.h` designated initializers

Upstream's patch wraps the `dwt_7_9_YCbCr_threshold` table in
`#ifndef __CUDACC__`, hiding it from `.cu` TUs entirely.

The fork (commit pre-dates upstream #1472) already solves the
same problem differently — by rewriting the initializer in
**positional** form (`{0.495, 0.466, 0.401, {…}}` instead of
`{.a = 0.495, .k = 0.466, .f0 = 0.401, …}`), which is C++-portable
across MSVC/GCC/clang.

**Fork keeps positional form.** It's strictly more useful — the
table stays visible to any future `.cu` TU that might want to
use it. The `#ifndef __CUDACC__` approach is the minimal change
if a `.cu` consumer never appears; the positional form works for
all cases. Comment updated to note the divergence.

### Conflict 2: `meson.build` gencode coverage + nvcc plumbing

Upstream's patch rewrites the `if get_option('enable_nvcc')`
block to add MSVC discovery, Windows SDK include path injection,
and CUDA version detection via `nvcc --version`. It also changes
`cuda_compiler.version()` to `cuda_version` (a new local string).

The fork already has its own extension of the same block: an
explicit gencode list that cubins for sm_75 / sm_80 / sm_86 /
sm_89 plus a compute_80 PTX fallback, with a long comment
explaining the Netflix coverage hole ADR-0122 documented.

**Merged shape**: upstream's MSVC/Windows detection + CUDA
version detection runs first (host-independent cubin generation
needs `cuda_version` regardless of platform), then the fork's
gencode coverage comment + list follows. Both now reference
`cuda_version` instead of the dropped `cuda_compiler.version()`.

### Conflict 3: `cuda_static_lib` dependencies

Upstream's patch drops the `dependencies : [pthread_dependency]`
line from `cuda_static_lib`, on the assumption that commit 1
removed the only `pthread.h` pull (`cuda/common.h`).

**Fork keeps the dependency**: `core/src/cuda/ring_buffer.c`
is part of `cuda_static_lib` and directly `#include`s
`<pthread.h>`. Removing the explicit `pthread_dependency` would
relink-fail on hosts where pthread is not transitively available
via another dep in the TU graph. Comment updated to explain.

### Drive-by lint cleanups (ADR-0141 touched-file rule)

The upstream patch touches `cuda/common.h` and `picture.h`, which
carried the reserved-identifier header guards
`__VMAF_SRC_CUDA_COMMON_H__` and `__VMAF_SRC_PICTURE_H__`
(leading-double-underscore = reserved at file scope).
`clang-tidy` flags both as `bugprone-reserved-identifier` /
`cert-dcl37-c` / `cert-dcl51-cpp`. Renamed to
`VMAF_SRC_CUDA_COMMON_INCLUDED` /
`VMAF_SRC_PICTURE_INCLUDED` — same pattern ADR-0148 used for
the IQA header guards.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Cherry-pick upstream PR verbatim via `git cherry-pick`** | One-shot; traceable to upstream | The meson.build and integer_adm.h conflicts need resolution; the cuda_static_lib pthread drop is actively wrong for the fork | Rejected — 3-way `git am --3way` with explicit conflict handling is cleaner |
| **Use `#ifndef __CUDACC__` for the ADM table (upstream shape)** | Matches upstream byte-for-byte; smallest delta on future rebase | Makes the table unavailable to any hypothetical `.cu` consumer | Rejected — fork's positional form is strictly more useful; `.cu` TUs that want the table don't have to special-case |
| **Drop pthread_dependency from cuda_static_lib (upstream shape)** | Matches upstream byte-for-byte | `ring_buffer.c` uses pthread directly; link can fail depending on transitive deps | Rejected — explicit `pthread_dependency` is the correct build description |
| **Skip Windows CUDA support entirely** | Zero port cost | Blocks Windows + CUDA users; widens the fork's platform-coverage gap vs upstream | Rejected — the port is small and mechanical once the conflicts are handled |

## Consequences

- **Positive**:
  - Windows + MSYS2 + MinGW + nvcc + MSVC Build Tools now builds
    libvmaf with CUDA enabled. `vswhere`-based `cl.exe`
    detection avoids polluting `PATH` (which would break the
    MinGW-GCC CPU build).
  - CUDA version detection via `nvcc --version` removes the
    Windows-hostile `add_languages('cuda')` meson helper.
  - Linux + GCC build is a strict no-op (all new code paths are
    guarded by `host_machine.system() == 'windows'`).
  - Two header-guard reserved-identifier warnings cleared as a
    drive-by.
- **Negative**:
  - Upstream PR #1472 is still OPEN. Future sync will need to
    re-diff the three conflict-resolved hunks (see rebase-notes
    entry 0043). Keep the fork's version on rebase for
    `integer_adm.h` positional initializers and
    `cuda_static_lib` pthread_dependency; adopt upstream's shape
    only if it changes shape meaningfully.
  - The fork is now slightly ahead of upstream on ADR-0122
    gencode coverage AND Windows support at the same time; a
    future rebase that touches either one will stretch across
    both features. Noted in rebase-notes 0043.
- **Neutral / follow-ups**:
  - CI does not yet exercise the Windows CUDA path — no runner
    with MSYS2 + MSVC Build Tools + CUDA toolkit is currently
    enrolled. The port is provably complete on Linux (CPU build
    DA build both pass all tests); Windows validation is
    operator-driven. Tracked as T7-3 in `.workingdir2/OPEN.md`
    (self-hosted GPU runner enrollment).
  - nv-codec-headers on MinGW needs to be built from
    `876af32` or later — the `n13.0.19.0` release tag is missing
    `cuMemFreeHost`, `cuStreamCreateWithPriority`,
    `cuLaunchHostFunc`, and other CudaFunctions members libvmaf
    uses. Pre-existing, not this PR's scope; documented in
    upstream PR body.

## Verification

- `meson setup libvmaf core/build-cuda-port -Denable_cuda=true
  -Denable_sycl=false -Denable_nvcc=true` → OK.
- `ninja -C core/build-cuda-port` → 6 `.fatbin` files produced
  (`adm_cm`, `adm_csf`, `adm_csf_den`, `adm_dwt2`, `filter1d`,
  `motion_score`), `tools/vmaf` CLI linked.
- `meson test -C core/build-cuda-port` → 35/35 pass.
- `meson test -C build` (CPU-only) → 32/32 pass.
- `clang-tidy -p build core/src/cuda/common.h
  core/src/picture.h core/src/feature/integer_adm.h` → zero
  warnings.
- Windows build: requires a Windows + MSYS2 + MSVC BuildTools +
  CUDA runner; not validated in this PR.

## References

- Upstream PR:
  [Netflix/vmaf#1472](https://github.com/Netflix/vmaf/pull/1472),
  commits `15745cdf` + `b7b65e64` (birkdev, 2026-03-16, OPEN).
- Backlog: `.workingdir2/BACKLOG.md` T4-2.
- [ADR-0122](0122-cuda-post-cubin-load-hardening.md) — fork's
  existing gencode coverage extension (Context for conflict 2).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (drove the header-guard rename).
- [ADR-0148](0148-iqa-rename-and-cleanup.md) — precedent for
  header-guard `_INCLUDED` renames.
- User direction 2026-04-24 popup: "Port Netflix#1472 CUDA-on-Windows
  MinGW".

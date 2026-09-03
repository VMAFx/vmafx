<!-- markdownlint-disable MD013 MD060 -->
# Research 1166 — upstream Netflix/vmaf issue harvest, 2026-09-03

Companion to [ADR-1166](../adr/1166-upstream-issue-harvest.md).

This is the durable output of the harvest: every candidate that was looked at,
its verdict, and the evidence behind it — **including the negative verdicts**.
A future session that wonders "does Netflix/vmaf#N affect us?" should read this
table before re-investigating.

Fork layout reminder: upstream's `libvmaf/src/...` maps to `core/src/...`
(ADR-0700), several C files became C++ twins, and CUDA / SYCL / HIP / Metal
backends are fork-added. An upstream line number is never directly usable.

## Verdict vocabulary

| Verdict | Meaning |
|---|---|
| `ALREADY-FIXED` | The defect existed here and was fixed before this harvest. Nothing to do. |
| `NOT-APPLICABLE` | The defect never existed here, or the fork solved the same problem a different way. |
| `AFFECTS-FORK` | Present in the fork's tree today. Either fixed in this batch or given a `docs/state.md` row. |
| `NEEDS-HARDWARE` | Present, but a live reproduction or the parity run needs silicon this workstation does not have. |

## Summary table

| Upstream | Severity | Verdict | Fixed here? | Evidence anchor |
|---|---|---|---|---|
| [#1582](https://github.com/Netflix/vmaf/issues/1582) | high | AFFECTS-FORK | **yes** | `core/src/feature/common/convolution_internal.h`, `convolution.c` |
| [#1581](https://github.com/Netflix/vmaf/issues/1581) | medium | AFFECTS-FORK | **yes** (chroma path) | `core/src/feature/float_motion.c` |
| [#1178](https://github.com/Netflix/vmaf/issues/1178) | medium | AFFECTS-FORK | **yes** | `core/src/meson.build` `libvmaf_private_libs` |
| [#1551](https://github.com/Netflix/vmaf/issues/1551) | medium | AFFECTS-FORK | **yes** | `core/src/feature/compat_builtin.h` |
| [#1422](https://github.com/Netflix/vmaf/issues/1422) | low | mixed | **yes** (1 of 3 hunks) | same header; other two hunks dead |
| [#1573](https://github.com/Netflix/vmaf/issues/1573) | low-medium | mixed | **yes** (2 of 3 hunks) | `core/src/meson.build`, `core/tools/test/meson.build` |
| [#1242](https://github.com/Netflix/vmaf/issues/1242) | low-medium | AFFECTS-FORK | **yes** | `core/src/model.c`, the three public headers |
| [#743](https://github.com/Netflix/vmaf/issues/743) | low | AFFECTS-FORK | **yes** | `core/tools/spinner.h`, `core/tools/vmaf.cpp` |
| [#1580](https://github.com/Netflix/vmaf/issues/1580) | low | mostly ALREADY-FIXED | **yes** (Metal residual) | `core/src/feature/metal/*_motion_metal.mm` |
| [#1564](https://github.com/Netflix/vmaf/issues/1564) | medium | AFFECTS-FORK | no — deferred | `core/src/feature/cuda/integer_adm/adm_cm.cu` |
| [#930](https://github.com/Netflix/vmaf/issues/930) | low-medium | AFFECTS-FORK | no — deferred | `core/src/feature/cuda/integer_adm/adm_decouple_inline.cuh` |
| [#1568](https://github.com/Netflix/vmaf/issues/1568) | medium | AFFECTS-FORK | no — deferred | `core/src/libvmaf.c` `output_file_open` |
| [#1109](https://github.com/Netflix/vmaf/issues/1109) | medium | AFFECTS-FORK | no — deferred | `core/src/feature/integer_psnr.c` |
| [#766](https://github.com/Netflix/vmaf/issues/766) | medium | AFFECTS-FORK | no — deferred | `core/tools/cli_parse.cpp` |
| [#818](https://github.com/Netflix/vmaf/issues/818) | low | AFFECTS-FORK | no — deferred | `core/include/libvmaf/libvmaf.h` pooling enum |
| [#1305](https://github.com/Netflix/vmaf/issues/1305) | medium | AFFECTS-FORK | no — deferred | `core/src/cuda/drain_batch.c` |
| [#1494](https://github.com/Netflix/vmaf/issues/1494) | medium | partly NOT-APPLICABLE | no — deferred | `core/src/feature/integer_adm.c` `i_rfactor` |

## Fixed in this batch

### Netflix/vmaf#1582 — single-bounce mirror + negative border bound

Two distinct defects in the float convolution, both carried verbatim.

**Defect 1 — single-bounce mirror.** `convolution_edge_s`, `_sq_s` and `_xy_s`
in `core/src/feature/common/convolution_internal.h` each open-coded

```c
if (j_tap < 0)            j_tap = -j_tap;
else if (j_tap >= width)  j_tap = width - (j_tap - width + 2);   /* == 2*w - j - 2 */
```

One bounce only lands in range when the plane is at least `radius + 1` across.
At `size == 2`, a tap of `-2` folds to `+2` (still `>= size`) and a tap of `+3`
folds to `-1`. There is no `convolution_mirror()` helper in the fork
(`grep -rn convolution_mirror` → 0 hits before this PR).

**Defect 2 — negative border bound.** `convolution.c` derived
`borders_right = vmaf_floorn(width - (filter_width - radius), step)`, which is
negative whenever the plane is narrower than the filter. The trailing loop then
started at a negative index and wrote `dst[i * dst_stride - 1]` — a heap
underflow **write**. `borders_left = vmaf_ceiln(radius, step)` can likewise
exceed the width, walking the leading loop past the row.

**Reachability.** Not latent. Two live CPU paths reach the defective sizes with
supported input and no non-default option beyond the one named:

- `--feature float_vif` on any frame in 9..15 px. `float_vif`'s own guard
  admitted `>= 9`, but `compute_vif()` runs a four-scale ladder that halves the
  working dimension per scale and re-convolves with that scale's Gaussian
  ({17, 9, 5, 3} taps at the default kernelscale). The binding constraint is
  scale 3: `w0 / 8 >= 2`, i.e. `w0 >= 16`. ASan reports a
  heap-buffer-overflow READ at `convolution_internal.h` for 15x15 / 12x12 /
  10x10 / 9x9 and is clean at 16 and above — the break is exactly at 16, as the
  arithmetic predicts.
- `--feature float_motion` with `motion_add_uv=true` on a 4x4 YUV420P frame.
  `motion_check_min_dim()` validated the **luma** dimensions only, while
  `motion_blur_plane()` is called per plane with `ref_pic->w[c]` /
  `ref_pic->h[c]`. 4x4 YUV420P gives a 2x2 chroma plane, below the 3x3 minimum.
  The same 4x4 run with default options (luma only) is clean, which isolates
  the chroma plane as the sole cause.

**Fix.** A single `convolution_reflect101(idx, size)` helper that folds
repeatedly until the index is in range (with a `size <= 1` short circuit, which
would otherwise not terminate), used by all three edge helpers; and a
`convolution_clamp_borders()` that clamps both bounds into `[0, dim]`. For every
`size >= radius + 1` the fold exits on the first iteration and yields the
identical index, so nothing in contract moves — pinned by
`test_large_plane_bit_identical` in `core/test/test_convolution_edge_small.c`,
which compares a 24x24 run against an explicit single-bounce reference and
asserts **bit** equality.

The two guard gaps are closed at the guard, not only in the kernel:

- `float_vif` now derives its minimum from `vif_get_min_dim(kernelscale)` —
  `max` over the ladder of `((filter_width_s / 2) + 1) << s`, which is 16 at the
  default kernelscale — instead of a hard-coded 9.
- `float_motion` validates every plane it will actually convolve, deriving the
  chroma dimensions with `picture.c`'s own `(dim + ss) >> ss` geometry.

**Regression test.** `core/test/test_convolution_edge_small.c` embeds each plane
in a NaN-poisoned buffer: an escaping tap reads NaN and taints the output, an
escaping write replaces a poison NaN with a finite value. On the pre-fix tree
`test_scalar_5tap_small_planes` fails on the first case; after the fix all four
cases pass. Plus the guard cases in `test_float_vif_min_dim.c` (9x9 and 15x15
now rejected, 16x16 accepted) and `test_motion_min_dim.c`
(`motion_add_uv` at 4x4 / 3x3 rejected, 5x5 accepted).

### Netflix/vmaf#1581 — same mirror, motion extractors

The candidate's framing ("latent hardening") was too weak: the chroma path
above is a live heap out-of-bounds read on plain CPU, and it is fixed here.

The motion extractors' own `mirror()` bodies (`integer_motion.c:148`,
`integer_motion_v2.c:152`, `x86/motion_avx2.c:27`, `x86/motion_avx512.c:30`,
`arm64/motion_v2_neon.c:46`, and the CUDA / HIP / Metal twins) are **left
single-bounce on purpose**. They sit downstream of an `init()` guard that has
rejected `w < 3 || h < 3` since Research-0094, so the defective sizes never
reach them. That is a deliberate divergence from upstream #1581, which fixes
`mirror()` so tiny frames can be *scored*; the fork errors out instead. Recorded
in `docs/rebase-notes.md` because `/sync-upstream` will collide here.

The Metal extractors were the exception — see #1580 below.

### Netflix/vmaf#1580 — motion mirror at tiny frames

Mostly ALREADY-FIXED. The fork found this independently
(`docs/research/0094-motion-v2-flush-dict-leak-round7.md`, "Finding E-1") and
discharged it with min-dim `init()` guards. Audit of all 15 motion extractor
translation units: 12 guarded, 3 not — and the 3 were exactly the fork-added
Metal extractors, written after that sweep:

- `core/src/feature/metal/integer_motion_metal.mm` (only rejected
  `motion_add_uv`, then assigned `frame_w`/`frame_h` and allocated)
- `core/src/feature/metal/integer_motion_v2_metal.mm`
- `core/src/feature/metal/float_motion_metal.mm`

All three are registered and shipped (`feature_extractor.cpp:229/232/234`), so
`--feature float_motion_metal` on a 1- or 2-pixel-tall frame read out of bounds
on device. The guard now runs as the first statement after the `fex->priv` cast,
before `vmaf_metal_context_new`, so no cleanup path is needed. Runtime
reproduction needs an Apple GPU; the gap is grep-provable and the fix needs no
hardware to write, which is why this is AFFECTS-FORK and not NEEDS-HARDWARE.

`test_motion_min_dim.c::test_metal_motion_min_dim` exercises the rejection
host-side (it degrades to a no-op on a build without `HAVE_METAL`).

**Adjacent finding, deliberately NOT fixed here.**
`core/src/feature/metal/integer_motion_v2.metal:54` uses
`2 * sup - idx - 1`, while the CUDA twin
(`cuda/integer_adm/../integer_motion_v2/motion_v2_score.cu:50`) and the CPU
source (`integer_motion_v2.c:157`) both use `2 * size - idx - 2`. The Metal
file's own header comment claims it "matches the CUDA twin" — it does not. This
is the boundary-row-replication off-by-one that PR #120 fixed for motion v1, so
Metal `motion_v2` is very likely off-parity at *every* frame size, not only tiny
ones. Fixing it moves Metal `motion_v2` scores and needs a cross-backend parity
run on Apple hardware: own PR, own ADR. `docs/state.md` row added.

### Netflix/vmaf#1242 — VmafFeatureDictionary ownership

Three findings, all present.

1. **Leak on the `-ENOMEM` path.** `core/src/model.c` returned `-ENOMEM`
   straight out of the loop when `vmaf_dictionary_merge()` failed, skipping the
   unconditional `vmaf_dictionary_free(&opts_dict)` at the tail. Under the
   contract the fork itself publishes, that is an unconditional leak of the
   caller's dictionary. The correct shape already existed in-tree, in the
   **unbuilt** C++ twin `core/src/model.cpp` (`err = -ENOMEM; break;`);
   `core/src/meson.build` compiles `model.c`, not `model.cpp`.
2. **Collection wrapper compounded it and swallowed the error.**
   `vmaf_model_collection_feature_overload()` discarded
   `vmaf_dictionary_copy()`'s return value, never freed the partially built
   copy, silently skipped the remaining sub-models, and could still return 0
   from the lead-model call. It also dereferenced `*model_collection` without
   checking it and never checked `model` / `feature_name` / `opts_dict`.
3. **The headers contradicted each other.** `core/include/libvmaf/feature.h`
   said "on failure the caller still owns the dictionary"; `model.h` and
   ADR-0806 said ownership transfers "on both success and failure". The fork's
   own test `core/test/test_model_collection_api.c:274-277` follows `feature.h`
   — correct against the implementation, a textbook CWE-415 double free against
   the `model.h` wording. A third-party caller reading either header alone gets
   it wrong on some path.

**Fix.** `model.c` breaks to the common exit instead of returning; the
collection wrapper captures the copy result, frees the partial copy, folds the
error into `err`, and gains the missing argument guards; `model.cpp` is kept
convergent. The contract is written **once**, identically, in `feature.h`,
`model.h` (both call sites) and `libvmaf.h`, in the shape the implementation
actually has:

> the dictionary is consumed on every path EXCEPT the argument-validation
> guards (`-EINVAL` from a NULL or unknown argument), where nothing was
> consumed and the caller still owns it.

That is also what `vmaf_use_feature()` does — `core/src/libvmaf.c:1607-1650`
returns `-EINVAL` for `!vmaf` / `!feature_name` / unknown feature name without
touching `opts_dict`, and consumes on every other path. ADR-0806's line
citations were stale (`model.c:196`, `libvmaf.c:1520-1525`); it is now marked
Superseded by ADR-1166 rather than edited in place.

**No production double free today**: the only in-tree production callers are
`core/tools/vmaf.cpp:471-473` and `:520-522`, and neither frees the dict
afterwards. The exposure was leaks on the OOM / `-EINVAL` paths plus the
contract landmine for external API users.

**Regression test.** `core/test/test_model_feature_overload_ownership.c` pins
the guard paths (caller may still free), the success path (consumed), and drives
the merge-failure branch deterministically without malloc-fail injection: a
heap-allocated **empty** dictionary makes `vmaf_dictionary_merge()` return NULL,
which is exactly the branch that leaked. The leak itself is what LeakSanitizer
reports in the ASan lane. The NULL-collection case is undefined behaviour
pre-fix (the optimiser deletes it under LTO, leaving a garbage return) and a
defined `-EINVAL` after.

### Netflix/vmaf#743 — Windows console mojibake

Present verbatim. `core/tools/spinner.h` carries the 56-entry UTF-8 braille
table (336 non-ASCII bytes = 56 × 2 glyphs × 3 bytes), and
`core/tools/vmaf.cpp` emitted it with a plain byte-oriented `fprintf` to stderr,
inside the `if (istty && !c->quiet)` block — the interactive-console case.

Grep-provable API misuse: nothing in the tree set the console output code page
or enabled VT processing.
`grep -rn "SetConsoleOutputCP\|CP_UTF8\|GetConsoleOutputCP\|_setmode\|_O_U8TEXT" core/`
returned nothing; the one `#include <windows.h>` in the CLI is commented as
being there for `QueryPerformanceCounter` (ADR-1081).

Decoding the exact frame bytes under the default console code pages, Linux-side:

| Code page | Result |
|---|---|
| cp437 (conhost default) | 6 garbage glyphs — the progress line is 4 characters wider than the `\r` overwrite assumes |
| cp1252 | 6 different garbage glyphs |
| cp936 | `illegal multibyte sequence` on byte 0x80; conhost renders replacement boxes |

Same line, second defect: the trailing `\033[K` erase-to-EOL was also
unconditional, and legacy conhost has `ENABLE_VIRTUAL_TERMINAL_PROCESSING` off
by default, so it prints literally.

The surface is live on Windows: `libvmaf-build-matrix.yml` builds the MinGW64
CPU leg and the two MSVC + CUDA / SYCL legs, all of which compile `vmaf.cpp`.

**Fix.** A `WindowsConsoleGuard` RAII object declared at the top of `main()`
switches the console to UTF-8 and enables VT for the run, restoring the previous
code page and mode in its destructor — which C++ runs on every `goto cleanup`
path too, since the guard is declared before every jump target. `spinner.h`
grows an ASCII fallback table plus `spinner_table_for_codepage()` and
`spinner_erase_eol()`; `vmaf.cpp` resolves both from the console's actual
capabilities. Everything Windows-specific is `#ifdef _WIN32`; on POSIX the
selectors return the braille table and `\033[K` unconditionally, so the emitted
bytes are unchanged.

**Regression test.** `core/test/test_spinner.cpp` drives the selectors with the
code pages a real conhost reports (437, 1252, 936, 0) and asserts the ASCII
table; asserts the braille table for 65001; asserts the erase sequence is
VT-gated; and pins the braille table's first/last entries byte-for-byte plus
`strlen(entry) == 6` for all 56, so the POSIX output cannot drift.

### Netflix/vmaf#1573 — build-system hunks

Hunk (a) — `picture_cuda.c` uninitialised `priv->cuda.state` — is
**ALREADY-FIXED**: `core/src/cuda/picture_cuda.c:190` sets it in the pinned path
and `:251` in the device path.

Hunk (b) — **AFFECTS-FORK, and worse here than upstream.**
`core/src/meson.build` fed nvcc relative includes (`-I ./src -I ../src
-I ../include ...`). Since ADR-0700 moved the project root to `core/`, those only
resolve when the build directory is a direct child of `core/`. With the layout
the fork's own docs use (`meson setup build core` from the repo root) the CUDA
build hard-fails:
`core/src/./feature/cuda/integer_adm/adm_dwt2.cu:23:10: fatal error: cuda/integer_adm_cuda.h: No such file or directory`.
The neighbouring SYCL block already did this correctly with absolute
`meson.current_source_dir()` paths, so the fix pattern was in-tree. The
Windows pthread-shim include was relative for the same reason and is now
absolute too.

Hunk (c) — **AFFECTS-FORK.** `core/tools/test/meson.build` registered
`test_vmaf_cuda_gpumask` with only `suite`/`timeout` — no `depends`, no
`workdir` — while the script invokes `./tools/vmaf`. Meson only rebuilds the
targets a *selected* test declares (`mtest.py` `rebuild_deps()`: "if not
targets: return True"), so selecting it as a subset built nothing and the script
died with `exit status 127`. Reproduced on a freshly configured, uncompiled CUDA
build dir. The two neighbouring fork-added tests (`test_vmaf_per_shot`,
`test_vmaf_roi_high_bitdepth`) had the same omission. All three now declare
`depends` and `workdir`.

### Netflix/vmaf#1551 (and the live third of #1422) — the MSVC clz shim

`core/src/feature/compat_builtin.h` carried the exact defective shim upstream
Netflix/vmaf#1551 replaces — and it is **fork-added**, adopted from #1422's proposal, not
inherited:

```c
static inline int __builtin_clz(unsigned x)             { return (int)__lzcnt(x); }
static inline int __builtin_clzll(unsigned long long x) { return (int)__lzcnt64(x); }
```

LZCNT encodes as `F3 0F BD`. On an x86-64 without ABM/LZCNT (Intel Nehalem
through Ivy Bridge; AMD pre-Barcelona) the `F3` prefix is ignored and the
instruction retires as **BSR**, returning the index of the highest set bit
instead of the leading-zero count. No fault, no diagnostic.

Two of the four call sites are on the generic scalar path, behind no SIMD gate:

| Site | Expression | correct | as BSR |
|---|---|---|---|
| `integer_vif.h:148` `log2_32` | `k = 16 - clz(temp)`, `temp=0x00010000` | 1 | 0 → a 2048-LSB error, i.e. a factor of two in the VIF log2 fixed point |
| `integer_vif.h:148` `log2_32` | `temp=0x80000000` | 16 | −15 → shift by a negative count (UB) |
| `integer_adm.c:989` `get_best15_from32` | `k = 17 - clz(temp)`, `temp=0x40000000` | 16 | −13 → `1 << (k-1)` shifts by a negative count |

CI cannot catch it: the MSVC leg is a real shipped path (it runs CPU unit tests
and uploads `install/bin/vmaf.exe` as the `windows-msvc-cuda-vmaf` artifact), but
every hosted Windows runner is LZCNT-capable, so the shim tests correct there
and the divergence only appears on a user's older machine. No runtime gate and
no `/arch` baseline exists in the tree.

**Fix.** `_BitScanReverse` / `_BitScanReverse64`, which are BSR by definition
and present on every x86-64 part, plus a `_M_X64 || _M_IX86` architecture test
on the guard (neither intrinsic exists on MSVC ARM64, so that leg previously
failed to compile) and a 32-bit fallback path. The names stay `__builtin_clz*`:
the four call sites are upstream-verbatim and the rebase story depends on the
spelling (ADR-0141 §2).

Do **not** adopt the `__lzcnt` form of Netflix/vmaf#1422 — that is the defect,
and #1551 is upstream's own retraction of it. This is recorded inline in the
header and enforced by `scripts/ci/check-msvc-clz-shim.sh`.

**Regression tests.** `scripts/ci/check-msvc-clz-shim.sh` (registered as a
`fast`-suite meson test) fails on the pre-fix header and passes after; it also
scans the rest of `core/src` so the intrinsic cannot come back elsewhere.
`core/test/test_compat_clz.c` unit-tests the `31 - msb` / `63 - msb`
arithmetic — the part the `__lzcnt` form got wrong — on every platform, and on
the MSVC legs exercises the real shim bodies.

The other two thirds of #1422 are dead here: the `integer_vif.c` pointer-typing
hunk was already rewritten fork-side (`vif_buffers_alloc` carves the aligned
allocation with an explicit `uint8_t *data` and per-field casts, and its block
comment names the MSVC C2036 problem), and the `HAVE_UNISTD_H` /
`HAVE_DIRECT_H` / `HAVE_STRUCT_TIMESPEC` meson feature-detection hunk is
NOT-APPLICABLE — the fork solved the same portability problem by point-gating
the includes (rebase-notes round-21 items (l)/(m)).

### Netflix/vmaf#1178 — C++ runtime missing from `Libs.private`

`core/src/meson.build` built `libvmaf_private_libs` as `[thread_lib, math_lib]`
and only extended it for SYCL and the Metal frameworks. Every generated
`libvmaf.pc` in this tree read:

```text
Libs: -L${libdir} -lvmaf
Libs.private: -pthread -lm
```

The fork is far more exposed than upstream: the undefined symbols come not just
from vendored `svm.cpp` but from the fork's own C++ conversions
(`luminance_tools.cpp`, `feature_extractor.cpp`, `feature_collector.cpp`,
`log.cpp`, `read_json_model.cpp`, the C++23 picture pools, ...).

Reproduced with the existing static archive:

```console
$ gcc t.c -I core/include build/src/libvmaf.a -pthread -lm -o t     # == Libs + Libs.private
/usr/bin/ld: undefined reference to `operator new(unsigned long)'
/usr/bin/ld: undefined reference to `operator delete(void*, unsigned long)'
...
$ gcc t.c -I core/include -L build/src $(pkg-config --static --libs libvmaf) -o t   # after the fix
GOOD LINK OK
```

Corroboration that this already bit a real consumer:
`docs/adr/0198-volk-priv-remap-static-archive.md:130-133` records the BtbN-style
fully-static FFmpeg reproducer with `-lstdc++` added **by hand**, precisely
because `libvmaf.pc` did not carry it.

Upstream's patch cannot be ported: `else compiler.get_id() == 'clang':` is not
meson syntax, and its clang→`-lc++` mapping is wrong on Linux, where clang
defaults to libstdc++. The fork detects the STL actually in use
(`_LIBCPP_VERSION` via `cxx.get_define`) instead of keying off the compiler id,
and skips MSVC/clang-cl, which auto-link the runtime with
`#pragma comment(lib)`.

**Regression test.** The CI step "Verify static pkgconfig" was grep-only; it
never attempted a link. It now compiles a 3-line C consumer with the **C**
driver against exactly what `pkg-config --static --libs libvmaf` reports — the
test that actually reproduces the downstream FFmpeg failure — and keeps the
`-lm` / `-pthread` / C++-runtime greps as cheap extras.

Note: the ORT dependency the candidate flagged as an "adjacent gap" is already
present in `Libs.private` on a DNN-enabled configure; the C++ runtime was the
only missing element.

CLAUDE.md §12 r14 check: **no `ffmpeg-patches/` update is required**. The change
adds no public C-API entry point, no configure flag, no `LIBVMAFContext` field,
and renames no symbol the `check_pkg_config` probes look for — the existing
probes simply start succeeding under `--pkg-config-flags=--static`.

## Confirmed but deferred

Each of these is real and located; each has a `docs/state.md` row. None is in
this PR, and the reason is given.

| Upstream | Why not batched |
|---|---|
| **#1564** | Three defects. (1) CUDA/HIP `i4 adm_cm` `i == 0` border walks flt rows {1,2,3} where the CPU reference reads {1,0,1} — reachable only for `h_at_scale <= 14`, which no current fixture hits. (2) The `>> shift_inner_accum` rounding is applied per warp (CUDA) / per thread (HIP) instead of once per image row, so `round(a)+round(b) != round(a+b)` accumulates ~n/2 output units per row. That one changes every CUDA/HIP ADM score and the launch shape, and `test_cuda_adm_parity.c`'s `PARITY_TOL` of 1e-4 is too loose to prove the fix. (3) The x86 `half_w_modN` empty scalar tail is present but empirically **score-neutral** at 388x288 across `--cpumask 0 / 8 / 56` — the corrupted column always falls inside the `ADM_BORDER_FACTOR` crop, the same conclusion the fork already reached for the NEON twin. Needs GPU parity runs; splits into two PRs of very different risk. |
| **#930** | The fork holds **four** different `angle_flag` predicates for one test: CPU/AVX2/AVX-512 (narrow to float, evaluate in double), CUDA/HIP scale 0 (exact int64), CUDA/HIP scales 1-3 (matches CPU), SYCL (all-float), Metal (exact int64 narrowed to float). A 40M-sample sweep of near-parallel vectors puts the disagreement at 0.0031-0.0046%, switching on exactly where the operands cross 2^24 — the float mantissa. Inherited verbatim from upstream, not fork-introduced. Fixing it moves GPU scores and needs `/regen-snapshots` plus an ADR; the CPU side is frozen by the golden gate. |
| **#1568** | `output_file_open()` uses the narrow `_open()`, which decodes with the process ANSI code page. The fork has the same class of defect at 12 further sites upstream does not have (`vmaf.cpp` fopen sites, `read_json_model.cpp`, `vmaf_per_shot.c`, `vmaf_roi.c`, `vmaf_bench.c`, `vmaf_vpl.c`, `dnn/model_loader.c`, `interop/pelorus_qp_report_csv.c`). Wants a new `core/src/compat/path_utf8.{h,c}` surface and a documented encoding contract on the public API — its own ADR. |
| **#1109** | The per-frame `MIN(..., psnr_max)` doubles as both the infinity sentinel and a hard truncation. Reproduced: a one-luma-byte flip on the 576x324 golden reference gives `psnr_y=60.0` where the ground truth (and FFmpeg's own psnr filter) is 100.840479 — a 40.84 dB under-report. The escape hatch (`--feature psnr=min_sse=0.000001`) exists but is undocumented, and there is no `docs/metrics/psnr.md` at all. The fix wants an opt-in `uncapped` option propagated to eight GPU twins plus a new docs page and an ADR; the golden assertions at 60/84/108 are all `sse == 0` byte-identical pairs and would survive, but proving that is part of the work. |
| **#766** | `cli_parse.cpp` splits `--model` / `--feature` option strings with raw `strsep` and no escape state. Reproduced against the existing build: `path=<dir>/dir=eq/m.json` **silently truncates** to `.../dir` and reports a phantom path; `path=C:\models\x.json` is unrepresentable. Blast radius is wider here because `pkg/libvmaf/libvmaf.go`, `pkg/scorecli`, `pkg/corpus` and `cmd/vmafx-mcp` all synthesise the same string from user paths. Changes user-visible CLI grammar and triggers §12 r14 on `ffmpeg-patches/0008`; needs its own ADR. |
| **#818** | The public pooling enum still has no `MEDIAN` / `PERC*` enumerator, five years on. The candidate's "silently falls back to mean" claim is **refuted** — `pool_reduce()` ends in `default: return -EINVAL;` — and so is "two surfaces disagree": the Python `perc10` path never reaches the C pooling code (it applies `ListStats.perc10` to the per-frame list in NumPy). Growing the enum triggers §12 r14 on three ffmpeg filter patches. |
| **#1305** | The upstream gap (`vmaf_score_at_index` has no fence against pending GPU work) is unchanged here. The fork *also* introduced a multi-instance defect of its own: `core/src/cuda/drain_batch.c:49` keys the ADR-0242 fence batch by `_Thread_local`, not by `VmafContext`, and `vmaf_close()` never closes it — so an abandoned instance leaves destroyed `CUevent`s and dangling `bool*` in a batch the next instance flushes. Establishes statically from the call graph; a GPU repro is the natural first step of the fix PR. |
| **#1494** | The candidate's nvd/rdh premise is **false**: `integer_adm.c:3509-3512` already rejects `nvd * rdh < 3240`, and over the whole allowed region `i_rfactor` stays below 65536. What *is* real is fork-specific: the fork-added `adm_csf_mode=1` (BARTEN) overflows `uint16_t i_rfactor` at the stock nvd=3.0/rdh=1080 (exact 2538596 / 10154382 wrap to 48227 / 61838), and the run produces `integer_adm2_csf_1` mean 0.000614 against the fork's own float reference of 0.9396 — a ~1500x discrepancy. Widening also needs the `adm_cm` products moved to int64 and the AVX2/AVX-512 twins restructured to 64-bit lanes; two stages, own PR. |

## Reproducer commands

```bash
# CPU-only build used throughout
meson setup build core -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled
meson compile -C build -j 8

# The regression tests added by this harvest
meson test -C build --suite=fast -j 4

# The two convolution defects, isolated (fails before the fix, passes after)
./build/test/test_convolution_edge_small

# The clz shim guard (fails on the pre-fix header)
bash scripts/ci/check-msvc-clz-shim.sh

# The pkg-config static-link defect
PKG_CONFIG_PATH=$PWD/build/meson-private pkg-config --static --libs libvmaf
# pre-fix:  -L/usr/local/lib -lvmaf -pthread -lm
# post-fix: -L/usr/local/lib -lvmaf -pthread -lm -lstdc++

# Netflix golden gate — must not move
CUDA_VISIBLE_DEVICES= make test-netflix-golden
```

## Golden values (unchanged)

Float VMAF v0.6.1, `build/tools/vmaf`, on the three canonical CPU pairs:

| Pair | VMAF mean |
|---|---|
| `src01_hrc00_576x324` vs `src01_hrc01_576x324` | 76.66744 |
| `checkerboard_1920_1080_10_3_0_0` vs `..._1_0` (1-px shift) | 35.070245 |
| `checkerboard_1920_1080_10_3_0_0` vs `..._10_0` (10-px shift) | 7.985956 |

`make test-netflix-golden`: 271 passed, 12 skipped, 0 failed.

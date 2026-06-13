# Known upstream bugs

Bugs that reproduce on `upstream/master` (Netflix/vmaf) as well as this fork's
`master`, discovered during fork work but out of scope for the PR that found
them. Each entry records the reproducer, the evidence it is upstream, and the
suggested fix.

When a fork-local PR touches the same file, prefer to fix the bug in that PR
and reference this entry in the commit. If the PR does not touch the file,
file a follow-up ticket and link to it here.

---

## `adm_decouple_s123_avx512` LTO+release SEGV — fixed in this fork

**Status:** fixed in this fork (PR #69 follow-up commit), still present upstream.

**Symptom:** `test_pic_preallocation` aborts with
`AddressSanitizer: SEGV on unknown address` inside
`adm_decouple_s123_avx512` when the binary is built with
`--buildtype=release -Db_lto=true -Db_sanitize=address`. The debug
ASan build used by CI (`--buildtype=debug -Db_lto=false`) does not
reproduce the crash.

Reproduce with:

```bash
meson setup build-asan-lto libvmaf \
  -Denable_cuda=false -Denable_sycl=false \
  -Db_sanitize=address --buildtype=release -Db_lto=true
ninja -C build-asan-lto test/test_pic_preallocation
ASAN_OPTIONS=detect_leaks=1 ./build-asan-lto/test/test_pic_preallocation
```

**Evidence it is upstream, not fork-local:** the same reproducer on
`origin/master` (no fork-local patches applied) produces the same
crash. The faulting instruction is
`vmovdqa64 zmm2, ZMMWORD PTR [rdi-0xc0]`, a 64-byte-aligned AVX-512
load served a 32-byte-aligned address.

**Why CI does not catch it:** CI's sanitizer job uses
`--buildtype=debug -Db_lto=false`, which keeps every
`_mm512_loadu_si512` as `vmovdqu64` (unaligned) and so runs fine. The
`--suite=unit` filter in `tests-and-quality-gates.yml` also matches
zero tests in `core/test/meson.build`, so the job reports green
even if the link succeeds. Tracked separately — the suite filter
needs to be corrected.

**Root cause:** the stack array `int64_t angle_flag[16]` inside
`adm_decouple_s123_avx512` is loaded via
`_mm512_loadu_si512(&angle_flag[0])` and
`_mm512_loadu_si512(&angle_flag[8])`. Under LTO, link-time
alignment inference promotes the unaligned loads to the aligned
`vmovdqa64` form. The C-level default stack alignment for an
`int64_t[16]` is 8 bytes, so the promoted aligned load faults on
every other 64-byte slot.

**Fix applied in this fork:** annotate the stack array with
`_Alignas(64)` at
[`core/src/feature/x86/adm_avx512.c:1317`](../../core/src/feature/x86/adm_avx512.c#L1317).
The unaligned load remains correct, and the LTO-promoted aligned
form is now also correct.

**Related issue surfaced during triage:**
`test_picture_pool_basic`, `test_picture_pool_small`, and
`test_picture_pool_yuv444` loaded a `VmafModel` via
`vmaf_model_load` and never called `vmaf_model_destroy`, so
LeakSanitizer reported 208 bytes direct + 23 KiB indirect leaks per
test. Pairing `vmaf_model_destroy(model)` with each load is also
landed in PR #69 (same commit).

---

## `KBND_SYMMETRIC` single-reflection at sub-kernel-radius input sizes

**Status:** fixed in this fork (PR #69), still present upstream.

**Symptom:** For a 2-D convolution with a 9-tap kernel on inputs
smaller than the kernel half-width (n ≤ 3 for `LPF_HALF = 4`),
upstream's `KBND_SYMMETRIC` reflects the index only once; the
reflected index is still out of bounds, causing an out-of-bounds read.

Reproduce with:

```c
/* With upstream KBND_SYMMETRIC, idx=-4, n=1 reflects to 3 (OOB for n=1). */
float v = KBND_SYMMETRIC(img_1x1, 1, 1, -4, 0, 0.0f);  /* reads img[3] */
```

**Why it is latent upstream:** MS-SSIM pyramids never decimate below
~60×34 in practice, and SSIM / ADM similarly never feed a 1×1 input
through the convolver. Nothing in Netflix/vmaf's test corpus exercises
the regime.

**Fix applied in this fork:** `KBND_SYMMETRIC` and
`ms_ssim_decimate_mirror` (scalar + AVX2 + AVX-512 + NEON) are
rewritten in the period-based (`period = 2*n`) form that bounces
correctly for any offset. See
[`docs/adr/0125-ms-ssim-decimate-simd.md`](../adr/0125-ms-ssim-decimate-simd.md)
and the inline comment in
[`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c).

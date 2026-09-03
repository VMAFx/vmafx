- **Upstream issue harvest (ADR-1166): nine stale Netflix/vmaf reports
  verified against the fork and fixed, eight more recorded.** The fork
  diverged far enough from upstream that an open report there is neither
  automatically applicable nor automatically stale, so each candidate was
  checked against this tree and reproduced before anything was changed. The
  full triage table — including the ALREADY-FIXED and NOT-APPLICABLE
  verdicts, which are the expensive ones to re-derive — is in
  `docs/research/1166-upstream-issue-harvest-2026-09-03.md`.

  Fixed here:

  - **Netflix/vmaf#1582 — two out-of-bounds accesses in the float
    convolution, both reachable from the public C API today.** The
    reflect-101 mirror in `convolution_edge_s` / `_sq_s` / `_xy_s` bounced an
    out-of-range tap exactly once, which only lands in range when the plane
    is at least `radius + 1` across; below that it fell out the opposite
    side (heap-buffer-overflow READ). Separately,
    `convolution_x_c_s` / `convolution_y_c_s` derived the trailing border
    bound as `dim - (filter_width - radius)`, which goes negative for a
    plane narrower than the filter, so the trailing loop started at a
    negative index and wrote `dst[i * dst_stride - 1]` — a heap underflow
    WRITE. Two live paths reached the defective sizes: `--feature float_vif`
    on any frame in 9..15 px (the guard admitted `>= 9`, but the four-scale
    ladder needs `>= 16` — the binding constraint is scale 3), and
    `--feature float_motion` with `motion_add_uv=true` on a 4x4 YUV420P
    frame (the guard validated luma only, while the blur runs per plane at
    the 2x2 chroma dimensions). The mirror now folds iteratively — bit-
    identical to the single bounce for every in-contract size, pinned by a
    bit-equality test at 24x24 — the borders are clamped, `float_vif`
    derives its minimum from `vif_get_min_dim(kernelscale)`, and
    `float_motion` validates every plane it will convolve.
  - **Netflix/vmaf#1581 — same mirror, motion extractors.** The motion
    extractors' own single-bounce `mirror()` bodies are deliberately left
    alone: they sit downstream of an `init()` guard that rejects
    `w < 3 || h < 3`, so the defective sizes never reach them. That is a
    deliberate divergence from upstream, which instead fixes `mirror()` so
    tiny frames can be scored; see `docs/rebase-notes.md`.
  - **Netflix/vmaf#1580 — the three fork-added Metal motion extractors had
    no dimension guard at all.** `motion_metal`, `motion_v2_metal` and
    `float_motion_metal` are registered and shipped, so a 1- or 2-pixel-tall
    frame read out of bounds on device. They now carry the same min-dim
    guard as the CPU, CUDA, SYCL and HIP twins, placed before the Metal
    context is created so no cleanup path is needed.
  - **Netflix/vmaf#1242 — `VmafFeatureDictionary` ownership.**
    `vmaf_model_feature_overload()` returned `-ENOMEM` straight out of its
    loop and skipped the unconditional free at the tail, leaking the
    caller's dictionary; `vmaf_model_collection_feature_overload()`
    discarded the copy's return value, leaked the partial copy, silently
    skipped the remaining sub-models, and dereferenced `*model_collection`
    without checking it. `<libvmaf/feature.h>` and `<libvmaf/model.h>` also
    documented **opposite** ownership rules, which made one of the two
    readings a latent double free for any third-party caller. All three
    public headers now state the implemented contract identically: the
    dictionary is consumed on every path except the argument-validation
    guards, where the caller still owns it. ADR-0806 is superseded.
  - **Netflix/vmaf#743 — the CLI progress spinner was mojibake on Windows.**
    The UTF-8 braille table went to stderr through a byte-oriented
    `fprintf` while nothing in the tree ever set the console output code
    page or enabled VT processing, so under cp437 the two glyphs rendered as
    six garbage characters (widening the line past the `\r` overwrite),
    under cp936 as replacement boxes, and the `\033[K` erase printed
    literally on legacy conhost. The CLI now switches the console to UTF-8 +
    VT for the run and restores the previous state on exit, and falls back
    to an ASCII spinner and space padding when the console refuses either.
    POSIX output is byte-identical.
  - **Netflix/vmaf#1551 (retracting Netflix/vmaf#1422) — the MSVC
    `__builtin_clz` shim emitted LZCNT.** MSVC's `__lzcnt` emits
    `F3 0F BD` with no runtime feature gate; on an x86-64 without ABM/LZCNT
    the prefix is ignored and the instruction retires as BSR, returning the
    MSB index instead of the leading-zero count. Two of the four call sites
    are on the generic scalar path, so an MSVC-built `vmaf.exe` on
    pre-Haswell hardware silently mis-normalised every VIF and ADM log2 (a
    2048-LSB error, i.e. a factor of two in the VIF fixed point) and shifted
    by a negative count for large inputs — no fault, no diagnostic, and
    invisible to CI because every hosted Windows runner has LZCNT. The shim
    now uses `_BitScanReverse`, which is BSR by definition and present on
    every x86-64 part, and carries an architecture guard so an MSVC ARM64
    leg compiles. `scripts/ci/check-msvc-clz-shim.sh` keeps the intrinsic
    from coming back.
  - **Netflix/vmaf#1178 — `libvmaf.pc` omitted the C++ runtime.**
    `pkg-config --static --libs libvmaf` reported only `-pthread -lm`, so
    linking the static archive failed with hundreds of undefined references
    to `operator new` / `std::ios_base::ios_base()` — the fork is more
    exposed than upstream because the C++ symbols come from its own
    converted translation units, not just vendored libsvm. Downstream
    fully-static FFmpeg builds had to add `-lstdc++` by hand (see
    ADR-0198). `Libs.private` now carries the C++ runtime, detected from
    the STL actually in use (`_LIBCPP_VERSION`) rather than from the
    compiler id, and the CI check performs a real link instead of grepping
    the flag list.
  - **Netflix/vmaf#1573 — two build-system defects.** The nvcc fatbin
    include list used relative paths, which only resolve when the build
    directory is a direct child of `core/`; since ADR-0700 the layout the
    docs themselves use put it elsewhere, and every `.cu` failed with
    `fatal error: cuda/integer_adm_cuda.h: No such file or directory`. And
    the three shell-driven tool tests declared no `depends`, so running one
    as a subset (`meson test test_vmaf_cuda_gpumask`) built nothing and the
    script died with exit 127.

  Regression tests: `core/test/test_convolution_edge_small.c`,
  `core/test/test_compat_clz.c`,
  `core/test/test_model_feature_overload_ownership.c`,
  `core/test/test_spinner.cpp`, `scripts/ci/check-msvc-clz-shim.sh`, plus
  extended cases in `core/test/test_motion_min_dim.c` and
  `core/test/test_float_vif_min_dim.c`. Netflix golden scores unchanged
  (76.66744 / 35.070245 / 7.985956; 271 passed, 12 skipped).

  Behaviour change to note: `float_vif` now rejects frames below 16 px in
  either dimension (it previously accepted 9 px and read out of bounds at
  scale 3), and `float_motion` with `motion_add_uv` now rejects frames whose
  chroma planes fall below the filter minimum. Both convert previously
  undefined behaviour into a documented `-EINVAL`.

    Post-review corrections (three defects an independent adversarial review
    found in the harvest itself, all reproduced before fixing):

    - **The #1582 border clamp landed only on the scalar path.**
      `convolution_f32_c_s` dispatches to `convolution_f32_avx_s` whenever
      AVX2 is present — every CI runner and the dev workstation — so the
      clamp was dead code on x86. The AVX2 and AVX-512 twins derive the same
      `height - radius` split at three sites each and kept it unclamped: for a
      plane shorter than the radius that is negative, so the trailing border
      loop starts at a negative row and the leading one runs past the end.
      Both are heap **writes**. All six sites now share the scalar clamp,
      which moved into `convolution_internal.h`.
    - **`motion_filter_size=1` bypassed the minimum-dimension guard.**
      `motion_check_min_dim` gated the whole check on
      `effective_filter_size > 1`, but `motion_blur_plane` keeps
      `filter_size = 5` for that value and only swaps in the no-op
      coefficients, so radius stays 2. A 1-row plane therefore reached the
      convolution above through a documented public option (range 0..9). The
      guard now mirrors `motion_blur_plane` exactly.
    - **Odd-height 4:2:0 chroma planes were under-allocated by one row.**
      `motion_chroma_heights` used `h / 2` while `picture.c` and the guard
      both use the ceiling `(h + 1) >> 1`, so `motion_copy_and_blur` overran
      `ref`, `tmp` and every blur-ring buffer for both U and V. Even heights
      were unaffected, which is why the golden fixtures never caught it.

    Regression test `core/test/test_motion_convolution_oob.c` drives
    `float_motion` through the public `vmaf_read_pictures` entry point,
    because neither existing test could reach the dispatched SIMD path:
    `test_motion_min_dim` only calls `init()`, and
    `test_convolution_edge_small` calls the scalar kernels directly. Verified
    both ways — the new test fails on the pre-fix tree, and under
    `-Db_sanitize=address` the pre-fix tree reports
    `heap-buffer-overflow ... WRITE of size 4 in convolution_f32_avx_s`
    reached from `vmaf_read_pictures`.

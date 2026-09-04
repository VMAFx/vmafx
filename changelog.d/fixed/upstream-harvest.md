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

    Second post-review round — the `VmafFeatureDictionary` ownership contract
    (Netflix/vmaf#1242) was still stated three different ways, and one of them
    was a double free:

    - `<libvmaf/feature.h>` and `docs/api/index.md` claimed that an unknown
      `feature_name` never consumes the dictionary. That is true of
      `vmaf_use_feature`, which resolves the name against the global extractor
      registry and returns `-EINVAL` before touching it — but **not** of
      `vmaf_model_feature_overload`, which matches against the features of one
      particular model. A name matching nothing there is a successful no-op
      returning `0`, and the dictionary is consumed anyway. A caller following
      the old wording would double-free. All three headers and the API page now
      state the asymmetry explicitly, and `<libvmaf/model.h>` no longer claims
      its rule "matches `vmaf_use_feature`".
    - `vmaf_use_feature` leaked the caller's dictionary on two failure paths —
      a failed `vmaf_dictionary_copy`, and a failed
      `vmaf_feature_extractor_context_create`, which frees only what it
      allocated. Both leaked exactly when the documented contract told the
      caller not to free. Both now release it.

    Two cases added to `core/test/test_model_feature_overload_ownership.c` pin
    the asymmetry from both sides, and pass under `-Db_sanitize=address`.

    Third post-review round — the MSVC clz shim's architecture allowlist
    excluded the one platform it was introduced to protect:

    - **The `_M_X64 || _M_IX86` guard locked MSVC ARM64 out of the shim
      entirely.** Both the header comment and
      `scripts/ci/check-msvc-clz-shim.sh` justified the exclusion by
      asserting that `_BitScanReverse` is x86-only. The MSVC intrinsics
      reference states the opposite: `_BitScanReverse` is available on x86,
      ARM, x64 **and** ARM64, and only `_BitScanReverse64` is restricted (to
      x64 and ARM64). Since this header is the sole definition of
      `__builtin_clz` for `integer_adm.c` and `integer_vif.h` — generic
      scalar code compiled for every target — an MSVC ARM64 build fell
      through the guard with no definition at all and could not compile.
      Nothing observed it because the fork has no MSVC ARM64 CI leg. The
      allowlist now enumerates every architecture MSVC targets,
      `_BitScanReverse64` is used on x64 and ARM64 with the two-step 32-bit
      reconstruction elsewhere, and the gate pins the ARM64 arm so it cannot
      be narrowed again. Both the narrowing and the `__lzcnt` reintroduction
      were negative-tested against the gate.

    Fourth post-review round — the adversarial review's remaining findings,
    each checked against the code before acting. Two were confirmed as
    defects in this PR's own earlier rounds, two were pre-existing
    cross-backend gaps, one was a real evasion hole in a gate this PR added,
    and one did not hold up:

    - **The Metal motion guard added in round 3 was insufficient, and the
      real defect was in the kernel.** `integer_motion.metal`,
      `float_motion.metal` and `integer_motion_v2.metal` load a
      `TILE_W x TILE_H = 20x20` threadgroup tile at origin `bid * 16 - 2`,
      so their mirror helper receives indices up to `16*bid + 17` -- far
      outside the 5-tap neighbourhood it appears to serve. A single bounce
      only lands in range when `idx <= 2 * (sup - 1)`. Enumerating the real
      tile span, the single-bounce form read out of bounds for **every
      dimension in 1..9 and for exactly 17** (at 17 the last workgroup
      reaches idx 33 while `2 * (17 - 1) = 32`, folding to -1). The
      `w < 3 || h < 3` guard closes neither the 4..9 range nor 17, so the
      fix moved into the kernels: all three now fold iteratively, exactly as
      the CPU scalar path does in `convolution_internal.h`. Verified by
      exhaustive enumeration over dims 1..299 across the full tile span --
      always in range, always terminating, and bit-identical to the single
      bounce wherever one bounce already sufficed, so no in-contract score
      moves. The host-side guard comments were rewritten: they had claimed
      the 3x3 floor was what kept the kernel in bounds, which was wrong.
    - **`integer_motion_v2.metal` was the last backend still using the
      wrong reflection convention.** Its `mv2_mirror` used
      `2 * sup - idx - 1`, which reflects `idx == sup` back to `sup - 1` and
      so REPEATS the boundary row; reflect-101 skips it
      (`2 * (sup - 1) - idx`). CPU (`integer_motion_v2.c::mirror`), CUDA
      (fixed in PR #120 / T7-15), SYCL and HIP all carry the corrected
      form, and the SYCL fix records the measured impact of the `- 1` form
      as a systematic ~2.6e-3 motion drift vs CPU on every frame after the
      first. Metal now matches (ADR-0214 places=4). The ADM kernels'
      `2 * sup - idx - 1` was checked and deliberately left alone: ADM
      legitimately uses whole-sample reflection, matching
      `adm_tools.c::dwt2_src_indices_filt_s`, the CUDA
      `calculate_indices()` and the SYCL twin.
    - **All four GPU `float_vif` backends were missing the CPU's minimum
      dimension.** The CPU floor became `vif_get_min_dim(kernelscale)` = 16
      at the default kernelscale earlier in this PR (the binding constraint
      is scale 3, `max(9, 10, 12, 16)`), but Metal's only check was
      `scale_w[FVIF_SCALES - 1] == 0`, i.e. `w >> 3 == 0` -- an effective
      floor of 8 -- and CUDA, HIP and SYCL had **no dimension floor at
      all**, halving to scale 3 unchecked. All four now derive the floor
      from `vif_get_min_dim()`, the same single source of truth the CPU
      uses, so the 8..15px range that walks the reflect-101 mirror out of
      the plane at scale 3 is rejected uniformly. `vif_tools.h` gained a
      `extern "C"` guard, without which the C++ (SYCL) and Objective-C++
      (Metal) translation units would demand mangled symbols and fail to
      link against the C `vif_tools.c`; it was previously included only by
      C translation units.
    - **`--help` and `--version` left the Windows console in UTF-8 + VT
      mode.** The `WindowsConsoleGuard` added for Netflix/vmaf#743 was an
      automatic local in `main`, and its comment claimed it restored "on
      every exit path". It did not: `cli_parse` terminates the process
      directly for `--help`, `--version` and every argument error
      (`usage_exit` is `[[noreturn]]` and calls `exit()`), and `exit()`
      does not destroy objects with automatic storage duration. Objects
      with static storage duration ARE destroyed by `exit()`
      ([basic.start.term]), so the guard is now `static` -- the restore
      runs on the `exit()` paths, on the `goto cleanup` spine and on a
      normal return alike. POSIX behaviour is unchanged (the whole block is
      `#ifdef _WIN32`).
    - **`scripts/ci/check-msvc-clz-shim.sh` was evadable by macro
      indirection.** Rules (1) and (4) keyed on the call syntax
      `__lzcnt(`, so `#define LZ __lzcnt` followed by `LZ(x)` -- or token
      pasting -- reintroduced the instruction while still passing the gate
      that exists to prevent exactly that. Both rules now match the bare
      identifier. Rule (4) is scoped to source extensions because
      `core/src/feature/AGENTS.md` legitimately discusses `__lzcnt` in
      prose. Negative-tested: macro indirection, a narrowed architecture
      allowlist, and a direct `__lzcnt` reintroduction all fail the gate.
    - **The static-link smoke test now links with the leg's own
      compiler.** It used bare `cc` while the matrix builds with
      `ccache gcc-14` / `ccache clang-22`, so it linked with a toolchain
      the archive was not produced with. Now `${CC:-cc}`. The review's
      accompanying LTO concern does not apply: `b_lto` is meson-default
      false here and explicitly `false` on the SYCL/CUDA legs, so the
      archive holds plain objects rather than LTO IR and no plugin
      mismatch is in play.
    - **Not a defect: the `Libs.private` libc++ detection.** The review
      held that keying on `_LIBCPP_VERSION` ignores an explicit
      `-stdlib=libc++`. Tested directly against the installed meson: a
      probe project reading `cxx.get_define('FOO')` under
      `-Dcpp_args=-DFOO=42` reports `42`, so compiler checks do observe
      the project's `cpp_args` and the `_LIBCPP_VERSION` probe therefore
      sees `-stdlib=libc++` exactly as its comment claims. No change made.

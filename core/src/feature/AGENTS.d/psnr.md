---
paths:
  - core/src/feature/psnr.c
  - core/src/feature/psnr.h
  - core/src/feature/float_psnr.c
  - core/src/feature/integer_psnr.c
  - core/src/feature/psnr_score.h
invariant: PSNR bucket lint shape, cross-backend enable_chroma parity, and uncapped options.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# PSNR Bucket Lint, Chroma Parity, and Uncapped Options

- **psnr bucket lint shape** (ADR-1142 ratchet, ADR-0278
  citations): [`psnr.c`](../psnr.c) includes its own
  [`psnr.h`](../psnr.h) — upstream does not, and include is
  what declares `compute_psnr()`'s external linkage to
  clang-tidy (alternative used for `compute_ssim` /
  `compute_ms_ssim` is NOLINT; psnr has real header, so it
  uses it). [`integer_psnr.c`](../integer_psnr.c) and
  [`float_psnr.c`](../float_psnr.c) carry
  `NOLINTNEXTLINE(misc-use-internal-linkage)` on
  `vmaf_fex_psnr` / `vmaf_fex_float_psnr` — same cross-TU
  registry pattern as `cambi.c` and `float_ssim.c` — and their
  `provided_features[]` sentinels plus `options[]`
  terminator keep upstream's `NULL` spelling: per
  [ADR-1138](../../../../docs/adr/1138-c-translation-units-keep-null.md) C
  translation unit never uses C23 `nullptr` keyword,
  because required `Build — Windows MSVC + CUDA` lane
  compiles these files with cl.exe and MSVC's documented
  `/std:clatest` feature set does not include it. Both files
  therefore carry file-scoped
  `/* NOLINTBEGIN(modernize-use-nullptr) … ADR-1138. */` …
  `NOLINTEND` bracket instead — keep it spanning whole
  file, and do not "modernise" `NULL`s inside it.
  [`psnr_tools.cpp`](../psnr_tools.cpp) is C++ and *does* use
  `nullptr`/designated initialisers; its `kFormatTable` peak /
  psnr_max values are byte-identical to upstream's `strcmp`
  ladder and must stay so — fork's
  `--feature psnr --precision=max` output on `src01` pair
  is asserted byte-identical across this refactor. See
  [ADR-1142](../../../../docs/adr/1142-whole-codebase-standards.md),
  [ADR-1138](../../../../docs/adr/1138-c-translation-units-keep-null.md),
  [ADR-0278](../../../../docs/adr/0278-t7-5-nolint-sweep.md)
  and [`docs/rebase-notes.md`](../../../../docs/rebase-notes.md).

- **`psnr` cross-backend `enable_chroma` option parity (ADR-0453)** —
  `psnr_cuda`, `psnr_sycl`, and `psnr_vulkan` now honour
  `enable_chroma` (default `true`) consistently with CPU reference.
  Passing `enable_chroma=false` produces luma-only output on all three
  GPU backends. option default must remain `true`; any change to
  default or `n_planes` clamp logic requires coordinated update
  across all three GPU twins. See CUDA AGENTS.md / Vulkan AGENTS.md
  invariant notes and [ADR-0453](../../../../docs/adr/0453-psnr-enable-chroma-gpu-parity.md).
- **`psnr` / `float_psnr` cross-backend `uncapped` option parity
  (ADR-1193)** — integer `psnr` and `float_psnr` extractors on CPU and
  all eight GPU twins (CUDA / SYCL / HIP / Metal x integer / float) carry
  opt-in `uncapped` boolean, default `false`. `psnr_max` has two roles:
  `mse == 0` infinity sentinel (unconditional, and what Netflix golden
  60 / 84 / 108 dB assertions pin) and truncation of genuinely computed
  values above it (dropped when `uncapped` is true). Two invariants:
  `!uncapped` arm must stay pre-ADR-1193 expression **verbatim** rather
  than re-derivation, because with `min_sse` below ~1.9e-11 ceiling
  rises past ~208 dB that zero MSE floored to 1e-16 produces.
  Re-derived `mse == 0 -> psnr_max` arm would move default score.
  Second invariant: option must **not** be
  `VMAF_OPT_FLAG_FEATURE_PARAM`, since CPU
  extractor appends without name dict while GPU twins append with
  one. Flagging it would make two backends emit different feature
  keys for same request. Adding it to one backend only is silent
  cross-backend divergence no CPU test catches. See
  [ADR-1193](../../../../docs/adr/1193-psnr-uncapped-option.md) and
  `core/test/test_psnr_uncapped.c`.

Two upstream-parity quirks are preserved on purpose: `float_psnr` leaves its
buffers to extractor teardown when bit depth is unsupported, and `ciede`
reports `-EINVAL` rather than `-ENOMEM` for same case. Both matched old label
ladders. Fixing either is behavioural change and needs own commit plus test.

- **APSNR clip SSE = 128 bits** (T-PSNR-APSNR-CLIP-SSE-UINT64-WRAP-2026-10-05).
  `apsnr` sums every frame's SSE of a plane into `VmafPsnrClipSse`
  (`psnr_score.h`: `vmaf_psnr_clip_sse_add()` carries into `hi`), on the CPU
  and in the CUDA, HIP, SYCL and Metal hosts. Frame SSE < 2^62, clip sum is
  not: a plain `uint64_t` wrapped at frame 33 of 16K, 16 bit, max diff.
  `vmaf_psnr_aggregate()` reads `(double)lo` while `hi == 0`, so old clips keep
  their bits. Never bring back `uint64_t apsnr_sse` / `+= sse`. Guards:
  `test_integer_psnr_coverage` (`test_psnr_apsnr_clip_sse_past_two_pow_64`),
  `test_{cuda,hip}_kernel_source_contract.py`,
  `test_metal_integer_psnr_exact_contract.py`.

---
paths:
  - core/src/feature/vif.c
  - core/src/feature/psnr.c
invariant: integer_vif is luma-only across backends; psnr_max separates scale from peak; float VIF avoids AVX-512.
area: backends-metrics-and-tests
---
<!-- markdownlint-disable MD013 MD060 -->
# PSNR and VIF metric contracts across backends

- **`integer_vif` is luma-only across every backend** (fork-local,
  [ADR-0541](../../docs/adr/0541-integer-vif-luma-only-clarification.md)).
  CPU [`src/feature/integer_vif.c`](../src/feature/integer_vif.c) reads
  `data[0]` only, has no `enable_chroma` option; CUDA
  [`src/feature/cuda/integer_vif_cuda.c`](../src/feature/cuda/integer_vif_cuda.c)
  hardcodes `s->n_planes = 1`, warn-on-trues `enable_chroma`; HIP,
  SYCL, and Metal twins all match. Upstream Netflix/vmaf is
  same. VIF (Sheikh & Bovik, 2006) is defined on single luminance
  channel — multi-plane VIF has no MOS-correlation literature. Never
  "fix" `n_planes = 1` or "wire enable_chroma through" without
  filing fresh ADR including research digest and golden-data
  regeneration plan; previous attempt (PRs #948 + #949, 2026-05-16)
  was abandoned, left vestigial CUDA `enable_chroma` option as
  only artefact. Regression test
  [`test/test_integer_vif_cpu_cuda_parity.c`](../test/test_integer_vif_cpu_cuda_parity.c)
  asserts CPU vs CUDA scale parity and `enable_chroma=true` bit-identity
  with default invocation — both must keep passing.
- **Vulkan PSNR chroma contract** (fork-local, [ADR-0216](../../docs/adr/0216-vulkan-chroma-psnr.md)).
  [`src/feature/vulkan/psnr_vulkan.c`](../src/feature/vulkan/psnr_vulkan.c)
  carries `ref_in[3] / dis_in[3] / se_partials[3]` arrays in
  `PsnrVulkanState` (Y / Cb / Cr), dispatches same
  `psnr.comp` shader once per active plane in single command
  buffer. Shader is plane-agnostic — reads
  `(width, height, num_workgroups_x)` from push constants. Rebases
  "simplifying" chroma loop back to single luma dispatch will
  silently regress `psnr_cb` / `psnr_cr` to CPU fall-through.
  This also breaks `cross_backend_vif_diff.py
  --feature psnr` gate, which now asserts on Y / Cb / Cr. YUV400
  is only supported `n_planes = 1` path; `pix_fmt`
  branch in `init` mirrors `enable_chroma = false` clamp in
  CPU `integer_psnr.c::init`, must follow it on any future
  `min_sse` / `psnr_max[p]` divergence. Descriptor pool is
  sized for 12 sets (4 frames in flight × 3 planes) — never
  shrink without re-checking lavapipe behaviour under
  frames-in-flight > 1.
- **PSNR `psnr_max` has two separate roles**
  (fork-local since [ADR-1193](../../docs/adr/1193-psnr-uncapped-option.md)).
  Role (a): finite stand-in reported when `mse == 0` and true
  PSNR is `+inf` — unconditional, and what Netflix golden 60 / 84 /
  108 dB assertions pin. Role (b): truncation of computed values at
  same number — applied only when `uncapped` option is `false`.
  Upstream conflates two in one
  `MIN(10*log10(peak^2 / MAX(mse, 1e-16)), psnr_max)`, so verbatim
  upstream hunk landing on `feature/integer_psnr.c` (arm now lives in
  `feature/psnr_score.h::vmaf_psnr_from_mse()`, shared with GPU twins,
  [ADR-1365](../../docs/adr/1365-sycl-twin-cpu-option-parity.md)),
  `feature/float_psnr.c::extract()` or `feature/psnr.c::compute_psnr()`
  silently reintroduces Netflix/vmaf#1109. `!uncapped` arm is that
  upstream expression, `MIN` / `MAX` macro-expanded in place, and must
  stay that way:
  with `min_sse` below ~1.9e-11 ceiling rises past ~208 dB
  floored zero MSE produces, so re-derived `mse == 0 -> psnr_max`
  default would not be bit-identical there. Never merge two
  computed arms. `uncapped` option name,
  `VMAF_OPT_TYPE_BOOL` type and `false` default are mirrored across ten
  extractors — two CPU ones plus all eight GPU twins — must move
  together. It is deliberately **not**
  `VMAF_OPT_FLAG_FEATURE_PARAM`: CPU extractor appends without
  name dict while twins append with one, so flagging it would make
  backends emit different feature keys for same request.
  `core/test/test_psnr_uncapped.c` guards both directions (default
  must still report 60.0; `uncapped=true` must report 100.840479).
  Option coverage: CUDA, HIP, Metal twins implement only
  `enable_chroma` and `uncapped`; `psnr_sycl` implements full CPU
  table (ADR-1365). Peak, `psnr_max` (`min_sse`), MSE -> PSNR and
  APSNR aggregate = `feature/psnr_score.h`, one implementation for
  CPU `integer_psnr.c` and twins: twin reduces SSE on device, calls
  these on host. Upstream change to that math -> edit header, not
  copy in one extractor.

- **Float VIF must NOT dispatch to AVX-512 (ADR-1104)**: `vif_filter1d_s`,
  `vif_filter1d_sq_s`, and `vif_filter1d_xy_s` in `core/src/feature/vif_tools.c`
  dispatch to AVX2 or scalar only. AVX-512 float convolution path
  (`convolution_f32_avx512_{s,sq_s,xy_s}`) produces different IEEE-754 rounding
  than AVX2 (wider 512-bit FMA partial-sum tree), causing Netflix golden
  VMAFEXEC assertion (`76.66740433333332`, `places=4`) to fail on AVX-512 CPUs.
  Any future PR re-adding `#if HAVE_AVX512` dispatch to these three functions
  must demonstrate golden assertion still passes on AVX-512 hardware,
  must update ADR-1104. Integer VIF AVX-512 path (`vif_avx512.c`) is
  unaffected, must remain enabled.

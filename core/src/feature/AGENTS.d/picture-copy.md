---
paths:
  - core/src/feature/picture_copy.cpp
  - core/src/feature/picture_copy.h
invariant: picture_copy channel selection and high-bit-depth sample normalization contracts.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# picture_copy Channel Parameter and Normalization

## `picture_copy()` carries a `channel` parameter

Upstream commit `d3647c73` (T-NEW-1, ported via this fork's
`upstream/port-d3647c73-feature-speed`) widened
`picture_copy()` / `picture_copy_hbd()` signatures with new
`int channel` argument so new `speed_chroma` and
`speed_temporal` extractors can lift U / V planes from
`VmafPicture`. Every fork-local extractor that calls
`picture_copy()` (`cuda/integer_ms_ssim_cuda.c`,
`vulkan/ssim_vulkan.c`, `vulkan/ms_ssim_vulkan.c`) passes
`channel=0`; upstream-mirror `float_*` callers already do.
**If future upstream commit evolves signature further
(extra parameter, type change): update those four fork-local
call sites in lockstep with upstream-mirror ones.** Silently
trailing upstream signature change fails compilation on any
GPU backend. See
[`docs/rebase-notes.md` §0075](../../../../docs/rebase-notes.md).

## High-bit-depth samples are normalised before accumulation (ADR-1212)

`picture_copy()` divides every 10/12/16-bit sample by 4 / 16 / 256 before
float extractors see it, so CPU "sum of samples" is sum of *normalised*
samples. GPU twin that reads raw plane and accumulates codewords must
apply that scaler itself — on host, to exact integer sums, which is
bit-identical to CPU at 10 and 12 bpc. `float_moment` on CUDA, SYCL and HIP
shipped without it and was 4x–256x off above 8 bpc; nothing caught it because
every parity fixture was 8-bit. Register `-DFIXTURE_BPC=10u` variant of any
new parity test whose extractor consumes samples.

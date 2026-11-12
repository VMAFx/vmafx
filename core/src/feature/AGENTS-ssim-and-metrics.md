<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# SSIM, CAMBI, SpEED, and metrics: `core/src/feature/` agent invariants

ssim*, ms_ssim*, ssimulacra2*, cambi*, speed*, ciede.c, delta_e_itp.c,
brisque*, niqe*, y_funque_plus.c, feature_lpips.c, feature_dists.c,
feature_mobilesal.c, fastdvdnet_pre.c, and transnet_v2*.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `brisque.c`, `brisque_math.h` | [brisque](AGENTS.d/brisque.md) | BRISQUE MATLAB pipeline parity and numerical stability assertions. |
| `cambi.c`, `cuda/integer_cambi_cuda.c`, `hip/integer_cambi_hip.c` | [cambi-gpu](AGENTS.d/cambi-gpu.md) | CAMBI GPU twins mirror host-side semantics and hybrid host/GPU dispatch contracts. |
| `cambi.c`, `cambi.h`, `/core/test/test_cambi_full_ref_wide_source.c`, `/core/test/test_cambi_heatmap_writers.c` | [cambi](AGENTS.d/cambi.md) | CAMBI bounded searches, c-values window boundaries, row-by-row 10-bit copies, and UTF-8 heatmap paths. |
| `ciede.c`, `/core/test/test_ciede.c` | [ciede](AGENTS.d/ciede.md) | CIEDE chroma-upsample subsample flags diverge from upstream to ensure correctness. |
| `feature_lpips.c`, `feature_dists.c` | [deep-metrics](AGENTS.d/deep-metrics.md) | LPIPS and DISTS tiny-AI metric contracts and high-bit-depth normalization. |
| `delta_e_itp.c` | [delta-e-itp](AGENTS.d/delta-e-itp.md) | Delta E ITP PQ-only transfer characteristics and color space invariants. |
| `fastdvdnet_pre.c` | [fastdvdnet](AGENTS.d/fastdvdnet.md) | FastDVDnet 5-frame-window buffering and temporal pre-filter lifecycle contracts. |
| `feature_mobilesal.c`, `/core/test/test_mobilesal.c`, `/core/test/dnn/test_mobilesal_run.c` | [mobilesal](AGENTS.d/mobilesal.md) | MobileSal extractor pads sides to multiple of 8 for students and averages only frame's own area. |
| `ms_ssim.c`, `ms_ssim.h`, `float_ms_ssim.c` | [ms-ssim](AGENTS.d/ms-ssim.md) | MS-SSIM decimate LPF coefficients, mirror bounds, -ENOMEM handling, and SIMD parity. |
| `niqe.c`, `niqe_math.h` | [niqe](AGENTS.d/niqe.md) | NIQE fork-pkl parity, sharpness calculation, and model coefficient loading. |
| `speed_internal.c`, `speed_internal.h` | [speed-internal](AGENTS.d/speed-internal.md) | speed_internal shared helper TU wiring, matrix multiplication dispatch, and CUDA dependencies. |
| `speed.c`, `speed_cov.h`, `speed_givens.h`, `/core/test/test_speed_upstream_form.c` | [speed](AGENTS.d/speed.md) | SpEED buffer allocation, chroma dimensions, anti-alias decimation, covariance sums, Netflix's fp64 expressions. |
| `ssim.c`, `ssim.h`, `float_ssim.c` | [ssim](AGENTS.d/ssim.md) | Integer SSIM samplemax² widening, fork-side registration, and pthread_once dispatch. |
| `ssimulacra2_pixel_format.h`, `ssimulacra2.c`, `cuda/ssimulacra2_cuda.c`, `sycl/ssimulacra2_sycl.cpp`, `hip/ssimulacra2_hip.c`, `metal/ssimulacra2_metal.mm`, `/core/test/test_ssimulacra2_pixel_format.c`, `/core/test/test_ssimulacra2_pixel_format_contract.py` | [ssimulacra2-pixel-format](AGENTS.d/ssimulacra2-pixel-format.md) | Every ssimulacra2 init() refuses 4:0:0 first, through vmaf_ss2_check_pixel_format(); no private check. |
| `ssimulacra2.c`, `/core/test/test_ssimulacra2_simd.c` | [ssimulacra2](AGENTS.d/ssimulacra2.md) | SSIMULACRA 2 regression gates, linear RGB conversion, blur, and SIMD bit-exactness. |
| `transnet_v2.c`, `transnet_v2_score.h`, `/core/test/dnn/test_transnet_v2_run.c` | [transnet-v2](AGENTS.d/transnet-v2.md) | TransNet V2 runs upstream predict_frames() windows on 0..255 thumbnails and binds its one output by position. |
| `y_funque_plus.c` | [y-funque-plus](AGENTS.d/y-funque-plus.md) | Y-FUNQUE+ atoms-only feature calculation and decomposition invariants. |

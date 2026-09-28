<!-- markdownlint-disable MD013 -->
# Research-2120: SYCL ciede2000 throughput — 2026-09-29

- **Status**: Active
- **Workstream**: RC3 performance ([ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1352](../adr/1352-rc-phase-shift-plus-one.md)); no ADR (implementation detail that matches the CUDA and HIP twins)
- **Last updated**: 2026-09-29

## Question

The RC3 benchmark recorded `--no_prediction --feature ciede` at 1619 ms per
frame on an Intel Arc B580 through SYCL, against 219 ms per frame on 16 CPU
threads (BBB 3840x2160, 8-bit 4:2:0, `t(22) - t(2)` over 20 frames). The UHD
770 was about as slow. Why is the SYCL path seven times slower than the CPU, and
what does `ciede_sycl` itself cost?

## Sources

- `core/tools/vmaf.cpp` `register_cli_feature()` and `core/src/libvmaf.c`
  `vmaf_use_feature()` / `vmaf_use_features_from_model()`: extractor lookup.
- `core/src/feature/sycl/integer_ciede_sycl.cpp` before and after this change.
- The CUDA and HIP twins: `core/src/feature/cuda/integer_ciede/ciede_score.cu`,
  `core/src/feature/hip/ciede_hip.c`
  ([ADR-1213](../adr/1213-hip-ciede-chroma-ceil-dimensions.md) for the ceil
  chroma size).
- The earlier observation in `docs/state.md` `T-SYCL-V1-MODEL-SEGFAULT-2026-09-04`
  that only a model reaches the SYCL twins.

## Findings

### The 1619 ms figure was the CPU extractor

`--feature ciede --backend sycl` never ran SYCL code. `register_cli_feature()`
passes the name to `vmaf_use_feature()`, which looks it up with
`vmaf_get_feature_extractor_by_name()`: an exact-name match that returns the
CPU `ciede` extractor. Only model features go through
`vmaf_get_feature_extractor_by_feature_name(name, fex_flags)` and reach a twin.
`--backend sycl` initialises the device and nothing is dispatched to it. With no
`--threads` flag, `CLISettings::thread_cnt` is 0 and
`vmaf_ctx_thread_pools_init()` creates no pool, so the CPU extractor ran one
frame at a time. The JSON still reported `"backend_used": "sycl"`.

Evidence, all on the same build and fixtures:

| Command (4K, 8-bit 4:2:0) | ms/frame |
| --- | --- |
| `--feature ciede --backend cpu --threads 16` | 208 |
| `--feature ciede --backend sycl`, B580 | 1714 |
| `--feature ciede --backend sycl`, UHD 770 | 1725 |
| `--feature ciede_sycl --backend sycl`, B580 | 17.2 |
| `--feature ciede_sycl --backend sycl`, UHD 770 | 52.9 |

A build that logs every `ciede_sycl` submit printed nothing for
`--feature ciede --backend sycl` and one line per frame for
`--feature ciede_sycl`. The UHD 770 has no fp64 (`SYCL: device lacks native
fp64` at start-up) and runs the module, so IGC emits no fp64 for this TU
([ADR-0220](../adr/0220-sycl-fp64-fallback.md)).

This affects every `--feature <cpu-name> --backend <gpu>` measurement, not only
ciede. It is filed as `T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29`; the
fix needs a decision on whether the CLI or `vmaf_use_feature()` should resolve
names to twins.

### Where `ciede_sycl` spent its frame

Phase times with a wait after each phase, 4K, steady-state frames:

| Phase | B580 before | B580 after | UHD 770 after |
| --- | --- | --- | --- |
| host chroma upscale / packing | 9.5 ms | 1.9 ms | 2.4 ms |
| H2D copy | 4.0 ms | 0.8 ms | 1.3 ms |
| kernel | 1.2 ms | 1.2 ms | 21 to 35 ms |
| D2H partials | 0.1 ms | 0.1 ms | 0.2 ms |

Before the change, `submit()` upscaled U and V to luma resolution on the host
with a per-pixel loop into six luma-size host-USM planes and copied all six:
50 MB per 4K frame. The kernel was 1.2 ms. The CUDA and HIP twins already read
native-resolution chroma at `(x >> ss_hor, y >> ss_ver)`.

Now `stage_plane()` packs each plane at its native size (one `memcpy` when the
picture stride equals the row size), and the kernel reads chroma at
`(x >> ss_hor, y >> ss_ver)`. A 4:2:0 frame moves 25 MB. The chroma size uses
`picture.c`'s ceil rule, so odd widths stage every column. The per-pixel inputs
are the same samples the upscale produced, so the output is bit-identical to
the old path: 48 of 48 Netflix frames and 50 of 50 BBB 4K frames at
`--precision max` on both GPUs. Against the CPU the worst per-frame difference
stays 1.18e-5 (Netflix pair) and 9.72e-5 (4K), well inside the 5e-3 ciede
tolerance in `scripts/ci/cross_backend_parity_gate.py`.

End to end (`t(N2) - t(2)`, median of three; the 576x324 pair repeated 10
times gives 480 frames, measured over 400):

| Device | 4K before | 4K after | 576x324 before | 576x324 after |
| --- | --- | --- | --- | --- |
| Arc B580 | 17.2 ms | 8.4 ms | 0.49 ms | 0.37 ms |
| UHD 770 | 52.9 ms | 46.0 ms | 2.16 ms | 1.96 ms |
| CPU, 16 threads (`ciede`) | 208 ms | 201 ms | 3.96 ms | 4.07 ms |

On the B580 the extractor now uses about 4 ms of the 8.4 ms frame; the rest is
YUV reading and picture handling in the CLI. The UHD 770 is bound by the kernel.

## Alternatives explored

- **Keep the host upscale and vectorise it.** It still writes, and then copies,
  luma-size chroma: twice the bytes of the native planes. Rejected because the
  CUDA and HIP twins already show the kernel-side index costs nothing measurable.
- **Copy straight from the `VmafPicture` without staging.** The pictures are
  pageable and the CLI refills them after `submit()` returns; the HIP twin had
  that race (`T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18`). Safe only with a wait on
  the copy inside `submit()`, which costs about what staging does.
- **Cheaper transcendentals** (`pow(x, 2.0f)` as `x * x`, a constant for
  `pow(25, 7)`, `powr` for the positive `pow(x, 2.4f)` base). This would help
  the UHD 770, which is kernel-bound, but changes the fp32 results, so it needs
  its own parity evidence against the "no worse than today" bar. Not done here.

## Open questions

- Whether the CLI or `vmaf_use_feature()` should map `--feature ciede` to the
  active backend's twin (`T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29`).
- How much of the UHD 770 kernel time the transcendental calls take, and why it
  varies between 21 and 35 ms from frame to frame (shared power budget with the
  CPU is the likely cause; not measured).
- The Metal twin still upscales on the host
  (`T-METAL-CIEDE-HOST-UPSCALE-2026-09-29`); not measured on Apple hardware.

## Related

- State rows: `T-SYCL-CIEDE-HOST-UPSCALE-2026-09-29` (closed),
  `T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29`,
  `T-METAL-CIEDE-HOST-UPSCALE-2026-09-29`.
- ADRs: [ADR-0220](../adr/0220-sycl-fp64-fallback.md),
  [ADR-0543](../adr/0543-adr-0498-enforcement-hardening.md),
  [ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md),
  [ADR-1213](../adr/1213-hip-ciede-chroma-ceil-dimensions.md).

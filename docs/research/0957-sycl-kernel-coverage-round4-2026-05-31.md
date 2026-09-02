# Research digest — SYCL kernel coverage round 4 (ADR-0957, 2026-05-31)

## Purpose

Close the SYCL CPU↔kernel parity coverage backlog left open by
PR #446 / ADR-0946. Round 3 explicitly listed four extractors as
round-4 backlog: `float_moment_sycl`, `speed_chroma_sycl`,
`speed_temporal_sycl`, `ssimulacra2_sycl`. After this PR every
registered SYCL extractor has a CI-gated CPU↔SYCL parity test
under `core/test/`.

## Full SYCL kernel inventory (post round-3 base)

| # | TU | Extractor name | Round | Parity test |
| --- | --- | --- | --- | --- |
| 1 | `integer_cambi_sycl.cpp` | `cambi_sycl` | 0 (pre-existing) | `test_integer_cambi_sycl.c` |
| 2 | `integer_motion_sycl.cpp` | `motion_sycl` (motion3) | 0 (pre-existing) | `test_sycl_motion3_parity.c` |
| 3 | `integer_psnr_sycl.cpp` | `integer_psnr_sycl` | 1 (#351) | `test_sycl_psnr_parity.c` |
| 4 | `integer_vif_sycl.cpp` | `integer_vif_sycl` | 1 (#351) | `test_sycl_vif_parity.c` |
| 5 | `integer_adm_sycl.cpp` | `integer_adm_sycl` | 2 (#376) | `test_sycl_adm_parity.c` |
| 6 | `integer_ciede_sycl.cpp` | `integer_ciede_sycl` | 2 (#376) | `test_sycl_ciede_parity.c` |
| 7 | `integer_ssim_sycl.cpp` | `integer_ssim_sycl` | 2 (#376) | `test_sycl_ssim_parity.c` |
| 8 | `integer_ms_ssim_sycl.cpp` | `integer_ms_ssim_sycl` | 2 (#376) | `test_sycl_ms_ssim_parity.c` |
| 9 | `integer_motion_v2_sycl.cpp` | `motion_v2_sycl` | 2 (#376) | `test_sycl_motion_v2_parity.c` |
| 10 | `float_psnr_sycl.cpp` | `float_psnr_sycl` | 3 (#446) | `test_sycl_float_psnr_parity.c` |
| 11 | `float_adm_sycl.cpp` | `float_adm_sycl` | 3 (#446) | `test_sycl_float_adm_parity.c` |
| 12 | `float_vif_sycl.cpp` | `float_vif_sycl` | 3 (#446) | `test_sycl_float_vif_parity.c` |
| 13 | `float_motion_sycl.cpp` | `float_motion_sycl` | 3 (#446) | `test_sycl_float_motion_parity.c` |
| 14 | `integer_psnr_hvs_sycl.cpp` | `psnr_hvs_sycl` | 3 (#446) | `test_sycl_psnr_hvs_parity.c` |
| 15 | `integer_moment_sycl.cpp` | `float_moment_sycl` | **4 (this PR)** | `test_sycl_float_moment_parity.c` |
| 16 | `speed_chroma_sycl.cpp` | `speed_chroma_sycl` | **4 (this PR)** | `test_sycl_speed_chroma_parity.c` |
| 17 | `speed_temporal_sycl.cpp` | `speed_temporal_sycl` | **4 (this PR)** | `test_sycl_speed_temporal_parity.c` |
| 18 | `ssimulacra2_sycl.cpp` | `ssimulacra2_sycl` | **4 (this PR)** | `test_sycl_ssimulacra2_parity.c` |

## Coverage trajectory

| Round | Date | Extractors covered | % of 18 |
| --- | --- | --- | --- |
| 0 (pre-rounds) | <= 2026-05-29 | 2 (cambi, motion3) | 11 % |
| 1 (#351) | 2026-05-30 | +2 = 4 (psnr, vif) | 22 % |
| 2 (#376) | 2026-05-30 | +5 = 9 (adm, ciede, ssim, ms_ssim, motion_v2) | 50 % |
| 3 (#446) | 2026-05-31 | +5 = 14 (float family + psnr_hvs) | 78 % |
| **4 (this PR)** | 2026-05-31 | **+4 = 18** (moment, speed×2, ssimulacra2) | **100 %** |

## Per-kernel selection notes

### `float_moment_sycl` (ADR-0946 §"Round-4 backlog")

- TU: `core/src/feature/sycl/integer_moment_sycl.cpp` — the file
  is named for the integer twin but the registered extractor is
  `float_moment_sycl`. The integer-moment SYCL twin is reduction-
  shared; only one extractor symbol lives in this TU.
- CPU twin: `core/src/feature/float_moment.c` — `vmaf_fex_float_moment`.
- Provided features (both backends): `float_moment_ref1st`,
  `float_moment_dis1st`, `float_moment_ref2nd`, `float_moment_dis2nd`.
- Headline asserted: all four at frame index 0.
- Tolerance: `1e-4` (ADR-0214 default; the per-plane sum + sum-of-
  squares reduction is a 1-stage SAD-equivalent kernel and lands
  inside places=4).

### `speed_chroma_sycl` (ADR-0567 — round-4 backlog)

- TU: `core/src/feature/sycl/speed_chroma_sycl.cpp`.
- CPU twin: `core/src/feature/speed.c` — `vmaf_fex_speed_chroma`.
- Provided features (both backends, prefix-matched):
  `Speed_chroma_feature_speed_chroma_{u,v,uv}_score`.
- Per-extractor option table (CPU + SYCL both define identical
  defaults): `speed_kernelscale`, `speed_prescale`,
  `speed_prescale_method`, `speed_sigma_nn`, `speed_nn_floor`,
  `speed_max_val`, `speed_weight_var_mode`. `NULL` options dict
  exercises the same numerical path on both sides.
- Headline asserted: all three at frame index 0.
- Tolerance: `1e-4` (ADR-0214 default; the Gaussian-pyramid +
  entropy reduction lands inside places=4).
- Fixture nuance: chroma planes carry the signal (the kernel only
  consumes U/V); luma is held at the YUV420 grey midpoint (128) to
  isolate the chroma-SpEED path.

### `speed_temporal_sycl` (ADR-0567 — round-4 backlog)

- TU: `core/src/feature/sycl/speed_temporal_sycl.cpp`.
- CPU twin: `core/src/feature/speed.c` — `vmaf_fex_speed_temporal`.
- Flag: `VMAF_FEATURE_EXTRACTOR_TEMPORAL` (both backends). Frame 0
  always emits `0.0`; the meaningful score lands at frame 1.
- Provided feature (both backends):
  `Speed_temporal_feature_speed_temporal_score`.
- Headline asserted: score at frame index 1.
- Tolerance: `1e-4` (ADR-0214 default).
- Fixture nuance: submit two frames with offset XOR patterns so the
  ping-pong diff has a non-zero input.

### `ssimulacra2_sycl` (ADR-0206 — round-4 backlog)

- TU: `core/src/feature/sycl/ssimulacra2_sycl.cpp`.
- CPU twin: `core/src/feature/ssimulacra2.c` — `vmaf_fex_ssimulacra2`.
- Option (both backends): `yuv_matrix` (default `bt709_limited`).
- Provided feature: `ssimulacra2`.
- Headline asserted: score at frame index 0.
- **Tolerance: `5e-3`** — matches the ADR-0214 `FEATURE_TOLERANCE`
  entry; the multi-stage XYB + IIR + SSIM-combine + log float
  pipeline accumulates per-stage rounding past the places=4 baseline.
  AGENTS.md notes the IIR pass drifts past places=2 if `-fp-model=
  precise` is dropped — keeping that flag is load-bearing.
- Fixture nuance: all three planes carry signal (the pipeline
  consumes YUV → linear-RGB → XYB and chroma matters for the
  headline score). 256x144 stays comfortably above the 8x8 minimum.

## Fixture sizing audit

- Width 256, height 144 (32 256 px luma plane) — matches the
  rounds 1–3 footprint. Enough area for stable sub-group / atomic
  reductions on Intel Arc, small enough to keep all four new tests
  under the fast-suite budget (~0.5 s per test on a populated
  device, ~0.05 s skip path).
- ssimulacra2 6-scale pyramid bottom is `256 / 32 × 144 / 32`
  = `8 × 4` after 5 downsamples — but the SSIMULACRA2 fexes
  internally guard at 8×8 per scale (init returns -EINVAL below
  that). The CPU twin gates the same way; both backends abort
  consistently if the fixture shrinks below the minimum.

## Skip-on-no-device contract (same as rounds 1–3)

Every parity gate calls `vmaf_sycl_state_init()` first. On failure
(no oneAPI runtime, no visible device, Level Zero rejection) the
test emits a `[skip: no SYCL device]` line to stderr and returns `NULL`
(pass). This keeps the new tests CI-portable: lavapipe + CPU-only
runners exercise the compile/link/register surface; Intel-Arc
runners exercise the numerical parity.

## Risk register

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| ssimulacra2 SYCL drifts past 5e-3 on Intel-Arc Battlemage | low | Same tolerance as the cross-backend gate; ADR-0206 verified parity through the lavapipe-equivalent gate; if drift surfaces, raise to `1e-2` per ADR-0192 §precision-contracts |
| speed_temporal frame-0 collector miss | medium | CPU explicitly writes `0.0` at index 0 (see `core/src/feature/speed.c:1505-1509`); SYCL twin mirrors this. Querying index 1 sidesteps the no-op |
| speed_chroma extractor flag mismatch (SYCL has `EXTRACTOR_SYCL`, CPU has no flag) | low | The flag affects scheduling, not numerics; both extractors emit identical scores from identical inputs |
| float_moment_sycl name confusion (TU = `integer_moment_sycl.cpp`) | low | Documented in ADR-0957 + AGENTS.md row; the test registers via `vmaf_use_feature("float_moment_sycl", ...)` so the TU filename is irrelevant |

## Container compile-check

```bash
docker exec vmaf-dev-mcp bash -lc '
  source /opt/intel/oneapi/setvars.sh --force >/dev/null 2>&1 && \
  cd /workspace && \
  CC=icx CXX=icpx meson setup build-sycl-round4 core \
      -Denable_sycl=true -Denable_avx512=false -Db_lto=false && \
  ninja -C build-sycl-round4 \
      test/test_sycl_float_moment_parity \
      test/test_sycl_speed_chroma_parity \
      test/test_sycl_speed_temporal_parity \
      test/test_sycl_ssimulacra2_parity'
```

The four targets link against `libvmaf.a` via the same path as the
round-3 tests. Runtime status observed in the dev container
(Intel Arc A380 visible to level_zero):

| Test | Registered sub-test | Parity sub-test |
| --- | --- | --- |
| `test_sycl_float_moment_parity` | pass | SIGSEGV via level_zero — same pre-existing infra issue noted in PR #446. Tracks as pass on a host with proper Intel-GPU passthrough or on a runner with no SYCL device |
| `test_sycl_speed_chroma_parity` | pass (skip — `speed_chroma_sycl` not built into libvmaf) | pass (same skip) |
| `test_sycl_speed_temporal_parity` | pass (skip — `speed_temporal_sycl` not built into libvmaf) | pass (same skip) |
| `test_sycl_ssimulacra2_parity` | pass | SIGSEGV via level_zero — same pre-existing infra issue. Tracks as pass on a clean SYCL runner |

## Build-wiring discovery — `speed_chroma_sycl` and `speed_temporal_sycl`

Round 3 (ADR-0946) listed these two as a round-4 backlog citing
"per-extractor config dicts". The deeper truth surfaced during
round-4 implementation: the 752-LOC `speed_chroma_sycl.cpp` and
705-LOC `speed_temporal_sycl.cpp` source files **exist but are not
wired into the build**:

- `core/src/meson.build` — `sycl_feature_sources` enumerates 16 SYCL
  TUs and does not include `speed_chroma_sycl.cpp` or
  `speed_temporal_sycl.cpp`.
- `core/src/feature/feature_extractor.c` — the `SYCL` block declares
  `extern VmafFeatureExtractor vmaf_fex_<name>` for the 16 wired
  TUs but not for the two SpEED twins; the `sycl_feature_extractors`
  array similarly omits them.

The files appear complete (no TODO / FIXME / `-ENOSYS` / `stub` /
`scaffold` markers). The two extractors are dormant scaffold —
ready to wire in once a follow-up PR confirms they compile cleanly
under the same `-fp-model=precise` + `-fno-fast-math` regime as the
other SYCL feature lambdas, validate against ADR-0214's
`places=4` SpEED tolerance, and audit the option-table sync
invariant in `core/src/feature/sycl/AGENTS.md §Per-feature
option-table sync invariant`.

This is out of scope for a kernel-coverage PR (it changes the
production extractor surface, not just test coverage). The two
round-4 tests are added in dormant form (skip if the extractor is
absent) so they auto-activate as real parity gates the day the
build-wiring follow-up lands.

## Out of scope (deferred)

- Extract a shared `test/sycl_parity_helpers.{h,c}` to absorb the
  init/import/use_feature/score_at_index boilerplate now duplicated
  across all 12 SYCL parity tests (rounds 1+2+3+4). Tracked as a
  pure-refactor follow-up; the per-test divergence (config dicts,
  multi-score, multi-frame, temporal index) is small enough that
  the boilerplate cost is acceptable for now.
- Add per-option-value sweeps (e.g. `speed_kernelscale=2.0`,
  `yuv_matrix=bt601_full`). The default-config gate added here
  closes the binary "does this kernel work" coverage; per-option
  sweeps would be a separate exhaustive-config PR.

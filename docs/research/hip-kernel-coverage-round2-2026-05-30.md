<!-- markdownlint-disable MD060 -->
# HIP kernel parity coverage — round 2 audit (2026-05-30)

Companion research digest for ADR-0883.  Quantifies the HIP-side coverage
gap remaining after ADR-0868 / PR #351 round-1, ranks the residual
extractors by user-visible impact, and justifies the 5-test selection
that round-2 ships.

## Inventory

`find core/src/feature/hip -maxdepth 1 -name '*_hip.c'` returns
**17 HIP extractor source files** (one extractor per file, plus the
`hip_hsaco_stubs.c` build-only entry that registers no extractor).

| Extractor source | Registered name | Tested before round-2? | Round-2 ships test? |
|---|---|---|---|
| `integer_adm_hip.c` | `adm_hip` | yes — `test_hip_adm_parity` (ADR-0539) | — |
| `integer_motion_v2_hip.c` | `motion_v2_hip` | yes — `test_hip_motion3_parity` | — |
| `integer_psnr_hip.c` | `psnr_hip` | yes — `test_hip_psnr_parity` (PR #351) | — |
| `integer_vif_hip.c` | `vif_hip` | yes — `test_hip_vif_parity` (PR #351) | — |
| `ciede_hip.c` | `ciede_hip` | **no** | **yes** — `test_hip_ciede_parity` |
| `integer_psnr_hvs_hip.c` | `psnr_hvs_hip` | **no** | **yes** — `test_hip_psnr_hvs_parity` |
| `integer_motion_hip.c` | `motion_hip` (v1) | **no** | **yes** — `test_hip_motion_parity` |
| `integer_ssim_hip.c` | `integer_ssim_hip` | **no** | **yes** — `test_hip_ssim_parity` |
| `integer_ms_ssim_hip.c` | `integer_ms_ssim_hip` | **no** | **yes** — `test_hip_ms_ssim_parity` |
| `integer_cambi_hip.c` | `cambi_hip` | **no** | deferred — round 3 (CPU `cambi` needs encoded-bitrate metadata) |
| `ssimulacra2_hip.c` | `ssimulacra2_hip` | **no** | deferred — PR #290 actively refactoring this file |
| `speed_chroma_hip.c` | `speed_chroma_hip` | **no** | deferred — round 3 (CPU `speed_chroma` has no scalar reference yet) |
| `speed_temporal_hip.c` | `speed_temporal_hip` | **no** | deferred — round 3 (same) |
| `float_adm_hip.c` | `float_adm_hip` | **no** | deferred — float-pipeline rework upstream of ADR-0119 |
| `float_motion_hip.c` | `float_motion_hip` | **no** | deferred — float-pipeline rework |
| `float_psnr_hip.c` | `float_psnr_hip` | **no** | deferred — float-pipeline rework |
| `float_ssim_hip.c` | `float_ssim_hip` | **no** | deferred — float-pipeline rework |
| `float_moment_hip.c` | `float_moment_hip` | **no** | deferred — internal helper, no public CPU twin |

Coverage delta: **4 / 17 → 9 / 17** (24 % → 53 %) parity-gated HIP
extractors after this PR.

## Selection rationale

The 5 round-2 picks were ranked on three axes:

1. **Score appearance in libvmaf-2.x.x default model** — `ssim`, `ms_ssim`,
   `ciede2000`, `psnr_hvs`, and `motion` all surface as feature columns
   the default predictor consumes either directly or via aggregation.
2. **CHUG re-extract column coverage** — every CHUG run materialises these
   5 features per clip; a silent HIP regression here would propagate into
   model training data.
3. **Kernel implementation maturity** — chose only extractors with a stable
   CPU reference and a stable HIP path (no in-flight refactor PRs).  This
   excludes `ssimulacra2_hip` (PR #290) and the float-pipeline family
   (waiting on ADR-0119 successor).

`cambi_hip` and the `speed_*` family are deferred to round 3 because their
CPU references require setup the synthetic-fixture template doesn't carry
(encoded-bitrate metadata for cambi; no scalar reference for the speed
features as of master tip).

## Fixture choices

All 5 tests reuse the `test_hip_vif_parity.c` template verbatim:

- 256×144 YUV420P, 8 bpc (small enough for fast CI; large enough for
  MS-SSIM's 5-level pyramid to converge).
- Synthetic gradient (`(row + col + salt * k) & 0xFF`) — produces a non-zero
  variance signal that exercises every reduction branch.
- Chroma = 128 (mid-grey) except for ciede, which uses a salted chroma
  pattern because the CIE-Lab path is the whole point of that test.
- 1 frame for everything except motion (needs 2 frames so the t-1
  reference exists).

## Tolerance choices (ADR-0214)

| Kernel | Filter? | Tolerance | Rationale |
|---|---|---|---|
| `ciede2000` | no | 1e-4 (places=4) | Per-pixel reduction; matches CUDA twin in PR #351 |
| `psnr_hvs` | yes (DCT) | 1e-4 (places=4) | DCT is deterministic; reduction tree is shallow |
| `motion` v1 | yes (Gaussian) | 1e-4 (places=4) | Same kernel as motion3; equal precision budget |
| `ssim` | yes (window) | 1e-3 (places=3) | Per-window mean/var/cov reduction has more rounding |
| `float_ms_ssim` | yes (pyramid) | 1e-3 (places=3) | Multi-scale product amplifies per-scale rounding |

## Skip behaviour

Every test follows the established pattern: `vmaf_hip_state_init()` is
called first; on `err != 0` or `hip_state == NULL` the test emits
`[skip: no HIP device]` to stderr and returns success.  This lets CI
runners without an AMD GPU run the suite without spurious failures
(same as PR #351's psnr_hip + vif_hip).

## Container-verified

Per CLAUDE.md §15, the test files compile against the `vmaf-dev-mcp`
container's HIP toolchain.  Local execution requires `-Denable_hip=true`
and an AMD GPU; the dev workstation has no AMD card, so on-host
execution exercises the skip path.  CI ROCm runners (when present)
will exercise the parity assertions.

## Round-3 backlog

| Extractor | Blocker | Resolution path |
|---|---|---|
| `cambi_hip` | needs `enc_width`/`enc_height`/`enc_bitdepth` options on the fixture | wrap CPU reference with synthetic encoder metadata |
| `ssimulacra2_hip` | PR #290 actively refactoring partial-alloc paths | wait for #290 merge, then add test |
| `speed_chroma_hip`, `speed_temporal_hip` | no scalar CPU reference exists on master | scope a CPU reference first or call into the existing GPU path twice with different RNG seeds |
| `float_adm_hip`, `float_motion_hip`, `float_psnr_hip`, `float_ssim_hip`, `float_moment_hip` | float pipeline pending ADR-0119 successor cleanup | re-evaluate after the float-pipeline ADR lands |

## References

- ADR-0214 — cross-backend parity tolerance gate.
- ADR-0539 — integer ADM HIP atomicAdd reduction pattern.
- ADR-0868 — round-1 GPU kernel coverage gap-fill (PR #351).
- ADR-0883 — this work (round 2).
- PR #290 — HIP ssimulacra2 partial-alloc leak (skip overlap).
- PR #308 — HIP/Metal ENOSYS stubs (no test overlap).
- PR #351 — round-1 GPU kernel coverage (psnr_hip + vif_hip).

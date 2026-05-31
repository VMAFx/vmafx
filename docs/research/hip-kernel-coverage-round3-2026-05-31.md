<!-- markdownlint-disable MD060 -->
# HIP kernel parity coverage — round 3 audit (2026-05-31)

Companion research digest for ADR-0945.  Quantifies the HIP-side
coverage gap remaining after PR #372 / ADR-0883 round-2, ranks the
residual extractors by user-visible impact, and justifies the 4-test
selection that round-3 ships.

## Inventory

`find core/src/feature/hip -maxdepth 1 -name '*_hip.c'` returns **17
HIP extractor source files** (one extractor per file, plus the
`hip_hsaco_stubs.c` build-only entry that registers no extractor).

| Extractor source | Registered name | Tested before round-3? | Round-3 ships test? |
|---|---|---|---|
| `integer_adm_hip.c` | `adm_hip` | yes — `test_hip_adm_parity` (ADR-0539) | — |
| `integer_motion_v2_hip.c` | `motion_v2_hip` | yes — `test_hip_motion3_parity` | — |
| `integer_psnr_hip.c` | `psnr_hip` | yes — `test_hip_psnr_parity` (PR #351) | — |
| `integer_vif_hip.c` | `vif_hip` | yes — `test_hip_vif_parity` (PR #351) | — |
| `ciede_hip.c` | `ciede_hip` | yes — `test_hip_ciede_parity` (PR #372) | — |
| `integer_psnr_hvs_hip.c` | `psnr_hvs_hip` | yes — `test_hip_psnr_hvs_parity` (PR #372) | — |
| `integer_motion_hip.c` | `motion_hip` (v1) | yes — `test_hip_motion_parity` (PR #372) | — |
| `integer_ssim_hip.c` | `integer_ssim_hip` | yes — `test_hip_ssim_parity` (PR #372) | — |
| `integer_ms_ssim_hip.c` | `integer_ms_ssim_hip` | yes — `test_hip_ms_ssim_parity` (PR #372) | — |
| `integer_cambi_hip.c` | `cambi_hip` | **no** | **yes** — `test_hip_cambi_parity` |
| `float_adm_hip.c` | `float_adm_hip` | **no** | **yes** — `test_hip_float_adm_parity` |
| `float_motion_hip.c` | `float_motion_hip` | **no** | **yes** — `test_hip_float_motion_parity` |
| `float_psnr_hip.c` | `float_psnr_hip` | **no** | **yes** — `test_hip_float_psnr_parity` |
| `ssimulacra2_hip.c` | `ssimulacra2_hip` | **no** | deferred — PR #290 in flight on this file |
| `speed_chroma_hip.c` | `speed_chroma_hip` | **no** | deferred — round 4 (CPU scalar reference missing) |
| `speed_temporal_hip.c` | `speed_temporal_hip` | **no** | deferred — round 4 (same) |
| `float_ssim_hip.c` | `float_ssim_hip` | **no** | deferred — round 4 (mirror of integer SSIM) |
| `float_moment_hip.c` | `float_moment_hip` | **no** | deferred — round 4 (internal helper, no CPU twin) |

Coverage delta: **9 / 17 → 13 / 17** (53 % → 76 %) parity-gated HIP
extractors after this PR.

Note: the round-2 audit doc reported 17 extractors but listed
`float_ssim_hip` and `float_moment_hip` as separate deferrals; the
inventory above resolves both as round-4 candidates rather than
round-3.

## Selection rationale

The 4 round-3 picks were ranked on three axes:

1. **Reachability** — only kernels whose CPU twin exists on master tip
   and whose init() succeeds without setup the synthetic-fixture
   template can't carry.  `cambi_hip` clears this after a code-read
   that confirmed `enc_width`/`enc_height` fall back to source
   dimensions (line 522 of `integer_cambi_hip.c`); the three `float_*`
   kernels skip cleanly via the established `-ENOSYS` pattern when
   `enable_hipcc=false`.
2. **Twin-pair coverage** — `float_adm` / `float_motion` / `float_psnr`
   give the float pipeline the same CI gate the integer pipeline got
   in rounds 1 & 2.  Model trainers that prefer the unquantised
   float-pipeline features now get the same parity guarantee.
3. **Implementation maturity** — chose only extractors with no active
   in-flight refactor PRs touching the same file (`ssimulacra2_hip`
   excluded because PR #290 is mid-rewrite).

`speed_chroma_hip` / `speed_temporal_hip` remain blocked on the lack of
a stable CPU scalar reference on master tip (no LHS to assert parity
against).  `float_ssim_hip` mirrors `integer_ssim_hip` which is already
covered; deferred to round 4 to keep round 3 reviewable.
`float_moment_hip` is an internal helper without a public CPU twin
surface — needs CPU API work before a parity test can be written.

## Fixture choices

| Test | Geometry | Bit depth | Frames | Rationale |
|---|---|---|---|---|
| `test_hip_cambi_parity` | 320×240 | 8 | 1 | Must clear `CAMBI_MIN_WIDTH_HEIGHT == 216` on at least one dim |
| `test_hip_float_adm_parity` | 256×144 | 8 | 1 | Matches `test_hip_adm_parity` (ADR-0539); >= 32×32 keeps DWT scale-3 alive |
| `test_hip_float_motion_parity` | 256×144 | 8 | 2 | Motion needs t-1; we assert at frame index 1 |
| `test_hip_float_psnr_parity` | 256×144 | 8 | 1 | Matches `test_hip_psnr_parity` (PR #351) |

All fixtures use synthetic gradients
(`(row + col + salt * k) & 0xFF` for the simple cases; a coarse
8-pixel step pattern for cambi to produce a non-zero banding signal).
Chroma is uniform 128 (mid-grey) except where the kernel under test
specifically consumes chroma signal.

## Tolerance choices (ADR-0214)

| Kernel | Filter? | Tolerance | Rationale |
|---|---|---|---|
| `float_psnr` | no | 1e-4 (places=4) | Per-plane sum reduction; matches integer twin |
| `float_motion` | yes (Gaussian) | 1e-4 (places=4) | Same kernel as motion3; equal precision budget |
| `float_adm` | yes (DWT2 + CSF) | 1e-4 (places=4) | Matches integer ADM (ADR-0539) per-scale ratios |
| `cambi` | yes (multi-scale pool) | 1e-3 (places=3) | Pooling tree amplifies per-window rounding |

## Skip behaviour

Every test extends the established pattern: `vmaf_hip_state_init()`
called first; on failure → emit `[skip: no HIP device]`.  Then
`vmaf_use_feature(<name>_hip, NULL)` is called; if it returns
`-ENOSYS` (scaffold posture under `enable_hipcc=false`) the test
emits `[skip: HIP scaffold ENOSYS]` and exits cleanly.  Same skip
checks are duplicated on the `feed_frame` and `vmaf_read_pictures(EOS)`
boundaries because the float_* scaffolds gate their lifecycle helpers
under `HAVE_HIPCC` independently.

For `cambi_hip` specifically, the CAMBI HIP extractor maintains a
CPU-residual code path under `!HAVE_HIPCC` so the typical
vmaf-dev-mcp posture with an AMD GPU but no hipcc still executes
end-to-end (Strategy II hybrid per ADR-0205).  The float_* extractors
require the full HSACO blob and skip when the toolchain is absent.

## Container-verified

Per CLAUDE.md §15, the test files compile against the `vmaf-dev-mcp`
container's HIP toolchain.  Tests skip cleanly on host runners
without an AMD GPU (the standard CI matrix).  Skip messages route to
stderr so meson test output preserves the diagnostic.

## References

- ADR-0214 — cross-backend parity tolerance gate.
- ADR-0539 — integer ADM HIP atomicAdd reduction pattern.
- ADR-0868 — round-1 GPU kernel coverage gap-fill (PR #351).
- ADR-0883 — round-2 HIP kernel parity coverage (PR #372).
- ADR-0945 — round-3 HIP kernel parity coverage (this PR).
- Related PRs: #351 (round-1), #372 (round-2), #290 (HIP
  ssimulacra2 leak — round-4 blocker), #308 (HIP/Metal ENOSYS stubs).
- Source: `req` (operator dispatch 2026-05-31).

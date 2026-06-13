# ADR-0205: cambi GPU feasibility spike

- **Status**: Accepted
- **Date**: 2026-04-28
- **Deciders**: <lusoris@pm.me>, Claude (Anthropic)
- **Tags**: vulkan, gpu, cambi, feasibility-spike, fork-local

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) committed the GPU long-tail
batch 3 to ship every remaining metric across Vulkan / CUDA / SYCL.
After parts 1–6 closed (ssimulacra2 Vulkan kernel landed in
[ADR-0201](0201-ssimulacra2-vulkan-kernel.md); the float twins shipped
in [ADR-0194](0194-float-ansnr-gpu.md) ..
[ADR-0199](0199-float-adm-vulkan.md)), only one metric remains:
**cambi** (1533 LOC, the largest single feature in the fork).

ADR-0192 explicitly conditioned cambi's implementation on a
*feasibility spike* — the **`calculate_c_values`** pass maintains a
sliding 65 × 65 window histogram per output column and updates it
row-by-row using +1 / -1 range modifications. The CPU's
[`get_derivative_data_for_row`](../../core/src/feature/cambi.c#L460)
hot path likewise walks left-to-right per row. The AVX2 / AVX-512 /
NEON SIMD paths only vectorise *within a single histogram update*
across columns
([`cambi_avx2.c::cambi_increment_range_avx2`](../../core/src/feature/x86/cambi_avx2.c#L23));
the row-to-row state dependency is preserved sequentially. That
combination — sequential-state running update + 1024-bin histogram
per output column — is the single most SIMT-hostile shape in the
fork's metric set.

This ADR is the spike. It answers two questions:

1. **Is cambi feasible on GPU at all?** Yes — see Decision.
2. **Which of the three classical re-formulations
   (single-WG direct port, parallel scan, parallel tile)
   wins?** None exclusively. Ship a **hybrid host/GPU pipeline**
   for v1, mirroring [ADR-0201](0201-ssimulacra2-vulkan-kernel.md)'s
   precedent. Defer a fully-on-GPU c-values pass to a future batch.

The full strategy comparison (effort tables, bandwidth analysis,
literature survey) lives in
[research digest 0020](../research/0020-cambi-gpu-strategies.md);
this ADR locks the decision.

## Decision

### Feasibility verdict

**Confirmed feasible — ship hybrid host/GPU.** Every cambi pipeline
phase except `calculate_c_values` (sliding histogram) is
embarrassingly parallel and ports cleanly to Vulkan / CUDA / SYCL.
The c-values phase stays on the host for v1.

### v1 architecture sketch

```text
                          cambi_vulkan_extract()
                                  │
   ┌──────────────────────────────┼──────────────────────────────┐
   │                              │                              │
   ▼                              ▼                              ▼
GPU shader chain              GPU shader chain              GPU shader chain
(per-frame setup)             (per-scale 0..4)              (per-scale 0..4)

cambi_preprocess.comp         cambi_decimate.comp           cambi_derivative.comp
  - decimate to enc_w/enc_h     (skipped at scale 0          - per-pixel right + bottom
  - bit-shift to 10-bit          unless high_res_speedup)    - bool: `equal_right && equal_bottom`
  - optional anti-dither      cambi_filter_mode.comp        cambi_mask_dp.comp
                                - 3-tap mode horizontal        - separable summed-area table
                                - 3-tap mode vertical          - threshold compare → mask plane

                              ─── readback boundary ───

                              CPU residual (host code)
                                - calculate_c_values
                                  (sliding histogram, top-k pool)
                                - scale-weighted final score
```

Concrete shader contracts:

1. **`cambi_preprocess.comp`** — per-pixel decimation + bit-shift
   to 10-bit + (optional) 4-tap anti-dither. Single dispatch, per-
   pixel thread, no reduction. Trivially parallel.
2. **`cambi_derivative.comp`** — implements
   `get_derivative_data_for_row` over the whole frame in one
   dispatch. Each thread reads `image[i, j]`, `image[i, j+1]`,
   `image[i+1, j]` and writes `derivative[i, j] =
   (h_eq && v_eq)`. Edge cases (last column / last row) match the
   AVX2 path's special handling. Output is one `uint8` per pixel.
3. **`cambi_mask_dp.comp`** (split into two passes) — computes the
   summed-area table of `derivative[]` and threshold-compares the
   7×7 box-sum against `get_mask_index(...)`. The CPU does this
   with a cyclically-indexed sliding 9-row DP buffer; on GPU we
   materialise the full SAT because global memory is cheap and
   the SAT is separable (one row-prefix-sum pass + one column-
   prefix-sum pass, both `subgroupInclusiveAdd`-friendly within a
   workgroup, cross-WG reduction via decoupled lookback per
   Merrill & Grimshaw 2016 if needed; the fixture sizes (≤4K)
   are small enough that a two-dispatch host-orchestrated pass
   suffices).
4. **`cambi_decimate.comp`** — per-pixel `data[i, j] =
   data[2i, 2j]`. Trivial.
5. **`cambi_filter_mode.comp`** — separable 3-tap mode filter.
   Two dispatches (horizontal then vertical); each pass per-pixel
   threaded. Uses the 3-element `mode3` reduction.
6. **Host residual** — runs the CPU `calculate_c_values` +
   `spatial_pooling` against the GPU-produced image + mask
   buffers (mapped HOST_VISIBLE). The c-values + pool produce a
   `double` per scale; the host applies the scale weighting and
   the final `MIN(score, cambi_max_val)` clamp.

### Precision contract — `places=4` (tightened from ADR-0192's `places=2`)

ADR-0192 carried `places=2` as a planning placeholder for cambi
("measure-first"). The hybrid architecture lets us tighten that to
**`places=4`** as the canonical floor — same contract as every
other GPU twin in the fork — for two structural reasons:

- The GPU phases (preprocess, derivative, SAT, decimate, mode
  filter) are all **integer + bit-exact** w.r.t. the CPU. The
  derivative output is a single `bool` per pixel; the SAT
  threshold compare is a single `>` against `mask_index`; the
  decimate is a stride-2 gather; the mode filter is a 3-element
  lookup. None of these operations introduce float rounding —
  the GPU buffer is byte-for-byte identical to what the CPU code
  path would write.
- The c-values + pooling phase stays on the host, executing the
  **exact CPU code path** against the GPU-produced buffers. Since
  the input buffers are bit-identical, the output is bit-identical.

So the v1 implementation should land at **ULP=0** on the cross-
backend gate fixture, comfortably under `places=4` (5e-5
threshold). The CI step in `tests-and-quality-gates.yml` lands at
`--places 4` from day one — no temporary `places=2` lane, no
"measure first then ratchet" two-step. The contract is set by
the architecture, not by empirical observation.

Per project rule (`feedback_no_test_weakening`): if v1 misses
`places=4` we fix the kernel, not the gate.

### v1 implementation effort estimate

| Surface | Estimated LOC | Notes |
| --- | --- | --- |
| `cambi_vulkan.c` host glue | ~700 | Mirrors `ssimulacra2_vulkan.c` shape (init, descriptor pool, pipeline build, dispatch sequencer, host residual call). |
| `cambi_*.comp` shaders (6 files) | ~400 total | Each 30–80 LOC; preprocessing, derivative, SAT-row, SAT-col, decimate, filter-mode. |
| Host residual integration | ~50 | Refactor `calculate_c_values` to take a buffer pair instead of a `VmafPicture`, share with the existing CPU extractor. |
| Meson + feature_extractor wiring | ~30 | Add to `vulkan_sources` + extern in `feature_extractor.c`. |
| Smoke test + cross-backend hookup | ~50 | Lavapipe lane in `tests-and-quality-gates.yml`; `FEATURE_METRICS` row in `cross_backend_vif_diff.py`. |
| **Total** | **~1230 LOC** | Three weeks of focused work, comparable to `ssimulacra2_vulkan` (1544 LOC). |

### v1 PR ordering (post-spike)

1. **This PR** — spike (ADR-0205 + research digest 0020 +
   reference shader scaffolds + dormant `cambi_vulkan.c` host
   skeleton). No build wiring; no feature_extractor registration.
2. **Follow-up PR** — wire the scaffolds into the build,
   implement the host residual call path, validate against the
   cross-backend gate, register `vmaf_fex_cambi_vulkan`. This
   matches the [ADR-0201](0201-ssimulacra2-vulkan-kernel.md)
   precedent: the feasibility kernel lands first as an in-tree
   reference, then the build wiring + integration follow.
3. **Future** — CUDA + SYCL twins, mirroring the Vulkan kernel's
   shape (per-PR cadence per ADR-0192).

### Out of scope for this spike (deferred)

- The fully-on-GPU `calculate_c_values` pass via *strategy III*
  (direct per-pixel histogram, see digest §III). Tracked as
  long-tail batch 4 / "GPU perf polish".
- Real-time / streaming use-cases on 4K (the host residual at
  4K runs ~250 ms / frame; for batch processing this is fine
  and matches CPU performance, but real-time integration would
  need strategy III or II).
- Heatmap dump on GPU. Heatmap export remains a host-only
  feature (`heatmaps_path` option).

## Alternatives considered

| Option | Pros | Cons | Effort | Why not chosen |
| --- | --- | --- | --- | --- |
| **Strategy I — single-WG direct port** | Bit-exact w.r.t. CPU; trivial to implement | Single workgroup = ~1/64 of GPU active; empirically *slower* than CPU. Defeats the purpose of a GPU port. | ~600 LOC | Not viable; GPU utilisation is catastrophically low. |
| **Strategy II — parallel-scan reformulation** | High GPU utilisation; integer-bit-exact; uses well-known Blelloch / Merrill & Grimshaw scan primitives. | Materialises per-row column histograms (W × H × 1033 bytes ≈ 17 GiB at 4K scale 0). Bandwidth-heavy gather phase for c-value lookup. ~1500 LOC. | ~1500 LOC | Bandwidth cost dominates; intermediate storage at 4K is impractical. |
| **Strategy III — direct per-pixel histogram (full GPU)** | Algorithmically clean; full grid utilisation; integer-bit-exact. Reads only ~9 bins per pixel (the c-value formula's domain). | Per-pixel 65² = 4225 reads → ~9× CPU bandwidth. Cache-hit-rate dependent. ~800 LOC. | ~800 LOC | Worth doing eventually but premature for the long-tail terminus; needs profile data we don't yet have. Tracked as v2 follow-up. |
| **Hybrid host/GPU (chosen)** | Closes the matrix gap. Mirrors ADR-0201's precedent. Bit-exact. Modest LOC. Leaves perf upside on the table for v2. | The c-values phase doesn't accelerate (host does same work). PCIe shuttle for the per-scale buffer (~50 MiB / frame at 4K, <2 ms). | ~1230 LOC v1 | **Selected** — best risk/reward for closing the long-tail terminus. |
| **Defer cambi GPU port indefinitely** | Zero implementation risk for batch 3. | Leaves the matrix permanently incomplete; lpips already has its dedicated exception per ADR-0022, accumulating exceptions weakens the gate. | 0 | Rejected — closing the long-tail is the explicit ADR-0192 goal. |

The runner-up was **strategy III**. We rejected it for v1 because:

1. We have no profile data on the cache-hit rate at the per-output-
   pixel 65×65 window scan (4225 reads/thread). On Lavapipe (the CI
   gate) it is plausibly bandwidth-bound; on RDNA / Ada it is
   plausibly cache-friendly. Without empirical evidence the LOC
   investment is speculative.
2. The hybrid v1 closes the matrix gap *now*, unblocks the long-tail
   terminus declared in [ADR-0192](0192-gpu-long-tail-batch-3.md),
   and leaves strategy III as a clean follow-up under a focused PR
   (rather than coupling the architectural decision to the
   profile-driven optimisation).
3. Per the user's standing direction in CLAUDE.md §13: prefer
   shipping the closed matrix over chasing peak GPU utilisation
   when the latter trades implementation risk for a perf gain we
   haven't yet measured.

## Consequences

- **Positive**: closes the GPU long-tail matrix terminus. After the
  follow-up integration PR, every registered feature extractor in
  the fork has at least one GPU twin (lpips remains delegated to
  ORT execution providers per [ADR-0022](0022-inference-runtime-onnx.md)).
  Establishes a reusable hybrid host/GPU template for any
  future metric whose state is too sequential for full SIMT
  parallelism.
- **Positive**: the architectural decision is *separated* from the
  full-GPU optimisation. v1 ships the matrix closure; v2 (strategy
  III) is a focused perf PR with its own ADR and profile data.
  This matches how
  [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) split ssimulacra2
  into "hybrid landing first" + "future GPU XYB / SSIM compute".
- **Negative**: cambi GPU performance in v1 is bottlenecked by the
  CPU c-values phase. At 4K this is ~250 ms / frame — comparable
  to a CPU-only run. The GPU does the easy parts faster but
  doesn't accelerate the hot path. Real users who care about
  cambi throughput will need to wait for v2 (strategy III) or
  stick with CPU.
- **Negative**: adds maintenance surface that mirrors CPU code.
  The `calculate_c_values` host residual must stay in lock-step
  with the CPU extractor; any CPU-side change (e.g. Netflix
  upstream tuning the c-value formula) requires touching both
  sites. Mitigated by sharing the same `c_value_pixel` /
  `update_histogram_*` helpers via header inclusion (the host
  residual *is* the CPU `calculate_c_values`).
- **Neutral / follow-ups**:
  1. Follow-up PR wires the scaffolds into the build, implements
     the host residual call path, validates against the cross-
     backend gate, registers `vmaf_fex_cambi_vulkan`. Lavapipe
     lane added to `tests-and-quality-gates.yml`.
  2. CUDA + SYCL twins follow per ADR-0192 cadence.
  3. v2 ADR (likely `ADR-02XX-cambi-vulkan-c-values-gpu`) tackles
     strategy III with profile data from Lavapipe + RDNA + Ada.
  4. Heatmap dump remains host-only; if a follow-up wants GPU
     heatmaps, it adds a write-out shader at the c-values phase.

## Precision investigation

Not applicable for v1 (the precision-sensitive c-values phase stays
on the host). The GPU phases are integer + bit-exact-by-construction
(all operations are `uint16` arithmetic, comparison, gather; no
float rounding, no cross-WG float reduction). The follow-up
integration PR — tracked as backlog **T7-36** per the
[Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
§A.1.3 — will run the cross-backend gate empirically and either
confirm bit-exact or document any drift in its own ADR.

The strategy III v2 PR will need its own precision investigation
because the order of integer histogram increments in a per-pixel
scan differs from the row-major sliding update. The c-value formula
itself involves a float divide `(diff_weight * p_0 * p_1) /
(p_1 + p_0)` which is order-insensitive at the per-pixel granularity
(no cross-pixel summation), so bit-exactness should still hold;
empirical validation is required.

## References

- Parent: [ADR-0192](0192-gpu-long-tail-batch-3.md) — GPU long-
  tail batch 3 scope; explicitly conditions cambi on this spike.
- Sibling: [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) — the
  ssimulacra2 Vulkan kernel's hybrid host/GPU split is the direct
  precedent for the cambi v1 decision.
- Sibling: [ADR-0178](0178-vulkan-adm-kernel.md) — Vulkan ADM
  kernel; closest pipeline shape (multi-scale, multi-stage).
- Sibling: [ADR-0022](0022-inference-runtime-onnx.md) — lpips
  carve-out; cambi v1 closes the only other GPU long-tail
  exception.
- Companion: [research digest 0020](../research/0020-cambi-gpu-strategies.md)
  — full strategy comparison, bandwidth analysis, literature
  survey (Blelloch 1990, Sengupta 2007, Merrill & Grimshaw 2016).
- CPU reference: [`core/src/feature/cambi.c`](../../core/src/feature/cambi.c)
  (1533 LOC); SIMD paths in `x86/cambi_avx2.c`,
  `x86/cambi_avx512.c`, `arm64/cambi_neon.c`.
- Banding-detection literature: Tandon et al., "CAMBI: Contrast-
  Aware Multiscale Banding Index" (PCS 2021); Wu & Liu, "Banding
  artefact detection in compressed images" (VCIP 2007).
- User direction: standing CLAUDE.md §12 r10/r11 (every fork-
  local PR ships the six deep-dive deliverables; doc-substance
  rule applies); standing CLAUDE.md §13 (prefer closing the
  matrix over chasing peak utilisation).

- **`vif.comp` + `ciede.comp` shaders — `precise` decorations on the
  load-bearing FP reductions (ADR-0269 / Step A of the Vulkan 1.4 bump
  path)** — tags the FP accumulators in
  [`core/src/feature/vulkan/shaders/vif.comp`](core/src/feature/vulkan/shaders/vif.comp)
  (`g`, `sv_sq`, `gg_sigma_f` — the three lines that compute the per-frame
  VIF stats) and
  [`core/src/feature/vulkan/shaders/ciede.comp`](core/src/feature/vulkan/shaders/ciede.comp)
  (yuv→rgb outputs, rgb→xyz matmul accumulators, ciede2000 chroma
  magnitudes + half-axes + s_l/c/h + lightness/chroma/hue + final ΔE)
  with the GLSL `precise` qualifier. glslc 2026.1 lowers each tagged
  result to a per-result `OpDecorate ... NoContraction`, instructing
  the Vulkan driver's shader compiler to keep the `mul + add` patterns
  as separate ops rather than fusing them into FMAs. The decoration is
  the only Vulkan-side knob on FMA contraction (the OpenCL
  `OpExecutionMode ContractionOff` is rejected). 62 NoContraction lines
  in vif's optimised SPIR-V, 126 in ciede's; 1.3 vs 1.4 SPIR-V remains
  byte-identical.

  Empirical impact on NVIDIA RTX 4090 + driver 595.71.05 (Vulkan
  1.4.341), measured against the canonical Netflix pair at
  [`places=4`](docs/adr/0214-gpu-parity-ci-gate.md) tolerance:

  - **ciede2000**: 42/48 → **5/48** mismatches, max abs `1.67e-04`
    → `8.9e-05` (19× reduction). The pre-existing 42/48 baseline at
    API 1.3 was unflagged fork debt because no NVIDIA validation
    lane runs in CI today; this PR repays most of it.
  - **vif scale 2**: bit-clean at API 1.3 both before and after
    (0/48 in either state). The decorations protect against future
    driver codegen flips.

  Step B of the API-version bump path
  ([ADR-0264](docs/adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md))
  remains **blocked**: under a hypothetical 1.4 bump the vif scale-2
  regression persists at 45/48 / 1.527e-02 *despite* the
  `NoContraction` decorations being correctly emitted on every
  load-bearing op (verified at the SPIR-V `OpFDiv` / `OpFMul` /
  `OpFSub` ID level). The regression's root cause is not in the
  five tagged float ops; investigation continues. Full findings:
  [research-0054](docs/research/0056-vif-ciede-precise-step-a-implementation.md).

  Conservative `precise` scope is empirically the maximum that helps
  on ciede — widening into the helpers (`get_h_prime`, `get_upcase_t`,
  `get_r_sub_t`, the Lab axes) regresses the gate to 46/48. The
  shader carries inline comments recording this empirical bound so
  future widening attempts don't repeat the experiment. Bit-exact
  on RADV (Mesa NIR is conservative on FMA contraction; `precise` is
  effectively a no-op there).

  See [ADR-0269](docs/adr/0269-vif-ciede-precise-step-a.md) +
  [research-0054](docs/research/0056-vif-ciede-precise-step-a-implementation.md).

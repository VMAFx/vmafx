- Fixed Vulkan VIF scale 2/3 numerical saturation that inflated the VMAF score
  by +1.073 on 576x324 inputs (95.069 Vulkan vs 93.996 CPU for the Netflix
  golden pair).  Two independent bugs: (1) `float_vif.comp` was compiled with
  the SPIR-V optimizer enabled (`glslc -O`), whose FMA contraction of the
  `sigma1_sq = xx - mu1*mu1` variance expression underestimated local variance
  at pyramid scales 2/3 and pushed all pixels into the unconditional
  `num=1, den=1` low-signal branch; fixed by adding `float_vif.comp` to the
  `-O0` strict-shader list and adding `precise` qualifiers for driver-side
  defence-in-depth. (2) The integer VIF fused kernel (`vif.comp`) used floor
  division to size the per-scale rd buffer, which was 72 slots short for the
  81-pixel-tall scale-2 plane of 576x324 input; the 288-byte overflow corrupted
  the adjacent per-WG int64 accumulator and produced massively-negative
  denominators clamped to 1.0; fixed by ceiling division `(h+1)/2`.
  ADR-0381.

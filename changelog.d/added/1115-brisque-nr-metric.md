Added the BRISQUE no-reference, opinion-aware CPU feature extractor
(`brisque`, ADR-1115): a blind spatial image-quality metric (Mittal, Moorthy &
Bovik, IEEE TIP 2012) that scores the distorted picture only (no reference
frame), reachable via `vmaf --feature brisque`, the libvmaf C API, and the
feature registry. It bundles the canonical LIVE-lab `allmodel` (libsvm
EPSILON_SVR, `model/other_models/brisque_live.model`) under a documented
research-use attribution exception with a NOTICE and a model card citing the
TIP 2012 paper. The model is embedded into the binary at build time via an
`xxd -i` Meson `custom_target` (the same mechanism libvmaf's JSON models use),
so it needs no install or runtime path; the `model_path` feature option
overrides it with an on-disk libsvm model
(`vmaf --feature brisque=model_path=…`). The C
pipeline replicates the gregfreeman MATLAB pipeline that trained the model — GGD
for the MSCN field (not the krshrimali C++ port's AGGD), Gaussian sigma=7/6 (not
the truncated 1.166), and MATLAB antialiased-bicubic half-resolution downscale —
and range-scales with the reference inline `computescore.cpp` arrays (not the
conflicting `allrange` file), with no output clamp. It is the first feature
extractor to consume the vendored libsvm for its own SVR model. Validated on a
stable natural image to ~5e-5 vs an independent MATLAB-faithful oracle; unit
oracles and an odd-dimension regression in `core/test/test_brisque.c`. User docs
in `docs/metrics/brisque.md`. Lower score is better; SDR-luma only — PQ/HLG HDR
and the AGGD near-zero sign sensitivity are documented limitations.

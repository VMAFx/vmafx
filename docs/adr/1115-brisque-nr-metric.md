<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1115: BRISQUE no-reference CPU feature extractor (bundled LIVE model)

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: metrics, feature-extractor, no-reference, cpu, model, license, fork-local

## Context

The fork ships SSIMULACRA2 and CIEDE2000 (full-reference) and CAMBI (NR
banding) but no general-purpose, distortion-generic no-reference IQA metric.
BRISQUE — Blind/Referenceless Image Spatial Quality Evaluator (Mittal, Moorthy,
Bovik, IEEE TIP 2012) — is the canonical opinion-aware blind metric, sensitive
to JPEG/JP2K/blur/noise/fast-fading distortion. Adding it gives `vmaf-tune` and
the MCP surface a single-picture quality signal when no reference is available,
and it is a clean first consumer of the vendored libsvm
(`core/src/svm.cpp`/`svm.h`) from a *feature extractor* — today libsvm is only
used by the VMAF model predict path — proving the "extractor owns its own SVR
model" pattern for future NR metrics.

Unlike NIQE (ADR-1112), the fork has **no** pre-trained BRISQUE model in tree.
BRISQUE's prediction depends on a trained EPSILON_SVR model (the LIVE-lab
`allmodel`) plus a fixed feature range-normalization. The canonical model was
trained by the gregfreeman MATLAB pipeline; the krshrimali C++ port is a
verbatim mirror of the same model. Its redistribution license is the central
decision: the LIVE lab released the BRISQUE model for research/education
conditioned on citing the TIP 2012 paper — a non-OSI, attribution-bearing term.

A second, load-bearing subtlety: the reference's feature range arrays exist in
two conflicting forms in the same upstream repo — the **inline** `min_[36]` /
`max_[36]` arrays in `computescore.cpp` (the array the trained model expects and
the only one the prediction code reads) and a separate `allrange` file that
disagrees on all 36 rows by up to 0.71 in scaled space. The reference code never
reads `allrange`; substituting it would corrupt every score (an RBF gamma=0.05
perturbation over a 0.71 shift is score-destroying, not 1e-4). The inline arrays
are the source of truth.

## Decision

Add a scalar CPU BRISQUE extractor (`core/src/feature/brisque.c`, feature name
`brisque`; numeric kernels in `brisque_math.h`) and **bundle the canonical
LIVE-lab `allmodel`** under
`model/other_models/brisque_live.model`, accepted as a **documented
research-use redistribution exception** with a NOTICE
(`model/other_models/NOTICE-brisque`) and a model card
(`model/brisque_live_card.md`) citing Mittal/Moorthy/Bovik TIP 2012, the source
repo, and the research-use terms. This mirrors the precedent that `model/`
already ships Netflix-trained `.pkl`/`.json` artifacts under their own non-fork
terms.

**Model embedding (revised).** The model is embedded into the libvmaf binary
**at build time** by an `xxd -i` Meson `custom_target` over
`model/other_models/brisque_live.model`, generating `brisque_live.model.c` in
the build directory with the symbols `src_brisque_live_model[]` /
`src_brisque_live_model_len`. This is the **identical mechanism libvmaf already
uses for its own SVR JSON models** (`json_model_c_sources` in
`core/src/meson.build`), so BRISQUE carries no runtime model-file dependency and
nothing needs installing alongside the binary. `core/src/feature/brisque_model.h`
is a tiny declaration header (the externs + provenance comment) — the ~2.1 MB
byte array is **never committed**, because it exceeds the repository's 1 MB
`check-added-large-files` pre-commit gate. (The original implementation committed
the generated array as a 2.1 MB header, which blocked the branch; this ADR was
revised to the build-time embed.) `init()` loads the model via
`svm_parse_model_from_buffer(src_brisque_live_model, …_len)`; the new
`model_path` feature option overrides it with an on-disk libsvm model
(`svm_load_model`) and is the only loader when `built_in_models` is disabled. A
missing model fails `init()` with `-EINVAL` and a clear log line. The model's
SHA-256 is pinned in the model card.

The C pipeline replicates the **gregfreeman MATLAB pipeline that trained the
model** (NOT the krshrimali C++ port, which diverges on three points the model
was not trained with): GGD fit of the MSCN field for f1/f2 (krshrimali uses
AGGD — a bug vs the paper and the trained model); 7×7 Gaussian sigma = 7/6 (not
the truncated 1.166); and MATLAB antialiased-bicubic 0.5× downscale (not OpenCV
INTER_CUBIC). MSCN is `(I−μ)/(σ+1)` on luma scaled to the [0,255] double working
range for all bit depths (matching the MATLAB training range — the additive C=1
term is not scale-invariant). Range-scaling uses the inline `min_[36]`/`max_[36]`
arrays with **no output clamp** (matching the reference prediction path; the
OpenCV variant clamps to [0,100] and is a *different* model). Prediction uses
plain `svm_predict` (proven identical to `svm_predict_probability` for
EPSILON_SVR). As an NR metric it scores the distorted picture only and
`(void)`-discards the reference and 90°-rotated inputs (CAMBI/NIQE posture).
PQ/HLG HDR is out of scope (no HDR-trained model exists); the extractor emits a
one-time warning on >8-bpc input and scores it as SDR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Bundle the LIVE `allmodel` under a documented attribution exception (chosen)** | Reproduces canonical BRISQUE; no training corpus or compute needed; one self-contained PR; precedented by the Netflix non-OSI artifacts already in `model/` | Redistributes a non-OSI, research-use model — needs a NOTICE + model card + ADR; single-source artifact (SHA-pinned) | — |
| Retrain a fork-owned BRISQUE SVR on an open corpus | Fork-owned, cleanly licensed weights | Needs a labelled subjective IQA corpus (LIVE/TID/KADID) + a full SVR training + validation pass; scores would diverge from canonical BRISQUE; large multi-PR effort | Out of scope for an RC metric add; defeats "canonical BRISQUE" |
| Code-only (ship the extractor, require a user-supplied model path) | No redistribution question at all | Metric is unusable out of the box; every user must source the same non-OSI model themselves; no end-to-end test possible in-tree | Fails the "first-class CLI/API metric" goal; worse license posture in practice (pushes the obligation onto users) |
| Use the OpenCV `brisque_model_live.yml` (Apache-2.0) | OSI-licensed | A *different* trained model (total_sv 774, rho −149.635) in `cv::ml::SVM` YAML, needs its own range file and a YAML→libsvm conversion to load via the vendored libsvm; clamps output | More code + a different metric than canonical BRISQUE; do-not-mix with the inline range arrays |
| Defer BRISQUE entirely | Zero license/redistribution exposure | Leaves the fork with no distortion-generic NR metric | The maintainer chose to ship; the gap is real |

### Model-loading mechanism

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Build-time `xxd` embed via Meson `custom_target` (chosen)** | Identical to libvmaf's own SVR JSON-model embed (`json_model_c_sources`); zero runtime model-file dependency and nothing to install; the giant byte array lives only in the build dir so the tree stays under the 1 MB large-file gate; reuses the existing `svm_parse_model_from_buffer`; end-to-end test works in-tree | Build needs `xxd` with `-n` (already required for the JSON models); one extra `custom_target` | — |
| Commit the generated byte-array header (original implementation) | Build needs no `xxd`; fully self-contained C | The 343 KB text model expands to ~2.1 MB of C — **exceeds the 1 MB `check-added-large-files` pre-commit gate**, which is exactly what blocked the branch; large committed generated artifact | Hard-blocked by the large-file gate |
| Runtime load from a resolved model dir (`svm_load_model` + a `VMAFX_MODEL_DIR` / install-prefix lookup) | No embedded bytes at all | The fork has **no** default model-dir resolution for SVR models (VMAF's own models are baked in; there is no `install_data`/datadir precedent for `model/`); would make BRISQUE the only metric whose model can go missing at runtime and require a brand-new install + path-resolution surface | More surface than the existing embed pattern; worse out-of-the-box behaviour; still keep `svm_load_model` only as the optional `model_path` override |

## Consequences

- **Positive**: BRISQUE is now a first-class CPU feature reachable from the CLI
  (`--feature brisque`), the libvmaf C API, and (once wired) the ffmpeg filter.
  On a stable natural image (cameraman) the C extractor matches an independent
  MATLAB-faithful reference to ~5e-5 (C = −13.70844 vs oracle = −13.70840). All
  eight `brisque_math.h` unit oracles match independent re-derivations exactly.
  First feature-extractor consumer of the vendored libsvm.
- **Negative**: Redistributes a non-OSI research-use model (documented exception;
  the NOTICE must accompany the bytes). The model is single-sourced
  (SHA-pinned). BRISQUE's AGGD paired-product fit classifies samples by a strict
  sign cutoff (`x<0` / `x>0`), so on heavily-compressed near-flat content a large
  fraction of paired products sit within ~1e-11 of zero and the left/right
  bucket assignment — hence the score — is sensitive to FP summation order
  (~0.1 units). This is inherent to BRISQUE; the in-tree fixture is therefore
  snapshotted (the extractor's own deterministic output) rather than
  cross-asserted at places=4. HDR (PQ/HLG) is out of scope: >8-bpc input is
  scored as SDR with a one-time warning.
- **Neutral / follow-ups**: GPU twins (CUDA/SYCL/HIP) are out of scope here; a
  future twin must keep the gamma argmin and RBF kernel sum in fp64 and is not
  expected to be bit-exact to CPU (per the fork's GPU-parity posture). The exact
  MATLAB `imresize` antialiased-bicubic kernel is the trained downsample; the C
  port reproduces it to ~1e-13 vs a numpy port for even and odd dimensions.
  Tracked in [`docs/state.md`](../state.md).

## References

- Mittal, Moorthy, Bovik, "No-Reference Image Quality Assessment in the Spatial
  Domain," IEEE TIP 21(12):4695-4708, 2012 —
  <https://live.ece.utexas.edu/publications/2012/TIP%20BRISQUE.pdf>
- Reference MATLAB pipeline that trained the model — gregfreeman/image_quality_toolbox
  `+brisque/{brisque_feature,estimateggdparam,estimateaggdparam}.m` and `allmodel`
- Reference C++ mirror — krshrimali/No-Reference-Image-Quality-Assessment-using-BRISQUE-Model
  (`C++/allmodel`, `C++/computescore.cpp` inline range arrays; the `C++/allrange`
  file is the conflicting, prediction-inconsistent set the code never reads)
- Bundled asset — `model/other_models/brisque_live.model`
  (sha256 19526fb799c4c7992ccc109fcfecddb25976ba024b194cd3ee275d27e8909c8d),
  NOTICE `model/other_models/NOTICE-brisque`, card `model/brisque_live_card.md`
- Design dossier — `.workingdir2/rc/metrics/brisque.md` (math + model adversarially confirmed; range-array provenance corrected to the inline arrays)
- Research digest — [`docs/research/1101-brisque-nr-metric.md`](../research/1101-brisque-nr-metric.md)
- Sibling NR extractor — [ADR-1112](1112-niqe-nr-metric.md) (NIQE)
- Source: `req` (maintainer direction to bundle the canonical LIVE/OpenCV BRISQUE model with a NOTICE/citation under a documented research-use redistribution exception, and to use the inline `computescore.cpp` range arrays — not `allrange`)

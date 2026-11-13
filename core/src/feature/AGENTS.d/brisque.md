---
paths:
  - core/src/feature/brisque.c
  - core/src/feature/brisque_math.h
invariant: BRISQUE MATLAB pipeline parity and numerical stability assertions.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# BRISQUE MATLAB-Pipeline Parity Invariants

- **BRISQUE MATLAB-pipeline parity invariants** (`brisque.c` /
  `brisque_math.h` / `brisque_model.h`, fork-local, ADR-1115): BRISQUE has **no
  upstream twin** — it replicates **gregfreeman MATLAB pipeline that trained
  bundled model** (`model/other_models/brisque_live.model`), NOT
  widely-copied krshrimali C++ port. Four choices are **load-bearing** and must
  survive any refactor (porting C++ behaviour instead would mis-predict
  against trained model):
  1. MSCN field (features f1/f2) is fit with **GGD**, not AGGD
     (`brisque_fit_ggd`). krshrimali uses AGGD — bug vs paper Table I and
     trained model.
  2. Gaussian window uses **sigma = 7/6** (`brisque_build_window`), not
     truncated `1.166` from C++ port.
  3. half-resolution downscale is **MATLAB antialiased bicubic**
     (`brisque_resize_coeffs`, scale 0.5, kernel width 8 → fixed 10 taps,
     symmetric-reflect index map), not OpenCV INTER_CUBIC.
  4. Range-scaling uses **inline `computescore.cpp` `min_[36]`/`max_[36]`
     arrays** baked into `brisque.c` — NOT conflicting `allrange` file in
     same upstream repo (which reference code never reads; substituting it
     corrupts every score). No output clamp. Prediction is plain `svm_predict`
     (== `svm_predict_probability` for EPSILON_SVR).
  AGGD fit excludes exact zeros from both sign buckets (strict `x<0` /
  `x>0`, MATLAB semantics — unlike NIQE, which buckets zeros right). model
  is **embedded at build time** by `xxd -i` Meson `custom_target` over
  `model/other_models/brisque_live.model` (same path as libvmaf's JSON models;
  symbols `src_brisque_live_model[]` / `_len`, declared in `brisque_model.h`) —
  big C array is NOT committed (it exceeds 1 MB large-file gate; only
  binary model + tiny declaration header live in-tree). `init()` parses
  that buffer via `svm_parse_model_from_buffer`, or, if `model_path` option
  is set, loads on-disk model via `svm_load_model`. end-to-end gate is
  `core/test/test_brisque.c` against
  `testdata/scores_cpu_brisque.json` (snapshotted because AGGD strict-sign
  fit is FP-summation-order-sensitive on near-flat content). First feature
  extractor to consume vendored libsvm. Registered in **C++23
  `feature_extractor.cpp`** registry.

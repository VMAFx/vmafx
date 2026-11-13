---
paths:
  - core/src/feature/niqe.c
  - core/src/feature/niqe_math.h
invariant: NIQE fork-pkl parity, sharpness calculation, and model coefficient loading.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# NIQE Fork-pkl Parity and Sharpness Invariants

- **NIQE fork-pkl parity invariants** (`niqe.c` / `niqe_math.h` /
  `niqe_model.h`, fork-local, ADR-1112): NIQE has **no upstream twin** — it
  replicates fork Python harness
  (`compat/python-vmaf/core/noref_feature_extractor.py::NiqeNorefFeatureExtractor`),
  not LIVE MATLAB or scikit-video. Two divergences from upstream NIQE are
  **load-bearing** and must survive any refactor:
  1. AGGD mean parameter `N` carries **trailing `*aggdratio`** factor
     (`niqe_math.h::niqe_extract_aggd`). Upstream omits it; pkl was
     trained with it. Dropping it shifts `N` ~0.428 → ~0.245.
  2. MSCN maps (`niqe_compute_mscn` casts to `float`) **and** PIL
     bicubic half-resolution output (`niqe_bicubic_resize` final
     `(double)(float)acc`) are **rounded through float32**. PIL returns
     float32 ('F'-mode) image and harness quantizes MSCN maps; without
     these rounds scale-2 features drift ~4e-7 and, through
     ill-conditioned averaged covariance, score by ~1e-4.
  pristine model `niqe_model.h` is generated from
  `model/other_models/niqe_v0.1.pkl` in per-block **interleaved** feature
  order (permutation in header comment); build-time checksums
  (`mu.sum`, `trace(cov)`, sha256 prefixes) pin it. end-to-end gate is
  `core/test/test_niqe.c` against `testdata/scores_cpu_niqe.json` at places=4.
  niqe is registered in **C++23 `feature_extractor.cpp`** registry, not
  dead `feature_extractor.c` twin.

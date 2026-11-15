---
paths:
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_collector.cpp
  - core/src/feature/feature_name.cpp
invariant: Scope, ground rules, discoverability, option tables, and feature extractor workflows.
area: framework
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Feature Extractors Scope, Ground Rules, and Workflows

## Scope

Every VMAF "feature" is small C module with `VmafFeatureExtractor`
registration:

```text
feature/
  feature_extractor.cpp/.h   # the registry + lifecycle contract (init/extract/flush/close)
  feature_collector.cpp/.h   # per-frame score aggregator
  vif.c / adm.c / …          # scalar CPU reference implementations
  integer_*.c                # integer-math reference implementations
  feature_lpips.c            # DNN-backed extractor (opens vmaf_dnn_session_*)
  feature_dists.c            # DISTS-Sq DNN-backed extractor (LPIPS-shaped ABI)
  x86/                       # AVX2 / AVX-512 SIMD paths — must match scalar bit-for-bit
  arm64/                     # NEON SIMD paths — must match scalar bit-for-bit
  cuda/                      # CUDA kernels + launchers
  sycl/                      # SYCL kernels (DPC++)
  common/                    # cross-arch helpers
```

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../../AGENTS.md)).
- **Bit-exactness with scalar reference** is non-negotiable for SIMD
  paths. Reductions, FMA-ordering, and rounding must match scalar path
  exactly — no "close enough". See
  [add-simd-path](../../../../.claude/skills/add-simd-path/SKILL.md) for
  dispatch pattern (`cpu.c` + feature_name_avx2.c + feature_name_avx512.c).
- **CUDA / SYCL kernels** should match CPU reference within
  documented tolerance. If kernel cannot match exactly, file snapshot
  justification in commit message and regenerate
  `testdata/scores_cpu_*.json` via
  [`/regen-snapshots`](../../../../.claude/skills/regen-snapshots/SKILL.md).
- **Registration is discoverable by both name and provided-feature-name**:
  `vmaf_get_feature_extractor_by_name()` and
  `vmaf_get_feature_extractor_by_feature_name()`. Both must resolve.
  - **Registered GPU twin = parity-gate cell (ADR-1460).** New `_cuda` /
    `_sycl` / `_hip` twin in `feature_extractor_list[]` -> gate feature in
    `scripts/ci/cross_backend_parity_gate.py` + `cross_backend_vif_diff.py`
    (`FEATURE_METRICS`, tolerance), same PR.
    `core/test/test_parity_gate_covers_registered_twins.py` fails otherwise.
  - **GPU/Metal twins live in `feature_extractor.cpp`'s `#if HAVE_*`
    blocks, NOT in parallel file.** registry was `feature_extractor.c`
    until PR #875 introduced compiled `.cpp` twin; for window both
    files existed and diverged, and `speed_{chroma,temporal}_{cuda,
    sycl,hip}` registrations were left behind in the dead `.c` — so the
    kernels compiled but `by_name("speed_chroma_cuda")` returned NULL and
    SpEED silently fell back to CPU. `.c` is now deleted; when you add
    GPU twin, add its `extern` + array entry to matching `#if HAVE_*`
    block in `.cpp` AND assert resolution in
    `test/test_feature_extractor.c`. registered-but-unresolvable twin is
    silent correctness bug, not build error.
  - **CPU extractor `provided_features` = names it WRITES, never pseudo-name.**
    ADR-1359 twin lookup (`vmaf_get_feature_extractor_twin`) + model dispatch
    pair CPU extractor with device twin through those names. Mismatch =
    twin registered, never selected: `--backend <gpu> --feature X` runs CPU
    with "has no twin" warning. Upstream `float_moment.c` declares
    pseudo-name `"float_moment"`; fork declares four emitted
    `float_moment_{ref,dis}{1st,2nd}` (twins declare same). KEEP fork list
    on upstream sync. `vmaf_feature_extractor_twin_audit()` counts device
    twins no CPU extractor reaches; `test_every_device_twin_is_reachable`
    asserts 0 in every build, so new twin with drifting names fails there.
- **Options tables** must have non-NULL `help` for every entry; see
  [../../test/test_lpips.c](../../../test/test_lpips.c) for unit-test
  pattern that enforces this.
- **DNN-backed extractors** open sessions through
  [src/dnn/](../../dnn/AGENTS.md) — never call ONNX Runtime directly from
  `feature_*.c` file.

## Workflows

| Task | Skill |
| --- | --- |
| Add a feature extractor | [add-feature-extractor](../../../../.claude/skills/add-feature-extractor/SKILL.md) |
| Add a SIMD path | [add-simd-path](../../../../.claude/skills/add-simd-path/SKILL.md) |
| Cross-backend diff | [cross-backend-diff](../../../../.claude/skills/cross-backend-diff/SKILL.md) |
| Profile a hot path | [profile-hotpath](../../../../.claude/skills/profile-hotpath/SKILL.md) |

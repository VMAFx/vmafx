---
paths:
  - core/src/feature/sycl/integer_*.cpp
  - core/src/feature/cuda/integer_*.c
invariant: Adding feature knob to any one backend must sync option tables across backends.
area: runtime-and-kernels
---
<!-- markdownlint-disable MD013 MD060 -->
# Per-feature option-table sync invariant

**Adding feature knob to any one backend (SYCL / CUDA / HIP / Metal /
Vulkan) requires adding it to all backends in same PR** — no deferred
follow-ups. Canonical source of truth for option signature (name,
alias, type, min, max, default, flags) = CPU feature extractor in
`core/src/feature/` (e.g. `integer_motion.c`). GPU twins copy
option entry verbatim, apply weight in equivalent host-side
`flush()` or post-processing callback.

Rationale: CHUG / K150K extractor whitelist in
`ai/scripts/extract_k150k_features.py` passes `_feature_arg` dicts to
`vmaf_use_features_with_opts`; if receiving backend's options table
misses knob, option silently falls through to default,
producing silently-wrong scores without any error. Root cause
of `motion_fps_weight` gap in `integer_motion_v2_sycl.cpp`, closed by
PR #851-follow-up (2026-05-16).

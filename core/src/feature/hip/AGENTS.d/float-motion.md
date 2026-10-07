---
paths:
  - core/src/feature/hip/float_motion_hip.c
  - core/src/feature/hip/float_motion_hip.h
  - core/src/feature/hip/float_motion/float_motion_score.hip
invariant: float_motion force-zero ownership and flush idempotency must follow BUG048 specifications.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_motion force-zero ownership and flush idempotency (BUG048 A5)

- `init_fex_hip()` releases device lifecycle before returning from
  `motion_force_zero` path, but cloned extractor still owns its
  `feature_name_dict`. Keep `close_fex_hip` (or equivalent dictionary-owning
  callback) installed; restoring `fex->close = NULL` leaks dictionary.
- Before appending tail `VMAF_feature_motion2_score`, `flush_fex_hip()`
  resolves actual score name through `feature_name_dict` and probes that
  name at `s->index`. literal-name probe misses option-derived names such as
  `motion_fps_weight=1.5` and makes repeated flush fail.
- `test_hip_float_motion_parity` exercises both invariants on real HIP device.
  Preserve its non-default feature parameter when rebasing this fork-local
  extractor. See [Research-2115](../../../../../docs/research/2115-hip-float-motion-lifecycle-flush.md).

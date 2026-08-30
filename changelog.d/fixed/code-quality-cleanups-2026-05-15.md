# Code-quality cleanups — 2026-05-15

Batch 6 mechanical defect fixes. No user-visible behaviour change.

| # | File | Line | Fix |
|---|------|------|-----|
| 1 | `core/src/feature/iqa/convolve.c` | 103 | Replace `assert(0)` in `iqa_calc_scale` with `return 1.0f` + comment; remove unreachable dead code below. Upstream `zli-nflx` left non-normalised 1-D separable kernel scaling unimplemented; all in-tree callers set `normalized = true` so this path is never reached. JPL Rule 14 / ADR-0012. |
| 2 | `core/src/feature/iqa/convolve.c` | 171 | Replace OOM `assert(0)` in `iqa_convolve_1d_separable` with a proper `-1` error return propagated to the `iqa_convolve` call site via a `(void)` cast (public API is `void`; ABI-compatible change deferred). JPL-CERT MEM52-CPP / ADR-0012. |
| 3 | `core/src/read_json_model.c` | 110 | Add `vmaf_log(VMAF_LOG_LEVEL_ERROR, …)` before the silent `return -EINVAL` for unknown JSON value types in `parse_feature_opts_entry`; add `#include "log.h"`. |
| 4 | `core/src/feature/hip/vif_hip.c` | 18 | Change `vmaf_hip_vif_init` return from `0` to `-ENOSYS`, matching `adm_hip.c:17` and `motion_hip.c` scaffold posture (ADR-0241). A `0` return misleads the feature engine into thinking VIF-HIP initialised successfully. |
| 5 | `ai/scripts/measure_quant_drop_per_ep.py` | 146 | Add `# TODO(#842)` comment to `_Runner.infer` abstract stub; opened GH issue #842 to track conversion to `abc.ABC`. |
| 6–12 | `ai/scripts/{export_tiny_models,extract_konvid_frames,fetch_konvid_1k,measure_quant_drop,ptq_dynamic,ptq_static,train_konvid}.py` | 1 | Add missing `Copyright 2026 Lusoris` + SPDX headers per CLAUDE.md §12 r7. |
| 13 | `docs/state.md` | — | Added Open rows: T-VCQ-223-LOCAL-EXPLAINER-HANG, T-VK-T7-29-PART-2-IMPORT-NOT-IMPL, T-CAMBI-HIP-NOT-STARTED. |

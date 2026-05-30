- **docs(state)**: closed-PR row sweep — 2 Open rows that cited
  CLOSED-not-merged PRs reconciled:
  1. **T-CUDA-FILTER1D-RES-DISPATCH-CONFLICT-2026-05-29** migrated
     from Open to Recently closed as superseded. PR #214 (the
     planned cleanup of conflict markers on the scaffold branch
     `feat/cuda-resolution-dispatch-scaffold-20260529`) was closed
     2026-05-30 once its base was abandoned. PR #91 had already
     merged ADR-0753 resolution-aware CUDA dispatch on master via
     the `adm_cm_device()` consumer without extending dispatch into
     `filter1d_8()`, so master never carried the conflict markers.
     `core/src/feature/cuda/integer_vif_cuda.c::filter1d_8()` on
     master uses the clean unconditional `cuLaunchKernel` paths.
  2. **T-CPP23-READ-JSON-MODEL-PENDING-2026-05-29** retained as Open
     but the dead PR #215 citation removed. The C++23 Wave 8
     conversion of `core/src/read_json_model.c` is still pending on
     master (still a `.c` source per `core/src/meson.build:1578`);
     a fresh PR is required. Owner field updated to "Owner-driven;
     pending fresh PR per ADR-0846 Wave 8".

  Net Open count -1; total T-row count unchanged (153).
  No code changes — documentation cleanup only.

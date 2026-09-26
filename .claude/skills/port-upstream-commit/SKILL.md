---
name: port-upstream-commit
description: Cherry-pick single upstream Netflix/vmaf commit onto fork's master, auto-adapting SIMD/GPU paths where commit touches feature with multiple implementations.
---
<!-- markdownlint-disable MD013 -->

# /port-upstream-commit

## Invocation

```text
/port-upstream-commit <sha> [--open-pr]
```

## Steps

1. `git fetch upstream`.
2. `git switch -c port/<sha-short> master`.
3. `git cherry-pick <sha>` (`-x` -> commit message references upstream).
4. If conflicts: inspect. For conflict in file with SIMD/GPU twins
   (e.g. edits to `float_adm.c` while repo has `x86/float_adm_avx2.c`,
   `x86/float_adm_avx512.c`, `arm64/float_adm_neon.c`, `cuda/adm_*.cu`,
   `sycl/integer_adm_sycl.cpp`, `feature/vulkan/float_adm_vulkan.c` +
   `feature/vulkan/shaders/*.comp`), report all sibling files to author
   so they propagate same change. Do NOT attempt automatic propagation —
   SIMD/GPU adaptations not string-substitutions.
5. Run `/build-vmaf --backend=cpu` + `python3 scripts/ci/run_meson_test.py -- -C build --suite=fast`.
6. Run `/cross-backend-diff` for affected feature (covers cpu / cuda / sycl /
   vulkan; mirrors T6-8 GPU-parity gate,
   [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md)).
7. If `--open-pr`: `gh pr create` with title
   `port(upstream): <original subject>`, body with upstream commit link,
   conflict summary, propagation TODO.

## Guardrails

- Abort if Netflix golden tests fail post-port.
- Never force-resolve conflicts — leave for human review.
- Link back to upstream commit in message
  (`(cherry picked from commit <sha>)`).

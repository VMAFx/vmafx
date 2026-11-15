<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/cuda

Orientation for agents working on CUDA backend runtime. Parent rules apply in full: [../../AGENTS.md](../../../AGENTS.md).

- Every CUDA call checks errors via `cuda_helper.cuh` macros; never invent silent wrappers.
- `cudaMemcpyAsync` requires pinned host memory (pageable buffers serialize); picture pools allocate pinned.
- Experimental flags (`--expt-relaxed-constexpr`, `--extended-lambda`, `--expt-extended-lambda`, `sm_120` gencode; [ADR-0027](../../../../docs/adr/0027-non-conservative-image-pins.md)) target stable CUDA >= 13.2, not preview branches.
- Numerical snapshots: kernels unable to bit-match CPU reference regenerate `testdata/scores_cpu_cuda.json` via [`/regen-snapshots`](../../../../.claude/skills/regen-snapshots/SKILL.md) with commit justification ([AGENTS.md §5](../../../../AGENTS.md)).

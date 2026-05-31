### CUDA `__mul24` silent-corruption audit (Research-0734)

Audited all 78 CUDA kernel and runtime files under `core/src/feature/cuda/`
and `core/src/cuda/` for use of the CUDA `__mul24` / `__umul24` / `__mul24hi`
intrinsics, which carry a confirmed silent data-corruption bug on CUDA 11.1–13.2
(fixed in CUDA 13.3). Zero occurrences were found; no scores are affected.

Deliverables: research digest `docs/research/0734-cuda-mul24-corruption-audit.md`,
prohibition invariant in `core/src/feature/cuda/AGENTS.md`, and known-issues note
in `docs/backends/cuda/overview.md`.

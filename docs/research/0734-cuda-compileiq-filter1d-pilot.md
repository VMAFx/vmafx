# Research-0734: CUDA CompileIQ Pilot — `filter1d.cu` (Abandoned)

- **Date**: 2026-05-28
- **Author**: Claude (Anthropic) / Lusoris
- **ADR**: [ADR-0739](../adr/0739-cuda-compileiq-filter1d-pilot.md)
- **Status**: Abandoned — two hard blockers identified before search ran

## Objective

Evaluate NVIDIA CompileIQ v1.0.0 (hyperparameter optimization for NVIDIA compilers) as
an automatic-tuning mechanism for `core/src/feature/cuda/integer_vif/filter1d.cu`
(845 LOC, the hot-path CUDA kernel for VIF horizontal/vertical filter passes). The
hypothesis was that a compiler-control ACF file persisted in-tree and gated behind
`-Denable_compileiq=true` could yield ≥3% throughput improvement with zero numeric
divergence.

## Methodology

1. Branch `research/cuda-compileiq-filter1d-pilot-20260528` from `origin/master`.
2. Detect container image via `docker inspect vmaf-dev-mcp --format '{{.Config.Image}}'`
   → `vmaf-dev-mcp:local`.
3. Smoke container approach: `docker run --rm --gpus all -v <worktree>:/workspace ...`
   with `--entrypoint bash` to bypass the persistent-container entrypoint script.
4. Attempt `pip install compileiq==1.0.0` inside the one-off container.
5. Check available CompileIQ search spaces via GitHub Releases API.
6. Audit host `nvcc` / `ptxas` version against search space catalog.

## Findings

### Blocker 1 — Python 3.14 incompatibility

The `vmaf-dev-mcp:local` container ships Python **3.14.4**. CompileIQ v1.0.0 declares
`Requires-Python >=3.11,<3.14`. pip resolves the only available version for Python 3.14
as the placeholder `0.0.0a0` (an empty stub authored by `NVIDIA Kitmaker Team`
that ships no CLI, no `libciq.so`, and no search-space tooling). The real
`1.0.0` wheel (`compileiq-1.0.0-py3-none-manylinux_2_35_x86_64.whl`, 34.2 MB,
shipping `compileiq/core/executable/linux/x86_64/lib/libciq.so`) is excluded by
the Python version constraint.

Host has Python 3.11.15 (compatible) but the task requires the container
environment for all CUDA work per CLAUDE.md §15.

### Blocker 2 — CUDA toolkit version mismatch

CompileIQ's search-space catalog (GitHub release `search-spaces-2026.05.22`)
contains:

```
nvcc13.3_search_space.bin
ptxas13.3_att_search_space.bin
ptxas13.3_search_space.bin
```

All three target **CUDA 13.3**. The container's compiler is **CUDA 13.2.78**
(`nvcc V13.2.78`, built 2026-03-19). No 13.2 or earlier search-space assets
exist in any release. CompileIQ's `NvccSearchSpace(version="13.3")` /
`PtxasSearchSpace(version="13.3")` would invoke `nvcc`/`ptxas` **13.3** flags
against a **13.2** binary; the flag vocabulary may differ and the optimizer
would be working from a mismatch catalog.

The `nvcc --apply-controls <acf>` flag described in some internal NVIDIA
documentation is **not present** in CUDA 13.2.78's `nvcc --help` output.
Confirmation: `docker run --rm --gpus all ... bash -c 'nvcc --help | grep apply-controls'`
returned empty (exit 0 with 0 matches).

### Supporting observations

| Item | Value |
|---|---|
| CompileIQ PyPI version | 1.0.0 (released alongside `v1.0.0` GitHub tag) |
| Wheel size | 34.2 MB (x86_64), includes `libciq.so` |
| Requires-Python | `>=3.11,<3.14` |
| Container Python | 3.14.4 (incompatible) |
| Container nvcc | 13.2.78 (no 13.2 search space in catalog) |
| Host nvcc | 13.2.78 (same) |
| Host Python 3.11 | Available (`python3.11 --version` → 3.11.15) |
| Search spaces available | nvcc 13.3, ptxas 13.3 default, ptxas 13.3 ATT |
| `--apply-controls` in nvcc 13.2 | Not present |
| Docker GPU passthrough | Confirmed working (one-off `--gpus all` tested) |
| `filter1d.cu` LOC | 845 (confirmed) |

## Fitness curve

Not obtained — search never ran due to blockers 1 and 2.

## Correctness verification

Not applicable — no ACF generated.

## Perf delta

Not obtained.

## Recommendation

**Do not proceed with CompileIQ on this toolchain.** Two independent hard blockers
require resolution before retrying:

1. **Update the container base image to ship Python ≤3.13** (3.12 or 3.13 preferred;
   CompileIQ pins `<3.14`). Track this as a container maintenance item.
2. **Update the container CUDA toolkit to 13.3** when NVIDIA releases it and the
   RTX 4090 driver (R610.43.02 or later) supports it. CompileIQ's search space
   catalog is currently 13.3-only; no workaround exists.

Alternatively, if the user can accept running CompileIQ on the host (Python 3.11.15
is available), a host-only pilot can proceed without the container constraint — but
this violates CLAUDE.md §15 for CUDA work and requires explicit user sign-off.

Once both blockers clear: the methodology (objective function → rebuild filter1d.cu
with `-Denable_compileiq=true`, time the Netflix golden pair) is sound. The filter1d
horizontal and vertical smem passes are genuine hot-path candidates and the 17-tap
convolution structure is exactly the type of register-pressure / scheduling problem
CompileIQ targets. Expected search budget: 10 generations × 16 population ≈ 160
evaluations ≈ 20–30 min on RTX 4090.

## References

- CompileIQ PyPI: <https://pypi.org/project/compileiq/>
- CompileIQ GitHub: <https://github.com/NVIDIA/CompileIQ>
- GitHub releases: `v1.0.0`, `search-spaces-2026.05.22`, `booster-packs-2026.05.27`
- Target file: `core/src/feature/cuda/integer_vif/filter1d.cu`
- ADR: [ADR-0739](../adr/0739-cuda-compileiq-filter1d-pilot.md)
- req: user instruction 2026-05-28 ("Pilot NVIDIA's CompileIQ on filter1d.cu (845 LOC)")

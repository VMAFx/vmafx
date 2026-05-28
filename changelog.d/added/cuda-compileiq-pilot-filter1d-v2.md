# CompileIQ pilot v2 — abandoned, investigation complete (ADR-0742)

Investigated automated CUDA kernel auto-tuning via `compileiq` (PyPI) against
`core/src/feature/cuda/integer_vif/filter1d.cu` using the `vmaf-dev-mcp:cuda13.3`
container. The `compileiq` package at v0.0.0a0 is an empty placeholder with no
functional CLI; the pilot was abandoned immediately after confirming this.
Decision recorded in ADR-0742; research digest at
`docs/research/research-0742-cuda-compileiq-filter1d-pilot-v2.md`.
Recommended next step for `filter1d.cu` tuning: `/profile-hotpath cuda vif`
with `ncu` or a grid-search wrapper over nvcc block-size parameters.
Closes PR #66 (CompileIQ pilot v1).

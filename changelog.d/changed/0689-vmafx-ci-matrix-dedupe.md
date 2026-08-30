**CI matrix deduplication (VMAFX Phase 1B, ADR-0689):** Remove five redundant
build rows from the PR matrix — three bare CPU legs subsumed by their DNN
counterparts (`Build — Ubuntu gcc (CPU)`, `Build — Ubuntu clang (CPU)`,
`Build — macOS clang (CPU)`), the advisory macOS MoltenVK Vulkan lane moved to
nightly, and the dynamic-only Ubuntu CUDA leg subsumed by the SYCL+CUDA
combined leg. Drop the duplicate `vulkan-vif-cross-backend` job in
`tests-and-quality-gates.yml` (the `vulkan-parity-matrix-gate` is a strict
superset). Required checks are unchanged. Approximate saving: ~15–25 min of
runner time per PR.

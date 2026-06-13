Remove dead Vulkan/MoltenVK subprojects, CI jobs, and Docker stages (ADR-0726).

Deleted: `core/subprojects/volk.wrap`, `vk-mem-alloc.wrap`, their `packagefiles/`
directories, the `build-vulkan` CI job in `docker-publish-production.yml`, the
`builder-vulkan` + `final-vulkan` Docker stages, the `Install Vulkan SDK` and
`Install MoltenVK` CI steps, the `vulkan-vif-cross-backend` /
`vulkan-parity-matrix-gate` / `vulkan-vif-arc-nightly` job bodies in
`tests-and-quality-gates.yml`, the `vulkan` entry in `smoke-probe-loop.sh`, and
`docs/backends/vulkan/moltenvk.md`.

ABI-reserved enum gaps (`VMAF_FEATURE_EXTRACTOR_VULKAN`, `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE`)
and the ADR audit trail are kept intact.

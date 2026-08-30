- **Research-0085 + ADR-0315: vendor-neutral VVC GPU encode strategy.**
  Source survey ([Research-0085](docs/research/0085-vendor-neutral-vvc-encode-landscape.md))
  of the 2026 H.266 encode landscape across NVENC, AMD AMF, Intel
  QSV, Vulkan Video extensions, HIP / SYCL ports of VVenC, NN-VC, and
  ZLUDA. Decision ([ADR-0315](docs/adr/0315-vendor-neutral-vvc-encode-strategy.md)):
  three-tier rollout — **Tier 1** ships today (document NN-VC as the
  vendor-neutral H.266 GPU story; wire Vulkan scoring through
  `vmaf-tune` per sibling ADR-0314); **Tier 2** queues a HIP port of
  VVenC hot kernels in the backlog with three demand-pull triggers;
  **Tier 3** revisits a `VK_KHR_video_encode_h266` adapter quarterly
  pending Khronos ratification. Pure docs + ADR change; zero code
  modifications.

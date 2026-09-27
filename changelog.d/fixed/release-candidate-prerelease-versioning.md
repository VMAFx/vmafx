- Release candidates after `1.0.0-rc.1` are numbered `1.0.0-rc.2`,
  `1.0.0-rc.3`, and so on (ADR-1348). release-please used its default
  versioning, which turned the first fix after `1.0.0-rc.1` into a proposed
  `1.0.1-rc.1` (release PR #1575); it now uses `prerelease` versioning, and the
  final cut becomes `1.0.0` once `prerelease` is switched off. The release guide
  also no longer claims that every new GHCR package starts private: with the
  organization's public-package setting on, a package first pushed from this
  repository is created public.
- The container quick start works for release candidates: it names the
  release tag instead of `latest` (release candidates are never tagged
  `latest`) and passes `--pixel_format 420` instead of the rejected `yuv420p`.
  The image docs list the `-rocm10` variant and the exact signing identities
  for recovered images, and a recovered image's
  `org.opencontainers.image.revision` label names the tag's source commit
  rather than the recipe commit it was built with.
- The container images carry the built-in models again: the CPU, MCP-server,
  CUDA and oneAPI builders lacked `xxd`, so libvmaf silently embedded no model
  and scoring without `--model` failed. The oneAPI image now installs the
  Unified Memory Framework runtime its SYCL adapters need; without it the image
  found no SYCL device. The publish smoke tests now score with the default model
  and check the oneAPI adapters, and the GPU image docs give working device and
  group flags, forced-backend scoring examples and measured parity figures.

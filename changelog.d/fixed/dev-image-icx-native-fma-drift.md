- **Omit `-march=native` from the reference binary build in `vmaf-dev-mcp`.**
  Intel oneAPI `icx` contracts multiply-accumulate operations in unvectorized CPU
  extractor scalar loops when `-march=native` exposes FMA target capabilities,
  drifting SpEED scores from uncontracted reference builds by up to 7.9e-4 on
  1080p content and 3.38e-7 on the Netflix 576x324 48-frame pair. Removing
  `-Dc_args="-march=native"` restores bit-exact CPU reference parity with
  standard GCC reference builds, following the golden-gate isolation principles
  of [ADR-1317](docs/adr/1317-golden-gate-build-isolation.md).

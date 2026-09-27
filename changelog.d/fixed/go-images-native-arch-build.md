- The `vmafx-operator` and `vmafx-server` release images build each
  architecture on its own native runner, like `vmafx-node` (ADR-1349). Their
  arm64 halves were emulated with QEMU and took 30 to 46 minutes of a 60-minute
  limit; each still publishes one signed, attested multi-arch image.

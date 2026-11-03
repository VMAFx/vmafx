- **Linux arm64 release download and an older glibc floor.** CPU releases now
  include `vmafx-linux-arm64.tar.gz`, with the same CLI, libraries, models,
  notices, Sigstore signing and SBOM coverage as x86-64. Both architectures
  build on digest-pinned manylinux images with glibc 2.28; the release checks
  every staged ELF file's GLIBC symbol versions before publication. Existing
  x86-64 download names stay available
  ([installation](docs/getting-started/index.md#release-download)).

- **Ubuntu development images install packages from one dated archive snapshot
  ([ADR-3541](docs/adr/3541-dev-image-apt-snapshot.md)).** The plain, CUDA and
  SYCL images read `UBUNTU_APT_SNAPSHOT` from `build-config.env`, and Renovate
  proposes a new timestamp weekly for manual review. The certificate bootstrap
  remains the only installation from the live Ubuntu archive; CUDA and oneAPI
  vendor repositories keep their separate version policies.

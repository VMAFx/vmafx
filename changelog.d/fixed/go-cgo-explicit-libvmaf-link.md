- Go cgo builds now require each caller to select a verified VMAFx libvmaf
  explicitly. Local Make/CI builds use `core/build-cpu/src`, container builders
  use their staged fork library, and a missing selection fails at link time
  instead of silently falling through to a distro or stale system libvmaf.

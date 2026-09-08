# Fedora development image

`docker/dev/fedora-40.Dockerfile` is a distro-compatibility build probe. Its
historical filename is stable; the `FROM` instruction identifies the actual
Fedora release. It is separate from the release container and the shared
development service. See [base-image ownership](base-images.md).

Both `ENABLE_CUDA` and `ENABLE_SYCL` default to `false`. With
`--build-arg ENABLE_SYCL=true`, the image writes
`/etc/yum.repos.d/oneAPI.repo`, enables package and repository signature
checks, installs the existing oneAPI/compiler/runtime and Level Zero RPMs,
then cleans DNF metadata. A repository-write or package-install failure
fails the build. The CUDA branch remains independent.

Check Dockerfile syntax without installing SDKs or resolving the base image:

```sh
docker buildx build --builder default --call=targets \
  -f docker/dev/fedora-40.Dockerfile .
```

A complete optional SYCL build is requested with:

```sh
docker buildx build --build-arg ENABLE_SYCL=true \
  -f docker/dev/fedora-40.Dockerfile .
```

The 2026-09-08 repair validates syntax and conditional shell behavior; it
does not establish current Fedora/oneAPI RPM availability or GPU runtime
acceptance. The distro probe still installs unversioned RPM package names,
which Hadolint reports as DL3041. See
[the diagnosis and validation limits](../research/2056-fedora-scorecard-heredoc.md).

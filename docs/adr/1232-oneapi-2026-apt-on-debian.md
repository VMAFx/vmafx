<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1232: oneAPI 2026.1 comes from Intel's apt repo on Debian 13, not from an Intel image

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: build, sycl, ci, security

## Context

The SYCL toolchain was two major releases behind: the builder used
`intel/oneapi-basekit:2025.3.2` and the runtime `intel/oneapi-runtime:2025.3.1`.
The reason it looked current is that `intel/oneapi-basekit` is a **retired
repository** whose newest tag really is 2025.3.2 — Intel continued the toolkit
image under the plain name `intel/oneapi`, which carries 2026.1.0. Any freshness
audit pointed at the old name returns "already latest".

The same retirement happened on the apt side, and there it was actively
harmful. `intel-basekit` and `intel-oneapi-base-toolkit` both stop at 2025.3.2;
only the component packages continue into 2026. `dev/Containerfile` installed
the *unversioned* `intel-basekit` specifically because Intel used to bump it, so
apt kept resolving it to 2025.3.2 with no error and no warning. The dev
container had silently frozen.

Crossing to 2026 is not a pin bump. Three measured constraints shape it, all
recorded in [the research digest](../research/1231-base-image-single-source.md):

1. **The SYCL soname moved.** A binary compiled by 2025.3.2 carries
   `DT_NEEDED libsycl.so.8`; 2026.x ships `libsycl.so.9`. A major soname bump is
   the ABI break, and it overrides the usual "runtime ≥ compiler" ordering rule,
   which only holds while the soname is stable. Compiler and runtime must cross
   together.
2. **Intel's images cannot supply a correct 2026 pair.** `intel/oneapi` is at
   2026.1.0 but `intel/oneapi-runtime` stops at 2026.0.0 — the runtime would be
   *older* than the compiler.
3. **libc.** Intel publishes for Ubuntu only. Ubuntu 26.04 carries glibc 2.43;
   Debian 13, which every other release image ships on, carries 2.41. A binary
   compiled inside Intel's image cannot load on our runtime image.

## Decision

Install both the oneAPI compiler and the oneAPI runtime from Intel's apt
repository, pinned to the same `ONEAPI_VERSION` (2026.1.1-325), onto the Debian
13 base the rest of the release track uses. The dev container installs the same
pinned component package, so development and release share one compiler.

Because Intel's runtime image also bundles the Intel NEO compute driver and
Debian does not package it, the runtime image installs NEO explicitly via the
existing `dev/scripts/fetch-intel-neo.py` (ADR-1145), pinned by
`INTEL_NEO_VERSION`. Without it, `libsycl` loads correctly and every device
query then fails with *"No device of requested type available"* — a silent fall
back off the GPU, which is the worst possible failure mode for this project.

The builder also installs `libze-dev` and `libva-dev`, and the runtime the
matching `libze1` / `libva2` / `libva-drm2` plus an Intel VA driver. Intel's
oneAPI apt repo ships neither Level Zero nor VA-API, and Intel's basekit image
— which this stage replaces — carried `ze_loader` but **not** libva. So every
published `-oneapi` image to date had `HAVE_SYCL_DMABUF` undefined and compiled
the zero-copy DMA-BUF import path out to its stub, while the dev container
(which does install `libva-dev`) built it in. The release image was quietly
less capable than the container it was developed in. A configure-time assertion
now fails the build if either dependency goes missing again.

The published tag becomes `-oneapi2026`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **apt, same version both sides, on Debian 13** (chosen) | One version string; compiler == runtime; same libc and distro as every other release image; reaches 2026.1.1, the newest published | Longer image build (apt install rather than a cached vendor layer); Debian is not Intel's officially supported distro | Only option satisfying the soname, ordering *and* libc constraints simultaneously; verified end to end |
| `intel/oneapi:2026.1.0` builder + `intel/oneapi-runtime:2026.0.0` | Both cached vendor images | Runtime *older* than compiler, the ordering violation being removed; Ubuntu-only, so glibc 2.43 binaries on a 2.41 runtime | Fails two of the three constraints |
| Keep 2025.3.2 builder + 2025.3.1 runtime | No work | Two releases stale; retains the compile-2025.3.2 / run-2025.3.1 patch skew; keeps the dev container frozen | The status quo being fixed |
| Move the whole release track to Ubuntu 26.04 | Intel's images work as shipped | Rebases every release image, not just SYCL, onto a different distro for one backend's convenience | Disproportionate; ADR-1231 deliberately unified the track on Debian 13 |
| Ship the runtime without NEO | Smaller image | SYCL loads and finds no device — fails at runtime, on the user's machine, silently | Unacceptable failure mode |

## Consequences

- **Positive**: the SYCL toolchain is current (2026.1.1) and the compiler and
  runtime are identical rather than merely compatible. Dev and release share one
  compiler, and the container fails its build if `icpx` is not the pinned 2026
  release, so a silent freeze cannot recur.
- **Positive**: the oneAPI runtime image drops from Intel's ~depth *devel* image
  to Debian 13 slim plus the runtime packages, and no release image is on Ubuntu
  24.04 any more.
- **Positive**: the published oneAPI image gains zero-copy DMA-BUF import,
  which it never actually had. `meson` now reports
  `SYCL DMA-BUF import: enabled (VA-API found)` for the release build, matching
  the dev container.
- **Positive**: the GitHub token for the NEO release lookup moved from an `ARG`
  to a BuildKit secret in both the production and dev images, so it no longer
  lands in image metadata.
- **Negative**: image builds install oneAPI from apt instead of inheriting a
  cached vendor layer, which costs build time on a cold cache. BuildKit layer
  caching and the GHA cache absorb the repeat cost.
- **Negative**: Debian is not an Intel-supported distro for oneAPI. Verified
  working; if Intel ever breaks it, the fallback is Ubuntu 26.04 for both the
  SYCL builder and its runtime, accepting a distro split for that one backend.
- **Neutral / follow-ups**: the published tag renames `-oneapi2025` →
  `-oneapi2026`. `docker/dev/ubuntu-26.04-sycl.Dockerfile` moves to
  `intel/oneapi:2026.1.0-devel-ubuntu26.04` and now actually builds on 26.04, as
  its filename always claimed.

## Supply-chain impact

- **New dependencies**: `intel-oneapi-compiler-dpcpp-cpp` and
  `intel-oneapi-runtime-dpcpp-cpp`, both pinned to `2026.1.1-325` (build /
  runtime, Intel EULA, `https://apt.repos.intel.com/oneapi`). These replace the
  equivalent components previously delivered inside Intel's images.
- **Removed dependencies**: `intel/oneapi-basekit`, `intel/oneapi-runtime` and
  the retired `intel-basekit` meta-package.
- **Build-time fetches**: the Intel apt repo, verified against Intel's GPG key,
  and the NEO `.deb` set resolved from a pinned `intel/compute-runtime` release
  tag with checksum verification (unchanged mechanism, ADR-1145). Package
  versions are exact-pinned; apt repos are not digest-addressable, which is a
  weaker guarantee than the image digests they replace and is the cost of
  reaching 2026 at all.
- **CVE surface delta**: narrows. The runtime image was Intel's *devel* image
  (full toolchain, Ubuntu 24.04); it is now Debian 13 slim plus the runtime
  libraries and the GPU driver.

<!-- markdownlint-disable MD013 MD041 -->

# Research digest — base-image single source, and what the oneAPI 2026 move actually requires

Supports [ADR-1231](../adr/1231-base-image-single-source.md). Everything below
was measured on 2026-09-07, not recalled.

## 1. The drift that motivated the config

`git grep` for `FROM` across the tree found 25 pinned bases across 10
Dockerfiles, resolving to 10 distinct images. Concretely stale:

| Where | Pinned | Rest of tree |
| --- | --- | --- |
| `Dockerfile.controller:36`, `Dockerfile.operator:48` | `golang:1.27-bookworm` (Debian 12) | `golang:1.27-trixie` (Debian 13) |
| `Dockerfile.controller:108` | `distroless/cc-debian12` | `distroless/cc-debian13:nonroot` |
| `Dockerfile.operator:80` | `distroless/static-debian12` | `distroless/static-debian13:nonroot` |
| `Dockerfile.production-gpu:82,278` | `nvidia/cuda:13.3.1-*-ubuntu24.04` | `…-ubuntu26.04` in the root `Dockerfile` |

Both `Dockerfile.controller` and `Dockerfile.operator` also quoted a golang
digest in their header comments (`ded31c68…`) that did not match the digest in
their own `FROM` line (`648f440f…`).

### The pins that were not `FROM` lines

Four more pins hid inside `COPY --from=<image>`, which no `FROM`-oriented
search finds:

- `dev/Containerfile:1040` — Go toolchain from `golang:1.27-bookworm`
- `docker/Dockerfile.node:338` — `nvidia/cuda:13.3.1-runtime-ubuntu24.04`
- `docker/Dockerfile.node:361,362` — `rocm/dev-ubuntu-24.04:7.2.4`
- `docker/Dockerfile.node:392,393` — `intel/oneapi-runtime:2025.3.1-…-ubuntu24.04`

These were the *most* stale pins in the repository, which is the argument for
the gate covering `COPY --from` and for converting them to named stages.

## 2. oneAPI: why the follow-up is a restructure, not a pin bump

### 2.1 The image repository was renamed

`intel/oneapi-basekit` is retired; its newest tag is `2025.3.2-0-devel-*`
(2026-04-09). Intel continued the toolkit image under the plain name
**`intel/oneapi`**, which carries `2026.0.0` and `2026.1.0`, including an
`ubuntu26.04` variant (2026-07-06). Intel's apt repository is further ahead
still: `intel-oneapi-compiler-dpcpp-cpp` and `intel-oneapi-runtime-dpcpp-cpp`
are both at **2026.1.1-325**.

Watching the retired name makes the toolchain look two years stale when it is
not. Any freshness audit of this repo must query `intel/oneapi`.

### 2.2 The SYCL soname moved, so both sides must cross together

Measured by compiling a trivial SYCL program and reading `DT_NEEDED`, and by
listing the runtime images:

| Component | libsycl |
| --- | --- |
| compiled by basekit 2025.3.2 | `DT_NEEDED libsycl.so.8` |
| runtime 2025.3.1 | ships `libsycl.so.8` |
| runtime 2026.0.0 | ships `libsycl.so.9` |
| compiled by 2026.1.1 | `DT_NEEDED libsycl.so.9` |

A major soname bump *is* the ABI break. The usual "runtime ≥ compiler" ordering
rule only holds while the soname is stable, so a 2025-compiled binary cannot
load against a 2026 runtime at all. Compiler and runtime must move together.

### 2.3 Intel's images cannot supply a correct 2026 pair

Two independent blockers:

1. **Version ordering.** `intel/oneapi` ships 2026.1.0 but
   `intel/oneapi-runtime` stops at 2026.0.0. Using both would put the runtime
   *behind* the compiler — the exact violation the pin is meant to remove.
2. **libc.** Intel publishes for Ubuntu only. Ubuntu 26.04 carries
   **glibc 2.43**; Debian 13 carries **glibc 2.41** (both measured). A binary
   compiled in Intel's image cannot load on the Debian 13 runtime the rest of
   the release track ships.

### 2.4 Intel's apt repo on Debian 13 does work

Verified end to end with a two-stage build — compile in `debian:13-slim` with
`intel-oneapi-compiler-dpcpp-cpp=2026.1.1-325`, run in a separate
`debian:13-slim` with only `intel-oneapi-runtime-dpcpp-cpp=2026.1.1-325`:

```text
icpx      : Intel(R) oneAPI DPC++/C++ Compiler 2026.1.1 (2026.1.1.20260724)
DT_NEEDED : libsycl.so.9
ldd       : libsycl.so.9 => /opt/intel/oneapi/redist/lib/libsycl.so.9   ✓ resolves
```

Same version on both sides, same libc, same distro as the rest of the release
track. This is the shape the follow-up should take.

### 2.5 The catch the follow-up must handle: the GPU driver

The runtime binary loads, but a SYCL device query in that image fails:

```text
No device of requested type available
```

Intel's own `oneapi-runtime` image ships the Intel compute runtime —
`libze_intel_gpu.so.1` plus `/etc/OpenCL/vendors/intel*.icd` — and Debian does
not package it. So moving the oneAPI runtime to Debian 13 **loses GPU device
enablement** unless the NEO driver is installed alongside.

The fork already solves exactly this for the dev container:
`dev/scripts/fetch-intel-neo.py` with `NEO_VER=26.31.39395.13` (ADR-1145). The
follow-up should reuse it.

**This is why the oneAPI migration is not in ADR-1231's PR.** Folding it in
would have shipped a GPU image that cannot see the GPU.

## 3. What the follow-ups are

| Item | Owner | Target |
| --- | --- | --- |
| ROCm 7.2.4 → 10.0.0 | PR #1386 (carries the HIP changes) | `rocm/dev-ubuntu-26.04:10.0.0-full` |
| oneAPI 2025 → 2026.1.1 | immediate follow-up to ADR-1231 | Intel apt on Debian 13 + NEO via `fetch-intel-neo.py` |

Both keep an explicit, self-closing Ubuntu 24.04 exemption in
`scripts/ci/check-base-image-single-source.sh` until they land.

## Reproducing

```bash
# soname a compiler emits
docker run --rm -v "$PWD:/w" -w /w \
  intel/oneapi-basekit:2025.3.2-0-devel-ubuntu24.04 \
  bash -lc 'source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1;
            icpx -fsycl sycl_hello.cpp -o h && readelf -d h | grep -i sycl'

# soname a runtime ships
docker run --rm intel/oneapi-runtime:2026.0.0-devel-ubuntu24.04 \
  ls /opt/intel/oneapi/redist/lib/libsycl.so.*

# libc generations
docker run --rm debian:13-slim ldd --version | head -1
docker run --rm ubuntu:26.04    ldd --version | head -1
```

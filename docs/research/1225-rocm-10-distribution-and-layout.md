<!-- markdownlint-disable MD013 -->
# Research: how ROCm 10 is distributed, and what it changed under `/opt/rocm`

**Date**: 2026-09-07
**Author**: ADR-1225 implementation pass

## Summary

The fork's HIP backend was pinned to ROCm 7.2.3 / 7.2.4 and installed from
AMD's apt channel everywhere. Bumping to ROCm 10.0.0 turned out not to be a
version-string edit, because AMD changed both **how ROCm is distributed** and
**how it is laid out on disk**. This digest records what was measured, so the
next person to bump ROCm does not have to rediscover it.

## 1. There is no apt channel for ROCm >= 7.14

Since ROCm 7.14, AMD builds and releases through
[TheRock](https://github.com/ROCm/TheRock). The legacy channels are frozen.
Measured 2026-09-07:

| Channel | Result |
| --- | --- |
| `repo.radeon.com/rocm/apt/latest` | resolves to **7.2.4** |
| `repo.radeon.com/rocm/apt/7.14` | HTTP 404 |
| `repo.radeon.com/rocm/apt/10.0.0` | HTTP 404 |
| `repo.radeon.com/rocm/manylinux/` | tops out at `rocm-rel-7.2.4` |
| TheRock nightly wheel index | only `7.14.0a2026061x` alphas |
| PyPI `rocm-sdk-core` | 0.1.0 placeholder |
| `hub.docker.com/r/rocm/dev-ubuntu-24.04` | `10.0.0-full` (8.22 GB), published 2026-08-26 |

`10.0.0-full` is the **only** 10.0.0 tag — there is no slim `10.0.0` runtime
variant, unlike the 6.x/7.x lines which shipped both a ~1 GB base and a
~5-7 GB `-complete`.

**Consequence**: the container image is the only stable, digest-pinnable ROCm
10.0.0 artifact, and any install path has to start from it.

## 2. The on-disk layout moved behind Debian alternatives

ROCm 10 installs the real tree at `/opt/rocm/core-10.0/` and makes
`/opt/rocm/{bin,lib,include,llvm,share,libexec,amdgcn}` symlinks into
`/etc/alternatives/rocm-*`, which point back into that tree:

```text
/opt/rocm/lib -> /etc/alternatives/rocm-lib -> /opt/rocm/core-10.0/lib
```

Two practical consequences:

- `ROCM_PATH=/opt/rocm` and `LD_LIBRARY_PATH=/opt/rocm/lib` keep working
  unchanged **inside** the image.
- Any extraction of `/opt/rocm` alone — a `COPY --from=<image> /opt/rocm`, or
  a tar of that prefix — lands a directory of **dangling symlinks**, because
  `/etc/alternatives` does not come with it. Copying `/etc/alternatives`
  instead is not an option: it is shared with every other package on the
  consuming image. Both the `rocm-src` container stage and
  `scripts/ci/install-rocm-from-image.sh` therefore repoint each link at
  `core-10.0/<name>` relative to `/opt/rocm`, making the tree self-contained.

Note also that the image self-reports **HIP 7.15.26333** (`hipconfig
--version`). The HIP component version is not the ROCm release version;
do not use it to identify the release.

## 3. `libamdhip64.so`'s link closure widened

Under ROCm 7 the fork's `node-rocm` runtime image copied exactly two
libraries: `libamdhip64.so*` and `libhsa-runtime64.so*`. Under ROCm 10 that
set no longer loads. Measured closure of `ldd /opt/rocm/lib/libamdhip64.so`,
transitively, inside the image:

```text
19 libraries, 234 MB total
  127M  libLLVM.so.23.0git
   86M  libclang-cpp.so.23.0git
   13M  libamd_comgr.so.3
  4.7M  libhsa-runtime64.so.1
  1.2M  librocm_sysdeps_elf.so.1
  948K  librocprofiler-register.so.0
  … plus the rest of the vendored rocm_sysdeps bundle
    (zlib, zstd, lzma, bz2, drm, drm_amdgpu, numa) and librocm_kpack
```

They resolve each other through `$ORIGIN`-relative RPATHs —
`$ORIGIN`, `$ORIGIN/llvm/lib`, `$ORIGIN/rocm_sysdeps/lib` — so the copy has
to preserve directory structure rather than flattening into
`/usr/local/lib`.

`librocprofiler-register` deserves a specific callout: despite the name it is
not a profiler add-on but a hard dependency of both `libamdhip64.so` and
`libhsa-runtime64.so`. An early prune pass in this work removed it with a
`librocprof*` glob; every HIP binary then failed at load with
`librocprofiler-register.so.0: cannot open shared object file`. The prune
list uses `librocprof-sys*` / `librocprofiler-sdk*` instead.

## 4. What can be pruned, and what it costs

`10.0.0-full` carries 19 GB under `/opt/rocm`. libvmaf's HIP backend links
none of the math libraries. Pruning hipBLASLt, hipSPARSELt, rocBLAS, MIOpen
(including `share/miopen`), Composable Kernel's `libdevice_*_operations.a`
archives, RCCL, rocFFT, rocSPARSE, rocSOLVER, rocRAND, rocRoller, rocJITSU,
`librocshmem.a`, RDC and the profiler runtimes takes it to **5.5 GB** while
`hipcc --offload-arch=gfx1036` still produces a runnable binary. `lib/llvm`
(2.8 GB) has to stay — hipcc needs it to emit `.hsaco`.

The prune list is gated by a hipcc smoke compile in the same stage, so an
over-wide glob fails the build instead of shipping a broken image.

## 5. `gfx1036` no longer needs `HSA_OVERRIDE_GFX_VERSION`

The fork's dev host (AMD Raphael APU iGPU, `gfx1036`) was not on the ROCm
6.x/7.x supported-GPU allowlist, so `dev/docker-compose.yml` set
`HSA_OVERRIDE_GFX_VERSION=10.3.0` to alias it onto the allowlisted
`gfx1030`. Measured under ROCm 10.0.0 on Linux 7.2.3-1-cachyos with no
override set:

```text
rocminfo                      → Agent 2  Name: gfx1036
amdgpu-arch                   → gfx1036
hipcc --offload-arch=gfx1036  → compiles; kernel dispatches (h[63] = 63)
meson test (HIP suite)        → Ok: 19, Expected Fail: 4, Fail: 0
end-to-end                    → hip vmaf = 45.315104 / cpu vmaf = 45.315104
```

Keeping the override would now be a regression, not a safety net: it maps the
agent to `gfx1030` while meson compiles `gfx1036` code objects for the arch
`rocm_agent_enumerator` reports.

## 6. Why CI streams the image instead of pulling it

The HIP CI leg shares its runner with the CUDA toolkit and oneAPI. A
`docker pull` of `10.0.0-full` costs 8.2 GB of download and 29 GB of
`/var/lib/docker`, plus another 5.5 GB for the `docker cp` — a disk-full
mid-job, not a clean error. `scripts/ci/install-rocm-from-image.sh` instead
does anonymous registry auth, reads the digest-addressed manifest, and pipes
each layer blob through `gzip -dc | tar -x` with the prune list as
`--exclude` patterns, so nothing is ever stored whole: peak disk is the
5.5 GB result.

Two things to know about that path:

- The Docker Hub token this returns carries `pull_limit: 100` over a
  21600-second window, per anonymous IP. Hosted runners share IPs, so the
  script retries with backoff (`--retry 5 --retry-delay 10
  --retry-all-errors`). If this proves flaky in practice, the fallback is to
  mirror the pruned tree to `ghcr.io/vmafx/rocm-toolchain` and pull that
  instead — the extractor is the same either way.
- Layers apply in order and later layers win. Overlay whiteout markers
  (`.wh.*`) are deleted after extraction rather than interpreted; ROCm is
  installed by a single layer that never deletes a path a later layer needs.

## Reproducing the measurements

```bash
# Channel availability
curl -sI https://repo.radeon.com/rocm/apt/10.0.0/ | head -1     # 404
curl -s https://hub.docker.com/v2/repositories/rocm/dev-ubuntu-24.04/tags/?name=10.0

# Layout + closure, inside the image
docker run --rm --entrypoint bash \
  rocm/dev-ubuntu-24.04:10.0.0-full@sha256:a90cf047f615abe70fbef83c64def0a2d549ef37a39c8ea545430aba4981b374 \
  -c 'ls -la /opt/rocm; ldd /opt/rocm/lib/libamdhip64.so'

# Extraction, on any Linux host with curl + jq + tar
scripts/ci/install-rocm-from-image.sh \
  --image rocm/dev-ubuntu-24.04@sha256:a90cf047f615abe70fbef83c64def0a2d549ef37a39c8ea545430aba4981b374 \
  --dest /tmp/rocm10
ROCM_PATH=/tmp/rocm10 LD_LIBRARY_PATH=/tmp/rocm10/lib \
  /tmp/rocm10/bin/hipcc --offload-arch=gfx1036 smoke.hip -o /tmp/smoke && /tmp/smoke
```

## References

- ROCm release notes — <https://rocm.docs.amd.com/en/latest/about/release-notes.html>
- TheRock — <https://github.com/ROCm/TheRock>
- [ADR-1225](../adr/1225-rocm-10-therock-migration.md)
